#include "../common/round_factory_application.h"
#include "boot_animation.h"

class SugaratFactoryApplication final : public AmoledFactoryApplication {
   public:
    SugaratFactoryApplication() : AmoledFactoryApplication(true, PlaySugaratBootAnimation) {}
};
DECLARE_APPLICATION(SugaratFactoryApplication);
