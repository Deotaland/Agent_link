#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "paper_display_diagnostic.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL at line %d\n", __LINE__); std::exit(1); } } while (false)
#define CONFIG_PAPER_S31_DISPLAY_DIAGNOSTIC 1
#define EPD_PANEL_3IN6 2
#define EPD_PANEL EPD_PANEL_3IN6
#define EPD_BUSY_TIMEOUT_MS 60000
#define EPD_BUSY_GPIO 7
#define pdMS_TO_TICKS(ms) (ms)
#define pdTICKS_TO_MS(ticks) (ticks)
using TickType_t = uint32_t;
static const char* TAG = "epd";
static bool s_dead;
static int s_busy_timeouts;
static int mode;
static uint32_t elapsed;
static std::string logs;
uint32_t xTaskGetTickCount() { return elapsed; }
int64_t esp_timer_get_time() { return int64_t(elapsed) * 1000; }
void vTaskDelay(uint32_t ms) { elapsed += ms; }
int gpio_get_level(int) {
    if (mode == 0) return elapsed >= 1200; // a real LOW -> HIGH transition
    if (mode == 1) return elapsed < 5;     // assertion arrives after the first HIGH sample
    if (mode == 2) return 0;              // never releases
    return 1;                            // stays HIGH throughout
}
void Log(const char*, const char* format, ...) {
    char text[512]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args); va_end(args);
    logs += text; logs += '\n';
}
#define ESP_LOGI Log
#define ESP_LOGW Log
#define ESP_LOGE Log
#define ESP_LOGD(...) do {} while (false)

// INSERT_PRODUCTION_BUSY_WAIT

void Reset(int value) { mode = value; elapsed = 0; logs.clear(); s_dead = false; s_busy_timeouts = 0; }
int main() {
    Reset(0); lcd_chkstatus_stage("DRF");
    CHECK(elapsed == 1300 && !s_dead && s_busy_timeouts == 0);
    CHECK(logs.find("observed_low=1 exit=1 after_settle=1 wait=1200 ms") != std::string::npos);
    CHECK(logs.find("UNVERIFIED") == std::string::npos);
    Reset(1); lcd_chkstatus_stage("DRF");
    CHECK(elapsed == 100); // diagnostic observes, it does not silently introduce a timing fix
    CHECK(logs.find("observed_low=0 exit=1 after_settle=0 wait=0 ms") != std::string::npos);
    CHECK(logs.find("UNVERIFIED") != std::string::npos);
    Reset(2); lcd_chkstatus_stage("DRF");
    CHECK(s_dead && s_busy_timeouts == 1 && elapsed > EPD_BUSY_TIMEOUT_MS);
    const auto before = elapsed; lcd_chkstatus_stage("POF");
    CHECK(elapsed == before && s_busy_timeouts == 1 && logs.find("POF skipped") != std::string::npos);
    Reset(3); lcd_chkstatus_stage("DRF");
    CHECK(elapsed == 100 && logs.find("UNVERIFIED") != std::string::npos);
    const auto first = paper::DisplayDiagnosticPage();
    const auto second = paper::DisplayDiagnosticPage();
    CHECK(first.Same(second));
    CHECK(std::strcmp(first.lines[0], "S31 DISPLAY TEST") == 0);
    CHECK(std::strcmp(first.lines[13], "HOLD UNTIL REBOOT") == 0);
    for (const auto& line : first.lines) CHECK(std::strlen(line) < sizeof line);
    std::puts("PASS: real BUSY completion, late assertion, stuck LOW, absent assertion and invariant diagnostic frame");
    return 0;
}
