#pragma once
#include <memory>
#include "amoled_system_menu.h"
class AmoledHardware;
// Optional board Factory pages. Hardware drivers remain in boards/; these
// services own page state and never touch LVGL from the control/audio workers.
class FactoryHardwareServices {
   public:
    virtual ~FactoryHardwareServices() = default;
    virtual AmoledSystemMenu::ExtraPage Menu() = 0;
    virtual bool Tick(bool can_power_down) = 0;
    virtual void Decorate(amoled_ui::Screen&) = 0;
    virtual void DeviceInfo(char*, size_t) = 0;
    virtual bool SetVolume(int) = 0;
    virtual bool Button(bool, int64_t) = 0;
    virtual bool HighLoad() const {
        return false;
    }
    virtual bool StartsHighLoad(const amoled_ui::Input&) const { return false; }
};
std::unique_ptr<FactoryHardwareServices> CreateFactoryHardwareServices(AmoledHardware&);
