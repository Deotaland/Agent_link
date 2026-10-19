/**
 * @file i2c_bus.h
 * @brief 板上共享 I2C 总线（SDA=IO0 / SCL=IO1，100kHz）
 *
 * ES8389、PCF85063、SC7A20、BQ27220、触摸、摄像头都挂在这一条上，全部共用这个句柄。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_bus_init(void);
i2c_master_bus_handle_t i2c_bus_get(void);

bool i2c_bus_probe(uint8_t addr);
esp_err_t i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t i2c_write_regs(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);
esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t value);

/* 16 位寄存器地址（高字节在前），摄像头 SCCB 用 */
esp_err_t i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
