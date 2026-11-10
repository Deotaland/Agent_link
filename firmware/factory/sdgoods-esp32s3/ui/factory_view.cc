#include "factory_view.h"
#include "amoled_fonts.h"
#include "amoled_cover_cache.h"
#include "agent_cover.h"
#include <cstdio>
#include <cstring>

namespace amoled_factory {
namespace {
// Native 360px layout; text and controls stay inside the round safe area.
lv_obj_t *root, *card, *picture, *glyph, *name, *subtitle, *eyebrow;
lv_obj_t *launch, *remove, *back, *launch_text, *remove_text, *dots[4];
std::shared_ptr<const agent_system::CoverPixels> image;
AmoledCoverCache covers;
size_t last_index = size_t(-1);
bool was_actions = false;
void Show(lv_obj_t* obj, bool on) {
    if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void Text(lv_obj_t* obj, const char* text) {
    if (strcmp(lv_label_get_text(obj), text)) lv_label_set_text(obj, text);
}
lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, int radius, uint32_t color) {
    auto* obj = lv_obj_create(parent); lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE); lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}
lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, int x, int y, int w, int h, uint32_t color) {
    auto* obj = lv_label_create(parent); lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, font, 0); lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}
void Create(lv_obj_t* parent, AmoledFonts& fonts, void (*bind)(lv_obj_t*, int)) {
    root = Box(parent, 0, 0, 360, 360, 0, 0x080D16);
    Box(root, 163, 24, 34, 3, 2, 0x526173);
    eyebrow = Label(root, fonts.Small(), 90, 38, 180, 20, 0x68D9D1);
    card = Box(root, 81, 70, 198, 198, LV_RADIUS_CIRCLE, 0x172738);
    lv_obj_set_style_border_width(card, 2, 0); lv_obj_set_style_border_color(card, lv_color_hex(0x3E6B7A), 0);
    bind(card, 0);
    picture = lv_image_create(card); lv_obj_remove_flag(picture, LV_OBJ_FLAG_CLICKABLE);
    glyph = Label(card, &lv_font_montserrat_48, 0, 0, 100, 60, 0x9DECE5); lv_obj_center(glyph);
    name = Label(root, fonts.Title(), 65, 280, 230, 26, 0xF0F6FC);
    subtitle = Label(root, fonts.Small(), 75, 306, 210, 20, 0x8B9DB1);
    back = Box(root, 66, 77, 36, 36, 18, 0x1A293C); bind(back, -1);
    auto* arrow = Label(back, &lv_font_montserrat_20, 0, 6, 36, 26, 0xB9C8D8); Text(arrow, LV_SYMBOL_LEFT);
    launch = Box(root, 64, 269, 112, 44, 22, 0x8DE5DB); bind(launch, 0);
    remove = Box(root, 184, 269, 112, 44, 22, 0x321E2C); bind(remove, 1);
    launch_text = Label(launch, fonts.Body(), 0, 11, 112, 24, 0x112D32);
    remove_text = Label(remove, fonts.Body(), 0, 11, 112, 24, 0xFFA3B4);
    for (auto& dot : dots) dot = Box(root, 0, 334, 5, 5, 3, 0x35465A);
    Show(root, false);
}
void Update(const amoled_ui::Screen& screen) {
    Show(root, true);
    const bool actions = screen.kind == amoled_ui::View::kAppActions;
    const int edge = actions ? 152 : 198;
    lv_obj_set_pos(card, (360-edge)/2, actions ? 68 : 70); lv_obj_set_size(card, edge, edge);
    if (actions) lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE); else lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    Text(eyebrow, "Agent_link Launcher");
    Text(name, screen.title); lv_obj_set_y(name, actions ? 231 : 280);
    Text(subtitle, actions ? screen.detail : amoled_ui::Tr("左右滑动 · 轻触打开", "Swipe to browse · Tap to open"));
    lv_obj_set_y(subtitle, actions ? 319 : 306);
    Text(launch_text, amoled_ui::Tr("启动", "Launch")); Text(remove_text, amoled_ui::Tr("卸载", "Remove"));
    Show(launch, actions); Show(remove, actions); Show(back, actions);
    auto next = screen.add_card ? nullptr : agent_system::AcquireCover({screen.cover_slot, screen.cover_revision});
    Show(picture, bool(next)); Show(glyph, !next);
    if (next) {
        image = next;
        const auto* cached = covers.Get(image, edge);
        if (cached) {
            if (lv_image_get_src(picture) != cached) lv_image_set_src(picture, cached);
            lv_obj_center(picture);
        }
        else { lv_image_set_src(picture, nullptr); Show(picture, false); Show(glyph, true); }
    } else if (!next && image) { lv_image_set_src(picture, nullptr); image.reset(); }
    Text(glyph, screen.add_card ? LV_SYMBOL_PLUS : LV_SYMBOL_FILE); lv_obj_center(glyph);
    const int count = std::min<size_t>(4, screen.card_count);
    int x = (360 - (count * 5 + (count-1)*8 + 11))/2;
    for (int i=0; i<4; ++i) {
        const bool selected = size_t(i) == screen.card_index;
        Show(dots[i], !actions && i<count); lv_obj_set_pos(dots[i], x, 334);
        lv_obj_set_width(dots[i], selected ? 16 : 5);
        lv_obj_set_style_bg_color(dots[i], lv_color_hex(selected ? 0x8DE5DB : 0x35465A), 0);
        x += (selected ? 16 : 5) + 8;
    }
    if (!actions && !was_actions && last_index != size_t(-1) && last_index != screen.card_index) {
        lv_anim_delete(card, nullptr); lv_anim_t animation; lv_anim_init(&animation);
        lv_anim_set_var(&animation, card); lv_anim_set_values(&animation, screen.card_index > last_index ? 14 : -14, 0);
        lv_anim_set_duration(&animation, 160); lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&animation, [](void* obj, int32_t x) { lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(obj), x, 0); });
        lv_anim_start(&animation);
    } else if (actions || was_actions) { lv_anim_delete(card, nullptr); lv_obj_set_style_translate_x(card, 0, 0); }
    last_index = screen.card_index; was_actions = actions;
}
void Hide() { Show(root, false); }
}
const amoled_ui::CardRenderer& Renderer() { static const amoled_ui::CardRenderer renderer{Create, Update, Hide}; return renderer; }
}
