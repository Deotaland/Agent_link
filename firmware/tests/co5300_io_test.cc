#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "co5300_qspi_io.h"
#include "co5300_panel.h"
#include "co5300_hal.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"

struct fake_spi_device {} device;
struct fake_mutex { bool held = false; };
struct Write { unsigned cmd, addr, flags; std::vector<uint8_t> data; };
std::vector<Write> writes;
spi_transaction_t* pending = nullptr;
bool block_completion = false, reject_lock = false;
bool driver_initialized = false;
int queue_error = ESP_OK, completion_error = ESP_OK, add_error = ESP_OK;
int max_query_error = ESP_OK;
int allocations = 0, devices = 0, submitted = 0, resets = 0, initializations = 0;
unsigned waited = 0;
int allocation_fail_after = -1;

void* heap_caps_malloc(size_t bytes, unsigned) {
    if (allocation_fail_after == 0) return nullptr;
    if (allocation_fail_after > 0) --allocation_fail_after;
    void* p = malloc(bytes); if (p) ++allocations; return p;
}
void* heap_caps_calloc(size_t count, size_t bytes, unsigned caps) {
    void* p = heap_caps_malloc(count * bytes, caps); if(p) memset(p, 0, count * bytes); return p;
}
void* heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps) {
    assert(alignment == 4 && (caps & MALLOC_CAP_DMA)); return heap_caps_malloc(bytes, caps);
}
void heap_caps_free(void* p) { if(p) { --allocations; free(p); } }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new fake_mutex; }
int xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    assert(wait <= 100); if (reject_lock || m->held) return 0; m->held = true; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t m) { assert(m->held); m->held = false; return 1; }
void vSemaphoreDelete(SemaphoreHandle_t m) { assert(!m->held); delete m; }
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t, size_t* bytes) {
    // IDF 5.5 initializes the SPI master context lazily in add_device(), not
    // in bus_initialize(). A fresh boot must not query it before adding a device.
    if (!driver_initialized) return ESP_ERR_INVALID_ARG;
    if (max_query_error) return max_query_error;
    *bytes = 12000; return ESP_OK;
}
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t* config, spi_device_handle_t* out) {
    assert(config->command_bits == 8 && config->address_bits == 24 && config->mode == 0);
    assert(config->flags == SPI_DEVICE_HALFDUPLEX && config->queue_size == 1);
    driver_initialized = true;
    if (add_error) return add_error;
    *out = &device; ++devices; return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t) { assert(!pending); --devices; return ESP_OK; }
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t* trans, TickType_t wait) {
    assert(wait > 0 && wait <= 100 && !pending);
    if (queue_error) return queue_error;
    ++submitted; pending = trans;
    Write write{trans->cmd, static_cast<unsigned>(trans->addr), trans->flags, {}};
    if (trans->length) {
        auto* data = static_cast<const uint8_t*>(trans->tx_buffer);
        write.data.assign(data, data + trans->length / 8);
    }
    writes.push_back(write);
    return ESP_OK;
}
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t** out, TickType_t wait) {
    assert(wait <= 100 && pending); waited += wait;
    if (block_completion) return ESP_ERR_TIMEOUT;
    *out = pending; pending = nullptr;
    const int error = completion_error; completion_error = ESP_OK;
    return error;
}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void* p, size_t size) {
    return io->tx_param(io, cmd, p, size);
}
esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io) { return io->del(io); }

struct fake_panel { esp_lcd_panel_io_handle_t io; int xgap=0, ygap=0; };
esp_err_t co5300_hal_init(int, int, co5300_hal_handles_t*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t co5300_hal_init_qspi(const co5300_qspi_config_t* config, co5300_hal_handles_t* out) {
    esp_lcd_panel_io_handle_t io;
    int err = co5300_new_qspi_io(config->spi_host, config->pin_cs, config->pclk_hz, config->max_transfer_bytes, &io);
    if (err) return err;
    *out = {}; out->io = io; out->panel = new fake_panel{io, config->x_gap, config->y_gap}; return ESP_OK;
}
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel) { delete panel; return ESP_OK; }
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x, int y, int xend, int yend, const void* data) {
    const size_t bytes = static_cast<size_t>(xend-x) * (yend-y) * 2;
    x += p->xgap; xend += p->xgap; y += p->ygap; yend += p->ygap;
    const uint8_t xs[]{uint8_t(x>>8), uint8_t(x), uint8_t((xend-1)>>8), uint8_t(xend-1)};
    const uint8_t ys[]{uint8_t(y>>8), uint8_t(y), uint8_t((yend-1)>>8), uint8_t(yend-1)};
    int err = p->io->tx_param(p->io, 0x02002A00, xs, 4);
    if (!err) err = p->io->tx_param(p->io, 0x02002B00, ys, 4);
    if (!err) err = p->io->tx_color(p->io, 0x32002C00, data, bytes);
    return err;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t p, bool on) {
    return p->io->tx_param(p->io, on ? 0x02002900 : 0x02002800, nullptr, 0);
}
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t) { ++resets; return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t p) {
    ++initializations; return p->io->tx_param(p->io, 0x02001100, nullptr, 0);
}
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t p, int x, int y) { p->xgap=x; p->ygap=y; return ESP_OK; }

int main() {
    esp_lcd_panel_io_handle_t io = nullptr;
    size_t max_bytes = 0;
    assert(spi_bus_get_max_transaction_len(SPI2_HOST,&max_bytes)==ESP_ERR_INVALID_ARG);
    assert(co5300_new_qspi_io(SPI2_HOST,12,40000000,16,&io)==ESP_OK && io);
    assert(io->del(io)==ESP_OK && allocations==0 && devices==0);
    driver_initialized=false;
    assert(co5300_new_qspi_io(SPI2_HOST, 12, 40000000, 12001, &io) == ESP_ERR_INVALID_SIZE && !io);
    assert(devices==0 && allocations==0);
    max_query_error=ESP_FAIL; driver_initialized=false;
    assert(co5300_new_qspi_io(SPI2_HOST,12,40000000,16,&io)==ESP_FAIL && !io);
    assert(devices==0 && allocations==0);
    max_query_error=ESP_OK;
    for(int n=0; n<2; ++n) {
        allocation_fail_after=n;
        assert(co5300_new_qspi_io(SPI2_HOST,12,40000000,16,&io)==ESP_ERR_NO_MEM && !io && allocations==0 && devices==0);
    }
    allocation_fail_after=-1; add_error=ESP_FAIL;
    assert(co5300_new_qspi_io(SPI2_HOST,12,40000000,16,&io)==ESP_FAIL && !io && allocations==0);
    add_error=ESP_OK;
    assert(co5300_new_qspi_io(SPI2_HOST,12,40000000,16,&io)==ESP_OK);
    uint8_t param[]{0,6,1,0xD7};
    assert(io->tx_param(io,0x02002A00,param,4)==ESP_OK);
    assert(writes.back().cmd==2 && writes.back().addr==0x2A00 && writes.back().flags==0);
    assert(writes.back().data==std::vector<uint8_t>({0,6,1,0xD7}));
    assert(io->tx_param(io,0x02001100,nullptr,0)==ESP_OK && writes.back().data.empty());
    assert(io->tx_color(io,0x32002C00,param,4)==ESP_OK && writes.back().flags==SPI_TRANS_MODE_QIO);
    assert(io->tx_color(io,-1,param,4)==ESP_ERR_INVALID_ARG);
    assert(io->tx_color(io,0x32002C00,param,17)==ESP_ERR_INVALID_ARG);
    assert(io->tx_param(io,0x02002A00,nullptr,4)==ESP_ERR_INVALID_ARG);
    bool recovered=true;
    assert(co5300_qspi_io_recover(io,&recovered)==ESP_OK && !recovered);
    reject_lock=true;
    assert(io->tx_param(io,0x02005100,param,1)==ESP_ERR_TIMEOUT);
    reject_lock=false;
    block_completion=true;
    assert(io->tx_param(io,0x02002A00,param,4)==ESP_ERR_TIMEOUT);
    assert(pending && pending->tx_buffer!=param);
    const auto descriptor=*pending;
    const int queued=submitted;
    param[1]=99;
    assert(static_cast<const uint8_t*>(pending->tx_buffer)[1]==6);
    assert(io->tx_color(io,0x32002C00,param,4)==ESP_ERR_TIMEOUT && submitted==queued);
    assert(co5300_qspi_io_recover(io,&recovered)==ESP_ERR_TIMEOUT && !recovered);
    assert(descriptor.cmd==pending->cmd && descriptor.addr==pending->addr &&
           descriptor.length==pending->length && descriptor.tx_buffer==pending->tx_buffer);
    assert(io->del(io)==ESP_ERR_TIMEOUT && allocations==2 && devices==1);
    block_completion=false;
    assert(co5300_qspi_io_recover(io,&recovered)==ESP_OK && recovered && !pending);
    queue_error=ESP_ERR_TIMEOUT;
    assert(io->tx_param(io,0x02005100,param,1)==ESP_ERR_TIMEOUT && !pending);
    queue_error=ESP_OK;
    assert(co5300_qspi_io_recover(io,&recovered)==ESP_OK && recovered);
    completion_error=ESP_ERR_INVALID_STATE;
    assert(io->tx_color(io,0x32002C00,param,4)==ESP_ERR_INVALID_STATE && !pending);
    assert(co5300_qspi_io_recover(io,&recovered)==ESP_OK && recovered);
    assert(io->tx_color(io,0x32002C00,param,4)==ESP_OK);
    block_completion=true;
    assert(io->tx_color(io,0x32002C00,param,4)==ESP_ERR_TIMEOUT && pending);
    param[1]=77;
    assert(static_cast<const uint8_t*>(pending->tx_buffer)[1]==99);
    assert(io->del(io)==ESP_ERR_TIMEOUT);
    block_completion=false;
    assert(co5300_qspi_io_recover(io,&recovered)==ESP_OK && recovered);
    assert(io->del(io)==ESP_OK && allocations==0 && devices==0);

    {
        Co5300Panel panel; Co5300Config cfg;
        cfg.bus=Co5300Bus::kQspi; cfg.pin_cs=12; cfg.x_gap=6;
        assert(panel.Init(cfg)==ESP_OK && panel.SetBrightness(0xB0)==ESP_OK);
        std::vector<uint8_t> pixels(40*454*2);
        for(size_t n=0;n<pixels.size();++n) pixels[n]=uint8_t(n);
        writes.clear();
        assert(panel.DrawBitmap(140,6,40,454,pixels.data())==ESP_OK);
        std::vector<uint8_t> transmitted;
        for(const auto& w:writes) if(w.cmd==0x32) transmitted.insert(transmitted.end(),w.data.begin(),w.data.end());
        assert(transmitted==pixels && writes.size()==12); // Four even-height stripes.
        assert(writes.front().data==std::vector<uint8_t>({0,146,0,185}));
        block_completion=true;
        assert(panel.DrawBitmap(140,6,40,454,pixels.data())==ESP_ERR_TIMEOUT);
        const int before=submitted;
        assert(panel.Recover(&recovered)==ESP_ERR_TIMEOUT && submitted==before);
        block_completion=false;
        // The next recovery attempt itself fails; retain reset_pending_ and retry.
        queue_error=ESP_ERR_TIMEOUT;
        assert(panel.Recover(&recovered)==ESP_ERR_TIMEOUT && !recovered);
        queue_error=ESP_OK;
        assert(panel.Recover(&recovered)==ESP_OK && recovered && resets==2 && initializations==2);
        assert(writes[writes.size()-2].addr==0x5100 && writes[writes.size()-2].data[0]==0xB0);
        assert(writes.back().addr==0x2900);
        assert(panel.DrawBitmap(140,6,40,454,pixels.data())==ESP_OK);
    }
    assert(allocations==0 && devices==0 && !pending);
    puts("CO5300 production IO/panel: cold-boot lazy SPI initialization, constructor cleanup, framed writes, finite waits, DMA ownership, deferred completion/errors, panel reinit/brightness/gap and striped payload passed.");
}
