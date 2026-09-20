// Work badge (ESP32-S3) - an electronic staff ID card.
//
// Same board as boards/rorolee-s3 (SH8501 AMOLED, ES8311/ES7210 codec, BQ27220 gauge, three
// buttons, motor), running a much smaller job: show who is wearing it. The codec, SD card and
// buttons are left alone - a badge only needs the screen, the fuel gauge and the App link.
//
// Who fills the card in: the App / Agent, through five `badge.*` OUT endpoints published in
// the agent_link I/O manifest. Each write lands in NVS, so the badge keeps showing the right
// person across a power cycle or a firmware upgrade.
//
// Firmware upgrades need no code here at all: the SDK handles the App's OTA commands itself.
// The only thing this board adds is a progress screen, wired through one callback.

#include "badge_data.h"
#include "badge_ui.h"
#include "board.h"
#include "bq27220.h"
#include "config.h"
#include "sh8501_lk_panel.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "agent_link.h"
#include "agent_link_ota.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "WorkBadge"

namespace {

// One endpoint per line of the card. `str` values arrive as raw UTF-8 with no terminator, so
// the actuate callback gets (bytes, length) and hands them straight to the store.
struct FieldEndpoint {
    agent_link_io_desc_t desc;
    badge::Field         field;
};

// Descriptors must outlive registration (the SDK keeps the pointer), hence file-scope storage.
FieldEndpoint g_fields[] = {
    {{ .id = "badge.name", .dir = AGENT_IO_OUT, .kind = "screen.text", .value = AGENT_VAL_STR,
       .unit = nullptr, .desc = "Name printed on the badge, in large type",
       .display_name = "Name" }, badge::Field::kName},
    {{ .id = "badge.title", .dir = AGENT_IO_OUT, .kind = "screen.text", .value = AGENT_VAL_STR,
       .unit = nullptr, .desc = "Job title, e.g. Firmware Engineer",
       .display_name = "Job title" }, badge::Field::kTitle},
    {{ .id = "badge.dept", .dir = AGENT_IO_OUT, .kind = "screen.text", .value = AGENT_VAL_STR,
       .unit = nullptr, .desc = "Department or team",
       .display_name = "Department" }, badge::Field::kDepartment},
    {{ .id = "badge.id", .dir = AGENT_IO_OUT, .kind = "screen.text", .value = AGENT_VAL_STR,
       .unit = nullptr, .desc = "Employee number shown under the card",
       .display_name = "Employee ID" }, badge::Field::kEmployeeId},
    {{ .id = "badge.company", .dir = AGENT_IO_OUT, .kind = "screen.text", .value = AGENT_VAL_STR,
       .unit = nullptr, .desc = "Organisation name in the header strip",
       .display_name = "Company" }, badge::Field::kCompany},
};

// Runs on the transport task: store + persist, and let the render task pick the change up.
void OnFieldWrite(const char* /*id*/, const uint8_t* args, size_t len, void* ctx) {
    auto* ep = static_cast<FieldEndpoint*>(ctx);
    badge::Data::Instance().Set(ep->field, reinterpret_cast<const char*>(args), len);
}

// Runs on the OTA worker task; SetOtaStatus only stores atomics, so it is safe from there.
void OnOtaProgress(const agent_ota_status_t* st, void* /*ctx*/) {
    badge::Ui::Instance().SetOtaStatus(st);
}

}  // namespace

class WorkBadgeBoard : public Board {
public:
    WorkBadgeBoard() {
        BuildDeviceName();
        PowerOnRail();
        badge::Data::Instance().Load();
        InitDisplay();
        InitI2cBus();
        InitFuelGauge();
        StartUi();
        RegisterEndpoints();
        StartStatusTask();
    }

    // "ROROLEE_<last 6 MAC hex>" - the exact shape the mass-production firmware advertises, so
    // the official App picks these units out of a scan the same way it always has, and so two
    // badges on one desk are still tellable apart.
    const char* Name() const override { return name_; }

    // Same PCB as rorolee-s3 and as the mass-produced unit, so it reports the production model
    // string: that is what lets the App offer this firmware to an existing product, and what
    // stops a build for different hardware being pushed here. See docs/agent_link_ota.md.
    const char* Model() const override { return "RRL-01"; }

    // A badge shows things and watches its own battery. No mic, speaker or motor is claimed:
    // the parts are on the board, but this firmware does not drive them, and advertising a
    // capability the Agent then cannot use is worse than not having it.
    uint32_t Capabilities() const override {
        return AGENT_CAP_SCREEN | AGENT_CAP_BATTERY;
    }

    // The Agent's generic "show this text": a short note under the card (shift, meeting room,
    // visitor escort...). The card fields themselves have their own endpoints.
    void ShowText(const char* utf8) override {
        ESP_LOGI(TAG, "note: \"%s\"", utf8 ? utf8 : "");
        badge::Ui::Instance().SetNote(utf8);
    }

    void OnLinkState(bool connected) override { badge::Ui::Instance().SetLinkState(connected); }

    // Served from the status task's cache so the gauge is read once per cycle, not once per
    // caller (app_main polls these while the link is up; the UI needs them regardless).
    int  GetBatteryLevel() override { return batt_pct_.load(std::memory_order_acquire); }
    bool IsCharging()      override { return charging_.load(std::memory_order_acquire); }

private:
    void BuildDeviceName() {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;   // keep the compiled-in fallback
        std::snprintf(name_, sizeof(name_), "ROROLEE_%02X%02X%02X", mac[3], mac[4], mac[5]);
    }

    // Peripheral rail (display + gauge hang off it) - hold it on across light sleep.
    void PowerOnRail() {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << POWER_CTRL_PIN;
        cfg.mode         = GPIO_MODE_OUTPUT;
        if (gpio_config(&cfg) != ESP_OK) { ESP_LOGE(TAG, "power gpio cfg failed"); return; }
        gpio_set_level(POWER_CTRL_PIN, POWER_CTRL_ACTIVE_HIGH ? 1 : 0);   // active low
        (void)gpio_hold_en(POWER_CTRL_PIN);
        vTaskDelay(pdMS_TO_TICKS(50));   // let the supply settle before talking to anything
        ESP_LOGI(TAG, "peripheral power on (GPIO%d)", static_cast<int>(POWER_CTRL_PIN));
    }

    // Factory 简码 panel: same controller as boards/rorolee-s3 but a different bring-up, and
    // the only one the production panels' OTP supports. See boards/common/sh8501_lk_panel.h.
    void InitDisplay() {
        Sh8501LkConfig c = {};
        c.spi_host = DISPLAY_SPI_HOST;
        c.pin_sck  = DISPLAY_SCK_PIN;
        c.pin_mosi = DISPLAY_MOSI_PIN;
        c.pin_cs   = DISPLAY_CS_PIN;
        c.pin_dc   = DISPLAY_DC_PIN;
        c.pin_rst  = DISPLAY_RST_PIN;
        c.width    = DISPLAY_WIDTH;
        c.height   = DISPLAY_HEIGHT;
        c.pclk_hz  = DISPLAY_SPI_CLK_HZ;
        if (panel_.Init(c) != ESP_OK) { ESP_LOGE(TAG, "display init failed"); return; }
        // No colour self-test here: the badge should come up looking like a badge. Init()
        // already leaves the panel lit and cleared, and the first LVGL frame follows in ms.
        (void)panel_.SetBrightness(BADGE_BRIGHTNESS);
        display_ok_ = true;
    }

    // rorolee-s3 gets its I2C bus for free from the codec bring-up; the badge skips the codec,
    // so it creates the bus itself for the fuel gauge.
    void InitI2cBus() {
        i2c_master_bus_config_t bc = {};
        bc.i2c_port                     = AUDIO_I2C_PORT;
        bc.sda_io_num                   = AUDIO_I2C_SDA;
        bc.scl_io_num                   = AUDIO_I2C_SCL;
        bc.clk_source                   = I2C_CLK_SRC_DEFAULT;
        bc.glitch_ignore_cnt            = 7;
        bc.flags.enable_internal_pullup = true;
        i2c_master_bus_handle_t bus = nullptr;
        const esp_err_t r = i2c_new_master_bus(&bc, &bus);
        if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already created
            ESP_LOGW(TAG, "i2c bus init: %s (battery will be unavailable)", esp_err_to_name(r));
            return;
        }
        i2c_ok_ = true;
    }

    void InitFuelGauge() {
        if (!i2c_ok_) return;
        if (gauge_.Init(AUDIO_I2C_PORT, BQ27220_ADDR) != ESP_OK) {
            ESP_LOGW(TAG, "BQ27220 init failed - battery shown as unknown");
            return;
        }
        gauge_ok_ = true;
    }

    void StartUi() {
        if (!display_ok_) { ESP_LOGE(TAG, "no display - badge cannot be shown"); return; }
        if (badge::Ui::Instance().Start(&panel_) != ESP_OK) ESP_LOGE(TAG, "UI start failed");
        // The SDK drives this from its OTA worker; it is the only board hook OTA needs.
        agent_link_ota_set_callback(OnOtaProgress, nullptr);
    }

    // Publish the card's fields so the App and the Agent can fill them in. Registration must
    // happen before agent_link_start(), which app_main calls after the board is constructed.
    void RegisterEndpoints() {
        for (auto& ep : g_fields) {
            if (agent_link_register_io(&ep.desc, OnFieldWrite, &ep) != ESP_OK)
                ESP_LOGW(TAG, "register '%s' failed", ep.desc.id);
        }
        ESP_LOGI(TAG, "%d badge field endpoint(s) published",
                 static_cast<int>(sizeof(g_fields) / sizeof(g_fields[0])));
    }

    void StartStatusTask() {
        xTaskCreate(&WorkBadgeBoard::StatusTaskEntry, "badge_status", 3072, this, 3, nullptr);
    }
    static void StatusTaskEntry(void* arg) { static_cast<WorkBadgeBoard*>(arg)->StatusLoop(); }

    // Single reader of the gauge: refresh the cache and the on-screen battery every 5s. The
    // link-side report is app_main's job and reads the same cache.
    void StatusLoop() {
        while (true) {
            const int  pct = gauge_ok_ ? gauge_.Soc() : -1;
            const bool chg = gauge_ok_ && gauge_.IsCharging();
            batt_pct_.store(pct, std::memory_order_release);
            charging_.store(chg, std::memory_order_release);
            badge::Ui::Instance().SetBattery(pct, chg);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    char              name_[32] = "ROROLEE_BADGE";   // replaced with the MAC-suffixed name at boot
    Sh8501LkPanel     panel_;
    Bq27220           gauge_;
    bool              display_ok_ = false;
    bool              i2c_ok_     = false;
    bool              gauge_ok_   = false;
    std::atomic<int>  batt_pct_{-1};
    std::atomic<bool> charging_{false};
};

DECLARE_BOARD(WorkBadgeBoard);
