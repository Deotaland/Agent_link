#include "co5300_qspi_io.h"

#include <limits.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "co5300.io";
#define IO_TIMEOUT_MS 100

typedef struct {
    esp_lcd_panel_io_t base;
    spi_device_handle_t device;
    SemaphoreHandle_t mutex;
    // Both descriptor and DMA buffer outlive a timed-out call. In particular,
    // never queue a stack descriptor or lend SPI an LVGL/command-stack buffer.
    spi_transaction_t transaction;
    uint8_t *dma;
    size_t capacity;
    uint32_t sequence;
    bool pending;
    esp_err_t fault;
} co5300_io_t;

static esp_err_t reap(co5300_io_t *io, TickType_t wait)
{
    if (!io->pending) return ESP_OK;
    spi_transaction_t *finished = NULL;
    esp_err_t err = spi_device_get_trans_result(io->device, &finished, wait);
    // DMA errors also return a completed descriptor. A timeout returns none.
    if (finished == &io->transaction) io->pending = false;
    if (err == ESP_OK && io->pending) return ESP_ERR_INVALID_STATE;
    return err;
}

static esp_err_t transmit(esp_lcd_panel_io_t *base, int cmd, const void *data,
                          size_t bytes, bool color)
{
    co5300_io_t *io = (co5300_io_t *)base;
    // This internal adapter accepts framed CO5300 writes, not raw continuation
    // calls. Each pixel stripe gets its own complete CASET/RASET/RAMWR window.
    const uint32_t opcode = (uint32_t)cmd >> 24;
    if ((color ? opcode != 0x32 : opcode != 0x02) ||
        bytes > io->capacity || (bytes && !data)) return ESP_ERR_INVALID_ARG;
    const TickType_t wait = pdMS_TO_TICKS(IO_TIMEOUT_MS) ? pdMS_TO_TICKS(IO_TIMEOUT_MS) : 1;
    if (xSemaphoreTake(io->mutex, wait) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = io->fault;
    if (err != ESP_OK) goto out;
    if (io->pending) { err = ESP_ERR_INVALID_STATE; goto fail; }

    if (bytes) memcpy(io->dma, data, bytes);
    memset(&io->transaction, 0, sizeof io->transaction);
    io->transaction.cmd = (uint16_t)opcode;
    io->transaction.addr = (uint32_t)cmd & 0xFFFFFF;
    io->transaction.flags = color ? SPI_TRANS_MODE_QIO : 0;
    io->transaction.length = bytes * 8;
    io->transaction.tx_buffer = bytes ? io->dma : NULL;
    ++io->sequence;
    err = spi_device_queue_trans(io->device, &io->transaction, wait);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "queue failed: seq=%lu cmd=%08lx bytes=%u error=%s",
                 (unsigned long)io->sequence, (unsigned long)(uint32_t)cmd,
                 (unsigned)bytes, esp_err_to_name(err));
        goto fail;
    }
    io->pending = true;
    err = reap(io, wait);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "completion failed: seq=%lu cmd=%08lx bytes=%u pending=%d error=%s",
                 (unsigned long)io->sequence, (unsigned long)(uint32_t)cmd,
                 (unsigned)bytes, io->pending, esp_err_to_name(err));
        goto fail;
    }
    goto out;
fail:
    io->fault = err;
out:
    xSemaphoreGive(io->mutex);
    return err;
}

static esp_err_t tx_param(esp_lcd_panel_io_t *io, int cmd, const void *data, size_t bytes)
{
    return transmit(io, cmd, data, bytes, false);
}

static esp_err_t tx_color(esp_lcd_panel_io_t *io, int cmd, const void *data, size_t bytes)
{
    return transmit(io, cmd, data, bytes, true);
}

static esp_err_t rx_param(esp_lcd_panel_io_t *io, int cmd, void *data, size_t bytes)
{
    (void)io; (void)cmd; (void)data; (void)bytes;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t callbacks(esp_lcd_panel_io_t *io, const esp_lcd_panel_io_callbacks_t *cbs, void *ctx)
{
    (void)io; (void)cbs; (void)ctx;
    // Completion is synchronous. Do not invoke ISR callbacks from task context.
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t delete_io(esp_lcd_panel_io_t *base)
{
    co5300_io_t *io = (co5300_io_t *)base;
    if (xSemaphoreTake(io->mutex, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    (void)reap(io, 0);
    esp_err_t err = io->pending ? ESP_ERR_TIMEOUT : spi_bus_remove_device(io->device);
    if (err != ESP_OK) { xSemaphoreGive(io->mutex); return err; }
    xSemaphoreGive(io->mutex);
    vSemaphoreDelete(io->mutex);
    heap_caps_free(io->dma);
    heap_caps_free(io);
    return ESP_OK;
}

esp_err_t co5300_new_qspi_io(spi_host_device_t host, int cs, uint32_t clock_hz,
                            size_t max_bytes, esp_lcd_panel_io_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (cs < 0 || !clock_hz || clock_hz > INT_MAX || !max_bytes || max_bytes > INT_MAX / 8)
        return ESP_ERR_INVALID_ARG;
    co5300_io_t *io = heap_caps_calloc(1, sizeof *io, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!io) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    io->mutex = xSemaphoreCreateMutex();
    if (!io->mutex) { err = ESP_ERR_NO_MEM; goto fail; }
    const spi_device_interface_config_t config = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,
        .clock_speed_hz = (int)clock_hz,
        .spics_io_num = cs,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 1,
    };
    err = spi_bus_add_device(host, &config, &io->device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add SPI device failed: %s", esp_err_to_name(err));
        goto fail;
    }
    // IDF 5.5 creates the SPI master context lazily in add_device(). Querying
    // its transfer limit immediately after bus_initialize() fails on cold boot.
    size_t bus_max = 0;
    err = spi_bus_get_max_transaction_len(host, &bus_max);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "query SPI transfer limit failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (max_bytes > bus_max) { err = ESP_ERR_INVALID_SIZE; goto fail; }
    io->dma = heap_caps_aligned_alloc(4, max_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!io->dma) { err = ESP_ERR_NO_MEM; goto fail; }
    io->capacity = max_bytes;
    io->base.tx_param = tx_param; io->base.tx_color = tx_color;
    io->base.rx_param = rx_param; io->base.del = delete_io;
    io->base.register_event_callbacks = callbacks;
    *out = &io->base;
    ESP_LOGI(TAG, "QSPI single transactions, %dms queue/completion limits, %uB owned DMA buffer",
             IO_TIMEOUT_MS, (unsigned)max_bytes);
    return ESP_OK;
fail:
    // No transaction has been queued during construction, so a newly added
    // device can be removed without waiting for DMA, including query/size errors.
    if (io->device) (void)spi_bus_remove_device(io->device);
    if (io->mutex) vSemaphoreDelete(io->mutex);
    heap_caps_free(io->dma); heap_caps_free(io);
    return err;
}

esp_err_t co5300_qspi_io_recover(esp_lcd_panel_io_handle_t base, bool *recovered)
{
    if (!base || !recovered) return ESP_ERR_INVALID_ARG;
    *recovered = false;
    co5300_io_t *io = (co5300_io_t *)base;
    if (xSemaphoreTake(io->mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (io->fault != ESP_OK) {
        err = reap(io, 0);
        if (!io->pending) {
            // SPI has returned ownership even if DMA reported an error. Reset
            // the glass before drawing again; do not retry an incomplete window.
            io->fault = ESP_OK; *recovered = true; err = ESP_OK;
        }
    }
    xSemaphoreGive(io->mutex);
    return err;
}
