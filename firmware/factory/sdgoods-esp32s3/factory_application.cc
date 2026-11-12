#include "../common/round_factory_application.h"

class SugaratFactoryApplication final : public AmoledFactoryApplication {
   public:
    SugaratFactoryApplication() : AmoledFactoryApplication(true) {}
};
DECLARE_APPLICATION(SugaratFactoryApplication);
