#include "button_hardware_facade.h"
#include "../factory/common/factory_hardware_services.h"
#include <cmath>
#include <cstdio>
void Wait(sdgoods::Audio& audio) {
    for (unsigned n = 0; n < 3000 && audio.Busy(); ++n)
        vTaskDelay(1);
    assert(!audio.Busy() && !button_fixture::amplifier.load());
    assert(button_fixture::channels.load() == 0 && button_fixture::pcm_allocations.load() == 0);
}
int main() {
    using namespace button_fixture;
    assert(sdgoods::BatteryPercent(3000) == 0 && sdgoods::BatteryPercent(3640) == 50 &&
           sdgoods::BatteryPercent(4280) == 100);
    assert(sdgoods::BatteryPercent(0) == -1 && sdgoods::BatteryPercent(5100) == -1);
    sdgoods::Sensors sensors;
    sensors.Init((void*)1);
    assert(sensors.Read().battery_mv == 3900 && sensors.Read().percent == 70 &&
           sensors.Read().calibrated);
    const int creations = adc_creations;
    sensors.Init((void*)1);
    assert(adc_creations == creations);
    sensors.Poll(true);
    assert(sensors.Read().imu_valid);
    assert(sensors.Read().acceleration[0] == 1 && sensors.Read().acceleration[1] == -1 &&
           sensors.Read().acceleration[2] == 2);
    assert(sensors.Read().angular_velocity[0] == 1 && sensors.Read().angular_velocity[1] == -2);
    sensors.Suspend();
    assert(registers[8] == 0x40);
    adc_read_fail = true;
    sensors.Poll(false);
    assert(sensors.Read().percent == -1);
    adc_read_fail = false;
    calibration_fail = true;
    adc_mv = 1613;
    sdgoods::Sensors fallback;
    fallback.Init((void*)1);
    assert(fallback.BatteryReady() && !fallback.Read().calibrated && fallback.Read().percent == 70);
    calibration_fail = false;
    adc_fail = true;
    sdgoods::Sensors absent;
    absent.Init((void*)1);
    assert(!absent.BatteryReady() && absent.Read().percent == -1);
    adc_fail = false;
    adc_mv = 1300;
    identity = 0;
    sdgoods::Sensors wrong;
    wrong.Init((void*)1);
    assert(!wrong.ImuReady());
    identity = 5;
    sdgoods::Audio audio;
    assert(channels.load() == 0 && !amplifier.load());
    audio.SetVolume(25);
    assert(audio.Start(sdgoods::Audio::Test::kSpeaker) == ESP_OK);
    assert(audio.Start(sdgoods::Audio::Test::kRecordPlayback) == ESP_ERR_INVALID_STATE);
    Wait(audio);
    assert(audio.State() == sdgoods::Audio::Stage::kDone && peak.load() > 0 && peak.load() <= 1500);
    amplifier_used.store(false);
    peak.store(0);
    audio.SetVolume(0);
    assert(audio.Start(sdgoods::Audio::Test::kSpeaker) == ESP_OK);
    Wait(audio);
    assert(!amplifier_used.load() && peak.load() == 0);
    audio.SetVolume(35);
    assert(audio.Start(sdgoods::Audio::Test::kRecordPlayback) == ESP_OK);
    Wait(audio);
    assert(audio.State() == sdgoods::Audio::Stage::kDone);
    audio_read_error = ESP_FAIL;
    assert(audio.Start(sdgoods::Audio::Test::kRecordPlayback) == ESP_OK);
    Wait(audio);
    assert(audio.State() == sdgoods::Audio::Stage::kFailed);
    audio_read_error = 0;
    audio_tx_error = ESP_FAIL;
    assert(audio.Start(sdgoods::Audio::Test::kSpeaker) == ESP_OK);
    Wait(audio);
    assert(audio.State() == sdgoods::Audio::Stage::kFailed);
    audio_tx_error = 0;
    assert(audio.Start(sdgoods::Audio::Test::kRecordPlayback) == ESP_OK);
    vTaskDelay(20);
    assert(audio.StopAndWait());
    Wait(audio);
    assert(audio.State() == sdgoods::Audio::Stage::kStopped);
    AmoledHardware hardware;
    hardware.sensors_.Init((void*)1);
    nvs["agent_nvs/al_wifi/ssid"] = 7;
    nvs["nvs/bmb_net/ssid"] = 9;
    auto service = CreateFactoryHardwareServices(hardware);
    amoled_ui::Screen status;
    service->Decorate(status);
    assert(status.battery_percent == hardware.sensors_.Read().percent);
    assert(strstr(status.hardware_status, "%"));
    test_wifi.connected = false;
    test_wifi.setup_active = true;
    snprintf(test_wifi.setup_ssid, sizeof test_wifi.setup_ssid, "Sugarat-Button-ABCDEF");
    service->Decorate(status);
    assert(!status.wifi_connected && status.wifi_setup_active &&
           !strcmp(status.wifi_setup_ssid, test_wifi.setup_ssid));
    test_wifi = {};
    service->Decorate(status);
    assert(status.wifi_connected && !status.wifi_setup_active && !status.wifi_setup_ssid[0]);
    auto menu = service->Menu();
    const auto row = [&](int index) {
        return menu.handle({amoled_ui::TouchAction::kRow, index}, menu.context);
    };
    const auto back = [&] { return menu.handle({amoled_ui::TouchAction::kBack, 0}, menu.context); };
    const auto render = [&] {
        amoled_ui::Screen s;
        menu.render(s, menu.context);
        return s;
    };
    assert(render().row_count == 7 && brightness == 75);
    row(0);
    auto battery = render();
    assert(strstr(battery.info, "70%") && strstr(battery.info, "Charging: unknown"));
    back();
    row(1);
    service->Tick(true);
    auto motion = render();
    assert(strstr(motion.info, "Accel (g)"));
    back();
    row(4);
    row(0);
    assert(brightness == 65 && nvs.at("agent_nvs/button_hw/bright") == 65);
    back();
    row(5);
    row(0);
    row(1);
    service->Tick(true);
    assert(power_events.empty());
    row(1);
    row(0);
    service->Tick(false);
    assert(power_events.empty());
    row(1);
    row(0);
    ui_ready = false;
    service->Tick(true);
    assert(power_events ==
           std::vector<std::string>({"factory", "display-drained", "display-resumed"}));
    power_events.clear();
    ui_ready = true;
    row(1);
    row(0);
    try {
        service->Tick(true);
        assert(false);
    } catch (const SleepSignal& action) {
        assert(action.off);
    }
    assert(power_events ==
           std::vector<std::string>({"factory", "display-drained", "wifi-stop", "off"}));
    assert(!service->Button(true, now));
    assert(!service->Button(false, now));
    assert(!service->Button(true, now));
    now += 1300000;
    assert(service->Button(true, now));
    assert(nvs.at("agent_nvs/al_wifi/ssid") == 7 && nvs.at("nvs/bmb_net/ssid") == 9);
    service.reset();
    Finish();
    puts(
        "Sugarat Factory: ADC calibration/fallback/failures, median filter, six-axis units, audio "
        "playback/capture/cancel/errors/PSRAM release, hardware pages, persistent brightness and "
        "guarded power actions passed. No hardware I/O.");
}
