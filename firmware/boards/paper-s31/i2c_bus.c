/**
 * @file i2c_bus.c
 * @brief 共享 I2C 总线：按地址缓存设备句柄，提供寄存器读写
 */
#include <string.h>
#include "board_pins.h"
#include "i2c_bus.h"

#define I2C_SPEED_HZ    100000
#define I2C_TIMEOUT_MS  100

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_devs[128];

esp_err_t i2c_bus_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* 板上已有 2.2k 上拉，内部上拉只是兜底 */
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

i2c_master_bus_handle_t i2c_bus_get(void)
{
    return i2c_bus_init() == ESP_OK ? s_bus : NULL;
}

static i2c_master_dev_handle_t dev(uint8_t addr)
{
    addr &= 0x7F;
    if (!s_devs[addr] && i2c_bus_init() == ESP_OK) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = I2C_SPEED_HZ,
        };
        i2c_master_bus_add_device(s_bus, &dc, &s_devs[addr]);
    }
    return s_devs[addr];
}

bool i2c_bus_probe(uint8_t addr)
{
    return i2c_bus_init() == ESP_OK && i2c_master_probe(s_bus, addr, 50) == ESP_OK;
}

esp_err_t i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t d = dev(addr);
    if (!d) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(d, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[17];
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    i2c_master_dev_handle_t d = dev(addr);
    if (!d) {
        return ESP_ERR_INVALID_STATE;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(d, buf, len + 1, I2C_TIMEOUT_MS);
}

esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    return i2c_write_regs(addr, reg, &value, 1);
}

esp_err_t i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len)
{
    i2c_master_dev_handle_t d = dev(addr);
    if (!d) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(d, r, sizeof(r), buf, len, I2C_TIMEOUT_MS);
}
