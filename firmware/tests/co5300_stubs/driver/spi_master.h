#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int spi_host_device_t;
#define SPI2_HOST 1
#define SPI_DEVICE_HALFDUPLEX 16
#define SPI_TRANS_MODE_QIO 2
typedef struct fake_spi_device* spi_device_handle_t;
typedef struct {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    size_t length;
    const void* tx_buffer;
} spi_transaction_t;
typedef struct {
    uint8_t command_bits, address_bits, mode;
    int clock_speed_hz, spics_io_num;
    uint32_t flags;
    int queue_size;
} spi_device_interface_config_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t spi_bus_get_max_transaction_len(spi_host_device_t, size_t*);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t*);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_device_queue_trans(spi_device_handle_t, spi_transaction_t*, TickType_t);
esp_err_t spi_device_get_trans_result(spi_device_handle_t, spi_transaction_t**, TickType_t);
#ifdef __cplusplus
}
#endif
