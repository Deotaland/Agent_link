#pragma once
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

namespace sdgoods {
// The vendor UI uses a linear estimate, not a fuel gauge/coulomb counter.
inline int BatteryPercent(int millivolts) {
    if (millivolts < 500 || millivolts > 5000)
        return -1;
    const int percent = (millivolts - 3000) * 100 + 640;
    return percent <= 0 ? 0 : percent >= 128000 ? 100 : percent / 1280;
}
struct SensorSnapshot {
    int battery_mv = -1, percent = -1;
    bool calibrated = false, imu_valid = false;
    float acceleration[3]{}, angular_velocity[3]{};
};
class Sensors {
   public:
    void Init(i2c_master_bus_handle_t bus);
    void Poll(bool motion);
    const SensorSnapshot& Read() const {
        return state_;
    }
    void Suspend();
    bool BatteryReady() const {
        return adc_ != nullptr;
    }
    bool ImuReady() const {
        return imu_ != nullptr;
    }
    bool MotionEnabled() const {
        return motion_enabled_;
    }

   private:
    esp_err_t Write(uint8_t reg, uint8_t value);
    adc_oneshot_unit_handle_t adc_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
    i2c_master_dev_handle_t imu_ = nullptr;
    SensorSnapshot state_{};
    int filtered_mv_ = -1;
    bool motion_enabled_ = false;
};
}  // namespace sdgoods
