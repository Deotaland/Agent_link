#include <cassert>
#include <cstdio>
#include <cstring>
#include "cst9217_touch.h"
#include "touch_test_state.h"

unsigned delayed_ms = 0, address_at = 0, transmits = 0, receives = 0;
int transmit_error = ESP_OK, receive_error = ESP_OK, probe_error = ESP_OK;
bool reset_low = false, reset_high = false;
struct FakeI2cDevice {} device;
uint8_t report[10]{0x06, 14, 15, 0x9C, 50, 1, 0xAB, 0, 0, 0}; // 233,252
int i2c_master_get_bus_handle(i2c_port_t port, void** bus) {
    assert(port == 0); *bus = &device; return ESP_OK;
}
int i2c_master_probe(void*, uint16_t address, int) { assert(address == 0x5A); return probe_error; }
int i2c_master_bus_add_device(void*, const i2c_device_config_t* cfg, FakeI2cDevice** out) {
    assert(cfg->device_address == 0x5A && cfg->scl_speed_hz == 400000); *out = &device; return ESP_OK;
}
int i2c_master_transmit(FakeI2cDevice*, const uint8_t* bytes, size_t length, int) {
    assert(length == 2 && bytes[0] == 0xD0 && bytes[1] == 0x00);
    ++transmits; address_at = delayed_ms; return transmit_error;
}
int i2c_master_receive(FakeI2cDevice*, uint8_t* bytes, size_t length, int) {
    assert(length == 10 && delayed_ms - address_at >= 2); ++receives;
    if (receive_error == ESP_OK) memcpy(bytes, report, length);
    return receive_error;
}
esp_err_t gpio_config(const gpio_config_t* cfg) {
    assert(cfg->pin_bit_mask == (1ULL << 40) || cfg->pin_bit_mask == (1ULL << 11)); return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t gpio, int high) {
    assert(gpio == 40);
    if (!high) reset_low = true;
    else { assert(reset_low && delayed_ms >= 10); reset_high = true; }
    return ESP_OK;
}

int main() {
    Cst9217Touch touch;
    assert(touch.Init(0, 40, 11, 466, 466) == ESP_OK && touch.Ready());
    assert(reset_low && reset_high && delayed_ms == 60);
    uint8_t raw[10]{}; cst9217::Point point;
    assert(touch.Poll(&point, raw) == ESP_OK && point.pressed && point.x == 233 && point.y == 252);
    assert(memcmp(raw, report, sizeof raw) == 0 && transmits == 1 && receives == 1);
    assert(touch.Poll(&point) == ESP_OK && transmits == 2 && receives == 2);
    report[6] = 0;
    assert(touch.Poll(&point, raw) == ESP_ERR_INVALID_RESPONSE && !point.pressed && raw[6] == 0);
    report[6] = 0xAB;
    transmit_error = ESP_FAIL;
    const auto before = receives;
    assert(touch.Poll(&point, raw) == ESP_FAIL && receives == before && !point.pressed);
    transmit_error = ESP_OK; receive_error = ESP_FAIL;
    assert(touch.Poll(&point, raw) == ESP_FAIL && !point.pressed);
    receive_error = ESP_OK;
    assert(touch.Poll(&point, raw) == ESP_OK && point.pressed);
    probe_error = ESP_FAIL;
    Cst9217Touch missing;
    assert(missing.Init(0, 40, 11, 466, 466) == ESP_FAIL && !missing.Ready());
    assert(missing.Poll(&point, raw) == ESP_ERR_INVALID_STATE && !point.pressed);

    probe_error = ESP_OK;
    Cst9217Touch oriented;
    assert(oriented.Init(0, 40, 11, 466, 466, true, true) == ESP_OK);
    auto set_report = [](unsigned x, unsigned y, bool pressed = true) {
        memset(report, 0, sizeof report);
        report[0] = pressed ? 0x06 : 0;
        report[1] = static_cast<uint8_t>(x >> 4); report[2] = static_cast<uint8_t>(y >> 4);
        report[3] = static_cast<uint8_t>(((x & 15) << 4) | (y & 15));
        report[5] = pressed ? 1 : 0; report[6] = 0xAB;
    };
    // Opposite corners and center-adjacent pixels catch axis/off-by-one errors.
    const unsigned samples[][2]{{0,0}, {465,0}, {0,465}, {465,465}, {233,252}, {232,233}};
    for (const auto& sample : samples) {
        set_report(sample[0], sample[1]);
        assert(oriented.Poll(&point, raw) == ESP_OK && point.pressed);
        assert(point.x == 465 - sample[0] && point.y == 465 - sample[1]);
        assert(memcmp(raw, report, sizeof raw) == 0);
        cst9217::Point original;
        assert(cst9217::Decode(raw, sizeof raw, 466, 466, &original));
        assert(original.x == sample[0] && original.y == sample[1]);
    }
    set_report(466, 233);
    assert(oriented.Poll(&point, raw) == ESP_ERR_INVALID_RESPONSE && !point.pressed);
    set_report(0, 0, false);
    assert(oriented.Poll(&point, raw) == ESP_OK && !point.pressed && point.x == 0 && point.y == 0);
    Cst9217Touch horizontal;
    assert(horizontal.Init(0, 40, 11, 466, 466, true, false) == ESP_OK);
    set_report(23, 78);
    assert(horizontal.Poll(&point, raw) == ESP_OK && point.x == 442 && point.y == 78);

    using namespace amoled_touch_test;
    // Feed reflected hardware reports through the actual driver into the actual
    // five-target monitor, just as the offline firmware does.
    Monitor aligned;
    for (unsigned i = 0; i < kTargetCount; ++i) {
        set_report(465 - kTargets[i].x, 465 - kTargets[i].y);
        assert(oriented.Poll(&point, raw) == ESP_OK);
        assert(aligned.Observe(Read::kValid, point, i * 40) == Event::kDown);
        assert(aligned.NextTarget() == i + 1);
        set_report(0, 0, false);
        assert(oriented.Poll(&point, raw) == ESP_OK);
        assert(aligned.Observe(Read::kValid, point, i * 40 + 20) == Event::kUp);
    }
    assert(aligned.Complete() && aligned.Downs() == 5 && aligned.Ups() == 5);
    PointerGate pointer;
    assert(!pointer.Update(false, true, true) && pointer.Down(true));
    assert(pointer.Update(true, true, false) && !pointer.Down(true));
    assert(!pointer.Update(false, true, true) && !pointer.Down(true)); // held across fault: cannot click
    pointer.Update(false, false, true);
    assert(!pointer.Down(false));
    pointer.Update(false, true, true); assert(pointer.Down(true)); // genuine fresh press
    Monitor state;
    assert(state.Stale(0));
    auto sample = [&](bool down, int x, int y, int64_t now) {
        return state.Observe(Read::kValid, {down, static_cast<uint16_t>(x), static_cast<uint16_t>(y)}, now);
    };
    assert(sample(true, 233, 252, 0) == Event::kDown && state.NextTarget() == 1);
    assert(sample(true, 233, 194, 20) == Event::kMove && state.NextTarget() == 1); // dragging is not another tap
    assert(sample(true, 233, 194, 40) == Event::kNone && state.Downs() == 1);
    assert(state.Observe(Read::kInvalidReport, {}, 60) == Event::kNone && state.Pressed() && state.Ups() == 0);
    assert(state.Observe(Read::kBusError, {}, 80) == Event::kNone && state.Pressed() && state.Ups() == 0);
    assert(!state.Stale(289) && state.Stale(290));
    assert(sample(true, 233, 194, 300) == Event::kNone && state.NextTarget() == 1); // recovery does not duplicate DOWN
    assert(sample(false, 0, 0, 320) == Event::kUp && state.X() == 233 && state.Y() == 194);
    for (unsigned i = 1; i < kTargetCount; ++i) {
        assert(sample(true, kTargets[i].x, kTargets[i].y, 400 + i * 40) == Event::kDown);
        assert(sample(false, 0, 0, 420 + i * 40) == Event::kUp);
    }
    assert(state.Complete() && state.Downs() == 5 && state.Ups() == 5);
    assert(state.Invalid() == 1 && state.BusErrors() == 1);
    state.Reset();
    assert(state.Stale(1000) && state.Downs() == 0 && state.Valid() == 0 && !state.Pressed());
    assert(sample(true, 311, 252, 1000) == Event::kDown && state.NextTarget() == 0); // wrong target order
    sample(false, 0, 0, 1020);
    assert(sample(true, 233, 464, 1040) == Event::kDown && state.Y() == 464); // no coordinate clamp
    assert(!InCircle(state.X(), state.Y()) && TargetsFit());
    puts("AMOLED touch: actual I2C driver/reset/raw reports, five tap targets, drag/release, fault recovery and bounds passed.");
}
