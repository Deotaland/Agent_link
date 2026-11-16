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
    puts("Legacy audio completed."); fflush(stdout);
    // Include a plateau beyond the driver's 320-sample fade at either end.
    int16_t clip[1024];std::fill(std::begin(clip),std::end(clip),4000);
    assert(audio.PlayClip(nullptr,256)==ESP_ERR_INVALID_ARG);
    assert(audio.PlayClip(clip,160001)==ESP_ERR_INVALID_ARG);
    audio.SetVolume(50);peak.store(0);amplifier_used.store(false);
    assert(audio.PlayClip(clip,std::size(clip))==ESP_OK);
    Wait(audio);
    assert(audio.State()==sdgoods::Audio::Stage::kDone && amplifier_used.load() && peak.load()==2000);
    audio_tx_error=ESP_FAIL;
    assert(audio.PlayClip(clip,std::size(clip))==ESP_FAIL);
    Wait(audio);audio_tx_error=0;
    // Continuous voice reuses the real board's separate I2S RX/TX setup.
    sdgoods::Audio voice;
    audio_tx_error = ESP_FAIL;
    assert(voice.Init(50) == ESP_FAIL && channels.load() == 0 && !voice.Busy());
    audio_tx_error = 0;
    assert(voice.Init(50) == ESP_OK && channels.load() == 2 && !voice.MicOn());
    assert(voice.Start(sdgoods::Audio::Test::kSpeaker) == ESP_ERR_INVALID_STATE);
    int16_t capture[320]; size_t got = 123;
    assert(voice.ReadPcm(capture, 320, &got) == ESP_ERR_INVALID_STATE && got == 0);
    assert(voice.StartMic() == ESP_OK && voice.MicOn());
    assert(voice.ReadPcm(capture, 320, &got) == ESP_OK && got == 320);
    assert(voice.ChannelLevels()[0].samples == 320 && voice.ChannelLevels()[0].peak > 900);
    assert(voice.ChannelLevels()[1].samples == 0);
    assert(voice.StopMic() == ESP_OK && !voice.MicOn());
    assert(voice.StartMic() == ESP_OK);
    audio_read_error = ESP_FAIL;
    assert(voice.ReadPcm(capture, 320, &got) == ESP_FAIL && got == 0);
    audio_read_error = 0;
    peak.store(0);
    assert(voice.WritePcm(clip, 1024) == ESP_OK && peak.load() == 2000);
    assert(voice.SetSpeakerEnabled(true) == ESP_OK && amplifier.load());
    assert(voice.SetVolume(0) == ESP_OK && !amplifier.load());
    peak.store(0);
    assert(voice.WritePcm(clip, 1024) == ESP_OK && peak.load() == 0);
    assert(voice.SetVolume(50) == ESP_OK && amplifier.load());
    voice.Shutdown();
    assert(channels.load() == 0 && !voice.MicOn() && !voice.Busy() && !amplifier.load());
    puts("Voice channels completed."); fflush(stdout);
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
    assert(render().row_count == 8 && brightness == 75);
    assert(!strcmp(render().title, "Device tools") && !strcmp(render().rows[7], "Screen test"));
    assert(!strcmp(render().rows[0], "Factory Test"));
    assert(service->StartsHighLoad({amoled_ui::TouchAction::kRow, 0}));
    assert(!service->StartsHighLoad({amoled_ui::TouchAction::kRow, 2}));
    row(1);
    auto battery = render();
    assert(strstr(battery.info, "70%") && strstr(battery.info, "Charging: unknown"));
    back();
    row(2);
    service->Tick(true);
    auto motion = render();
    assert(motion.kind == amoled_ui::View::kHardwareTest && !motion.hardware_test.valid);
    // Flat, still samples must remain centered for a full second. Movement,
    // free-fall, read failures and upright placement cannot pass the test.
    const auto sample = [&] {
        now += 100000;
        service->Tick(true);
        return render().hardware_test;
    };
    const auto axes = [&](int x, int y, int z, int speed = 0) {
        imu_axes[0] = x; imu_axes[1] = y; imu_axes[2] = z;
        imu_axes[3] = speed; imu_axes[4] = imu_axes[5] = 0;
    };
    axes(0, 0, 8192);
    assert(sample().centered && !render().hardware_test.passed);
    for (int n = 0; n < 9; ++n) sample();
    assert(!render().hardware_test.passed);
    axes(1000, 0, 8131);
    assert(!sample().centered);
    axes(0, 0, 8192, 6000);
    assert(!sample().centered);
    axes(0, 0, 0);
    assert(!sample().valid);
    axes(8192, 0, 0);
    assert(sample().valid && !render().hardware_test.centered);
    axes(0, 0, 8192);
    for (int n = 0; n < 11; ++n) sample();
    assert(render().hardware_test.passed);
    row(0); // Restart clears the receipt and the hold timer.
    assert(!render().hardware_test.passed);
    sample();
    imu_read_fail = true;
    assert(!sample().valid && !render().hardware_test.centered);
    imu_read_fail = false;
    axes(0, 0, 8192);
    for (int n = 0; n < 11; ++n) sample();
    assert(render().hardware_test.passed);
    back();
    assert(registers[8] == 0x40);
    row(6);
    assert(strstr(render().rows[0], "Start touch"));
    row(0);
    const auto tap = [&](int x, int y) {
        return menu.handle({amoled_ui::TouchAction::kTouchPoint, 0, x, y}, menu.context);
    };
    tap(98, 132); // The wrong target shows coordinates but cannot advance.
    assert(render().hardware_test.x == 98 && render().hardware_test.completed == 0);
    tap(-1, 500);
    assert(render().hardware_test.x == 98);
    tap(205, 180); // Outside the 24px hit radius.
    assert(render().hardware_test.completed == 0);
    for (int n = 0; n < 5; ++n) {
        const auto target = render().hardware_test;
        tap(target.target_x, target.target_y);
        assert(render().hardware_test.completed == n + 1);
    }
    assert(render().hardware_test.passed);
    tap(180, 180);
    assert(render().hardware_test.completed == 5);
    row(0);
    assert(!render().hardware_test.passed && render().hardware_test.completed == 0);
    back(); // Touch -> display page -> hardware root.
    assert(strstr(render().title, "Display and touch"));
    back();
    peak.store(0);
    row(0);
    assert(render().hardware_test.kind == amoled_ui::HardwareTest::Kind::kColors &&
           render().hardware_test.color_index == 0 && service->HighLoad());
    // The button starts recording itself; taps only change the displayed color.
    assert(hardware.audio_.Busy() && peak.load() == 0);
    for (int color = 1; color <= amoled_ui::HardwareTest::kColorCount; ++color) {
        tap(180, 180);
        assert(render().hardware_test.color_index == color % amoled_ui::HardwareTest::kColorCount &&
               hardware.audio_.Busy());
    }
    bool recorded = false, played = false;
    for (unsigned n = 0; n < 3000 && hardware.audio_.Busy(); ++n) {
        const auto stage = hardware.audio_.State();
        recorded |= stage == sdgoods::Audio::Stage::kRecording;
        if (stage == sdgoods::Audio::Stage::kPlaying) {
            played = true;
            assert(!hardware.audio_.MicOn());
        }
        vTaskDelay(1);
    }
    Wait(hardware.audio_);
    assert(recorded && played && peak.load() > 0 && hardware.audio_.State() == sdgoods::Audio::Stage::kDone);
    assert(render().hardware_test.kind == amoled_ui::HardwareTest::Kind::kColors &&
           render().hardware_test.color_index == 0 && !service->HighLoad());
    back();
    assert(!service->HighLoad() && channels.load() == 0 && !amplifier.load());
    row(0);
    menu.close(menu.context);  // Home and top-down navigation use the same cleanup.
    assert(!hardware.audio_.Busy() && channels.load() == 0 && !amplifier.load());
    audio_read_error = ESP_FAIL;
    row(0);
    Wait(hardware.audio_);
    service->Tick(true);
    assert(!strcmp(render().title, "Device tools") && toast.find("injected-error") != std::string::npos);
    audio_read_error = 0;
    row(7);
    puts("Combined audio/color page completed."); fflush(stdout);
    assert(render().kind == amoled_ui::View::kHardwareTest && render().local_mode);
    assert(render().hardware_test.kind == amoled_ui::HardwareTest::Kind::kColors);
    assert(render().hardware_test.color_index == 0);
    tap(-1, 500);
    tap(0, 0);
    assert(render().hardware_test.color_index == 0);
    for (int index = 1; index <= amoled_ui::HardwareTest::kColorCount * 2; ++index) {
        tap(180, 180);
        assert(render().hardware_test.color_index == index % amoled_ui::HardwareTest::kColorCount);
    }
    tap(180, 30);
    assert(render().hardware_test.color_index == 1);
    assert(back() && !strcmp(render().title, "Device tools"));
    row(7);
    assert(render().hardware_test.color_index == 0);
    assert(menu.handle({amoled_ui::TouchAction::kHome, 0}, menu.context));
    assert(!strcmp(render().title, "Device tools"));
    assert(menu.open_brightness);
    menu.open_brightness(menu.context);
    assert(render().kind == amoled_ui::View::kBrightnessControl);
    assert(!strcmp(render().title, "Brightness") && render().brightness_percent == 75);
    assert(!back()); // Settings shortcut returns directly to the settings root.
    row(4);
    const auto level = [&](int value, bool commit) {
        amoled_ui::Input input{amoled_ui::TouchAction::kBrightness, value};
        input.commit = commit;
        return menu.handle(input, menu.context);
    };
    const auto before_drag = nvs;
    level(65, false);
    assert(brightness == 65 && render().brightness_percent == 65 && nvs == before_drag);
    level(65, true);
    assert(brightness == 65 && nvs.at("agent_nvs/button_hw/bright") == 65);
    level(0, true);
    assert(brightness == 5);
    level(110, true);
    assert(brightness == 100);
    auto restored = CreateFactoryHardwareServices(hardware);
    assert(brightness == 100);
    restored.reset();
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
    assert(!skip_wake_animation);
    power_events.clear();
    const auto key = [&](bool down, int64_t delta = 0) {
        now += delta;
        button = down;
        return service->Button(down, now);
    };
    // A key held across boot/wake must never cause immediate sleep or shutdown.
    assert(!key(true));
    assert(!key(true, 50000));
    assert(!key(true, 1500000));
    assert(!key(false));
    assert(!key(false, 50000));
    service->Tick(true);
    assert(power_events.empty() && !service->HighLoad());
    // Sub-debounce noise cannot become a short press.
    assert(!key(true));
    assert(!key(false, 10000));
    assert(!key(false, 50000));
    assert(!key(true));
    assert(!key(true, 50000));
    assert(!key(false, 100000));
    assert(key(false, 40000));
    assert(!key(false, 100000));
    service->RequestSleep(); // The controller requests this only from its home page.
    service->Tick(false);
    assert(power_events.empty() && !skip_wake_animation);
    service->RequestSleep();
    try {
        service->Tick(true);
        assert(false);
    } catch (const SleepSignal& action) {
        assert(!action.off && skip_wake_animation);
    }
    assert(power_events ==
           std::vector<std::string>({"running", "display-drained", "wifi-stop", "sleep"}));
    power_events.clear();
    // The release timestamp, not the 40ms debounce, decides the 1.2s boundary.
    assert(!key(true));
    assert(!key(true, 50000));
    assert(!key(false, 1149999));
    assert(key(false, 40000)); // 1,199,999us is still short.
    assert(!service->HighLoad());
    assert(!key(true));
    assert(!key(true, 40000));
    assert(!key(true, 1160000));
    service->Tick(true);
    assert(power_events.empty()); // Holding beyond the threshold never cuts power.
    assert(!key(false));
    assert(!key(false, 40000)); // 1,200,000us -> long release, no short action.
    assert(service->HighLoad());
    try {
        service->Tick(true);
        assert(false);
    } catch (const SleepSignal& action) {
        assert(action.off && !skip_wake_animation);
    }
    assert(power_events ==
           std::vector<std::string>({"factory", "display-drained", "wifi-stop", "off"}));
    power_events.clear();
    assert(!key(false, 100000));
    service->Tick(true);
    assert(power_events.empty());
    assert(nvs.at("agent_nvs/al_wifi/ssid") == 7 && nvs.at("nvs/bmb_net/ssid") == 9);
    service.reset();
    Finish();
    puts(
        "Sugarat Factory: ADC calibration/fallback/failures, median filter, six-axis units, audio "
        "playback/capture/cancel/errors/PSRAM release, guided touch targets, level stability, persistent brightness and "
        "guarded power actions, short/long release, boot-held keys and sleep boot selection passed. No hardware I/O.");
}
