#pragma once
#include <cstdint>
#include "esp_err.h"

namespace agent_system::launcher {
enum class State {
    kIdle,
    kChecking,
    kCurrent,
    kAvailable,
    kDownloading,
    kVerifying,
    kReady,
    kError
};
struct Status {
    State state = State::kIdle;
    uint32_t revision = 0, current_code = 0, version_code = 0;
    uint32_t size = 0, received = 0, elapsed_ms = 0;
    uint64_t firmware_id = 0;
    esp_err_t error = ESP_OK;
    int http_status = 0, network_errno = 0;
    bool forced = false;
    char current[32]{}, version[32]{}, changelog[512]{}, message[384]{};
    char md5[33]{}, url[1024]{};
};
esp_err_t Init();
esp_err_t ArmTrial();  // Early app_main deadline, before NVS, animation or UI initialization.
Status Read();
bool Busy();
esp_err_t Check();
esp_err_t Install();
void Cancel();
esp_err_t Restart();
// Called after the local display/control loop has remained healthy for 10 s.
esp_err_t ConfirmHealthy();
}  // namespace agent_system::launcher
