#pragma once
#include "button_hardware_stubs.h"
#include "sdgoods_sensors.h"
#include "sdgoods_audio.h"
class AmoledHardware {
   public:
    struct Panel {
        bool Ready() const {
            return true;
        }
        unsigned TeEdges() const {
            return 60;
        }
        unsigned TeTimeouts() const {
            return 0;
        }
    } panel_;
    struct Touch {
        bool Ready() const {
            return true;
        }
    } touch_;
    sdgoods::Sensors sensors_;
    sdgoods::Audio audio_;
    bool ButtonDown() const {
        return button_fixture::button;
    }
    int EnterSleep(bool off) {
        button_fixture::skip_wake_animation = !off;
        button_fixture::power_events.push_back(off ? "off" : "sleep");
        throw SleepSignal{off};
    }
};
