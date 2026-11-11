#include "../common/round_factory_application.h"
std::unique_ptr<FactoryHardwareServices> CreateFactoryHardwareServices(AmoledHardware&) {
    return {};
}

DECLARE_APPLICATION(AmoledFactoryApplication);
