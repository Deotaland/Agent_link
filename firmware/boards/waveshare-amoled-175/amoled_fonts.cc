#include "amoled_fonts.h"
#include "cjk_font.h"
#include "esp_log.h"

void AmoledFonts::Init() {
    if (initialized_) return;
    initialized_ = true;
    lv_font_t* body = LoadCjkFont("anim_pack", 20, 96);
    lv_font_t* small = LoadCjkFont("anim_pack", 14, 48);
    lv_font_t* title = LoadCjkFont("anim_pack", 24, 32);
    body_ = lv_font_montserrat_20;
    small_ = lv_font_montserrat_14;
    heading_ = lv_font_montserrat_20;
    title_ = lv_font_montserrat_24;
    body_.fallback = body ? body : &lv_font_simsun_14_cjk;
    small_.fallback = small ? small : &lv_font_simsun_14_cjk;
    heading_.fallback = body_.fallback;
    title_.fallback = title ? title : &lv_font_simsun_14_cjk;
    ready_ = body && small && title;
    if (!ready_) ESP_LOGW("Amoled175.font", "Shared Chinese font unavailable; flash app and font together");
}
