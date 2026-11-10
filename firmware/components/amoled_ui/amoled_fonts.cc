#include "amoled_fonts.h"
#include "cjk_font.h"
#include "amoled_ui_layout.h"
#include "esp_log.h"

void AmoledFonts::Init() {
    if (initialized_) return;
    initialized_ = true;
    lv_font_t* body = LoadCjkFont("font_cjk", (AMOLED_UI_SIZE <= 360 ? 16 : 20), AMOLED_UI_SIZE <= 360 ? 128 : 96);
    lv_font_t* small = LoadCjkFont("font_cjk", 14, 48);
    lv_font_t* title = LoadCjkFont("font_cjk", (AMOLED_UI_SIZE <= 360 ? 18 : 24), AMOLED_UI_SIZE <= 360 ? 48 : 32);
#if AMOLED_UI_SIZE <= 360
    body_ = lv_font_montserrat_16;
    heading_ = lv_font_montserrat_16;
    title_ = lv_font_montserrat_18;
#else
    body_ = lv_font_montserrat_20;
    heading_ = lv_font_montserrat_20;
    title_ = lv_font_montserrat_24;
#endif
    small_ = lv_font_montserrat_14;
    body_.fallback = body ? body : &lv_font_simsun_14_cjk;
    small_.fallback = small ? small : &lv_font_simsun_14_cjk;
    heading_.fallback = body_.fallback;
    title_.fallback = title ? title : &lv_font_simsun_14_cjk;
    ready_ = body && small && title;
    if (!ready_) ESP_LOGW("Amoled175.font", "Shared Chinese font unavailable; flash app and font together");
}
