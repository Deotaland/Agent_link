#pragma once
#include <cstdint>
#include <cstddef>
#include "esp_err.h"
using i2c_port_t=int;
using i2c_master_bus_handle_t=void*;
struct FakeI2cDevice;
using i2c_master_dev_handle_t=FakeI2cDevice*;
constexpr int I2C_ADDR_BIT_LEN_7=7;
struct i2c_device_config_t { int dev_addr_length=0; uint16_t device_address=0; uint32_t scl_speed_hz=0; };
int i2c_master_get_bus_handle(i2c_port_t,void**);
int i2c_master_probe(void*,uint16_t,int);
int i2c_master_bus_add_device(void*,const i2c_device_config_t*,FakeI2cDevice**);
int i2c_master_bus_rm_device(FakeI2cDevice*);
int i2c_master_transmit_receive(FakeI2cDevice*,const uint8_t*,size_t,uint8_t*,size_t,int);
int i2c_master_transmit(FakeI2cDevice*,const uint8_t*,size_t,int);
