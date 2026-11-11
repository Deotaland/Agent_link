#include "sdgoods_sensors.h"
#include "config.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <algorithm>
#include <iterator>

namespace sdgoods {
namespace {
constexpr const char* kTag = "button.sensors";
}
esp_err_t Sensors::Write(uint8_t reg, uint8_t value) {
    const uint8_t bytes[]{reg, value};
    return i2c_master_transmit(imu_, bytes, sizeof bytes, 30);
}
void Sensors::Init(i2c_master_bus_handle_t bus) {
    if (!adc_) {
        adc_oneshot_unit_init_cfg_t unit{};
        unit.unit_id = ADC_UNIT_1;
        esp_err_t error = adc_oneshot_new_unit(&unit, &adc_);
        if (error == ESP_OK) {
            adc_oneshot_chan_cfg_t channel{};
            channel.atten = ADC_ATTEN_DB_12;
            channel.bitwidth = ADC_BITWIDTH_DEFAULT;
            error = adc_oneshot_config_channel(adc_, ADC_CHANNEL_7, &channel);
        }
        if (error != ESP_OK) {
            if (adc_)
                adc_oneshot_del_unit(adc_);
            adc_ = nullptr;
            ESP_LOGW(kTag, "Battery ADC unavailable: %s", esp_err_to_name(error));
        }
    }
    if (adc_ && !calibration_) {
        adc_cali_curve_fitting_config_t calibration{};
        calibration.unit_id = ADC_UNIT_1;
        calibration.chan = ADC_CHANNEL_7;
        calibration.atten = ADC_ATTEN_DB_12;
        calibration.bitwidth = ADC_BITWIDTH_DEFAULT;
        const auto error = adc_cali_create_scheme_curve_fitting(&calibration, &calibration_);
        if (error != ESP_OK)
            ESP_LOGW(kTag, "Battery calibration unavailable: %s", esp_err_to_name(error));
    }
    if (bus && !imu_ && i2c_master_probe(bus, IMU_I2C_ADDRESS, 30) == ESP_OK) {
        i2c_device_config_t device{};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = IMU_I2C_ADDRESS;
        device.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus, &device, &imu_) == ESP_OK) {
            const uint8_t who = 0;
            uint8_t identity = 0;
            if (i2c_master_transmit_receive(imu_, &who, 1, &identity, 1, 30) != ESP_OK ||
                identity != 0x05 || Write(0x02, 0x40) != ESP_OK || Write(0x03, 0x15) != ESP_OK ||
                Write(0x04, 0x25) != ESP_OK || Write(0x08, 0x40) != ESP_OK) {
                i2c_master_bus_rm_device(imu_);
                imu_ = nullptr;
            }
        }
    }
    ESP_LOGI(kTag, "Battery ADC=%d calibration=%d; QMI8658=%d", adc_ != nullptr,
             calibration_ != nullptr, imu_ != nullptr);
    Poll(false);
}
void Sensors::Poll(bool motion) {
    int samples[9]{};
    bool valid = adc_ != nullptr;
    for (int& sample : samples) {
        int raw = 0;
        if (!valid)
            continue;
        if (adc_oneshot_read(adc_, ADC_CHANNEL_7, &raw) != ESP_OK || raw < 0 || raw > 4095)
            valid = false;
        else if (calibration_)
            valid = adc_cali_raw_to_voltage(calibration_, raw, &sample) == ESP_OK;
        else
            sample = raw * 3300 / 4095;  // Vendor fallback; explicitly labelled uncalibrated.
    }
    if (valid) {
        std::sort(std::begin(samples), std::end(samples));
        const int measured = samples[4] * 3;
        if (BatteryPercent(measured) >= 0) {
            filtered_mv_ = filtered_mv_ < 0 ? measured : (filtered_mv_ * 3 + measured + 2) / 4;
            state_.battery_mv = filtered_mv_;
            state_.percent = BatteryPercent(filtered_mv_);
            state_.calibrated = calibration_ != nullptr;
        } else
            valid = false;
    }
    if (!valid) {
        state_.battery_mv = state_.percent = filtered_mv_ = -1;
        state_.calibrated = false;
    }
    state_.imu_valid = false;
    if (!imu_)
        return;
    if (motion != motion_enabled_) {
        if (Write(0x08, motion ? 0x43 : 0x40) != ESP_OK)
            return;
        motion_enabled_ = motion;
    }
    if (!motion)
        return;
    const uint8_t reg = 0x35;
    uint8_t bytes[12]{};
    if (i2c_master_transmit_receive(imu_, &reg, 1, bytes, sizeof bytes, 30) != ESP_OK)
        return;
    // Vendor CTRL2=0x15: +/-4 g. CTRL3=0x25: +/-64 degrees/second.
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto read = [&](unsigned offset) {
            return static_cast<int16_t>(uint16_t(bytes[offset]) |
                                        (uint16_t(bytes[offset + 1]) << 8));
        };
        state_.acceleration[axis] = read(axis * 2) / 8192.0f;
        state_.angular_velocity[axis] = read(6 + axis * 2) / 512.0f;
    }
    state_.imu_valid = true;
}
void Sensors::Suspend() {
    if (imu_)
        (void)Write(0x08, 0x40);
    motion_enabled_ = false;
    state_.imu_valid = false;
}
}  // namespace sdgoods
