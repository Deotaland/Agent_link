/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Deotaland
 * Layout and palette inspired by suoyousheng-bot/sugarat-sdgoods, launcher c07957f.
 */
#include "factory_view.h"
#include "../factory_diagnostics.h"
#include "amoled_fonts.h"
#include "amoled_cover_cache.h"
#include "amoled_label_layout.h"
#include "agent_cover.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace amoled_factory {
namespace {
constexpr uint32_t kBg = 0x080D16, kCard = 0x202735, kText = 0xF3F0E8;
constexpr uint32_t kMuted = 0x8B9DB1, kAccent = 0x247D78, kBrand = 0x68D9D1;
constexpr amoled_ui::CardRenderer::Palette kPalette{kBg, kCard, kText, kMuted, kBrand, 0x303B49};
lv_obj_t *root, *home, *apps, *details, *busy, *download;
lv_obj_t *installed_label, *count_label, *free_label, *usage_ring, *usage_label, *status;
lv_obj_t *hero, *wifi_title, *wifi_step, *wifi_ap, *wifi_url, *wifi_arrow;
lv_obj_t *home_tiles[3], *home_names[3], *grid_tiles[4], *grid_names[4];
lv_obj_t *grid_title, *empty, *empty_text, *empty_button, *empty_button_text;
lv_obj_t *previous, *next, *page_label, *detail_name, *detail_version;
lv_obj_t *launch, *remove, *launch_text, *remove_text, *detail_back;
lv_obj_t *progress_ring, *progress_label, *busy_title, *busy_detail;
lv_obj_t *download_title, *download_hint, *code_boxes[6], *code_labels[6];
lv_obj_t *query_key, *query_text, *detail_hint;
lv_obj_t *diagnostics, *test_title, *test_hint, *test_info, *retry_text;
lv_obj_t *touch_plot, *touch_targets[5], *touch_numbers[5], *touch_dot, *touch_pass;
lv_obj_t *level_plot, *level_ball;
lv_obj_t* color_screen;
lv_obj_t *carousel, *carousel_name, *carousel_hint, *carousel_dots[15], *carousel_plus[2];
lv_obj_t *action_shade, *action_popup, *action_name, *action_version;
lv_obj_t *action_launch, *action_remove, *action_launch_text, *action_remove_text;
lv_obj_t *action_wait, *action_close;

void Show(lv_obj_t* obj, bool on) {
    if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
void Text(lv_obj_t* obj, const char* text) {
    if (strcmp(lv_label_get_text(obj), text)) {
        lv_label_set_text(obj, text);
        amoled_ui::CenterLabelVertically(obj);
    }
}
lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, int radius, uint32_t color) {
    auto* obj = lv_obj_create(parent); lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}
lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, int x, int y, int w, int h,
                uint32_t color, bool center = true) {
    auto* obj = lv_label_create(parent); lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    lv_obj_set_style_text_font(obj, font, 0); lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT); lv_label_set_text(obj, "");
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}
lv_obj_t* Ring(lv_obj_t* parent, int x, int y, int edge, int width, uint32_t track) {
    auto* obj = lv_arc_create(parent); lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, edge, edge);
    lv_arc_set_rotation(obj, 270); lv_arc_set_bg_angles(obj, 0, 360); lv_arc_set_range(obj, 0, 100);
    for (const auto part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
        lv_obj_set_style_arc_width(obj, width, part);
        lv_obj_set_style_arc_rounded(obj, true, part);
        lv_obj_set_style_arc_color(obj, lv_color_hex(part == LV_PART_MAIN ? track : kBrand), part);
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE); return obj;
}
lv_obj_t* Back(lv_obj_t* parent, void (*bind)(lv_obj_t*, int)) {
    auto* button = Box(parent, 62, 46, 44, 44, 22, kCard); bind(button, -1);
    Text(Label(button, &lv_font_montserrat_24, 0, 0, 44, 44, kText), LV_SYMBOL_LEFT);
    return button;
}

// Each visible image owns one cache entry: replacing a grid page cannot evict
// a descriptor that another LVGL image is still displaying.
struct CoverView {
    lv_obj_t *box = nullptr, *picture = nullptr, *glyph = nullptr;
    AmoledCoverCache cache{1};
    void Create(lv_obj_t* parent, int x, int y, int edge) {
        box = Box(parent, x, y, edge, edge, LV_RADIUS_CIRCLE, 0x443027);
        // Uploaded covers can be fully opaque squares; clip the child image too.
        lv_obj_set_style_clip_corner(box, true, 0);
        picture = lv_image_create(box); lv_obj_remove_flag(picture, LV_OBJ_FLAG_CLICKABLE);
        glyph = Label(box, &lv_font_montserrat_24, 0, 0, edge, edge, kBrand);
        Text(glyph, LV_SYMBOL_FILE);
    }
    void Update(int slot, uint32_t revision, int edge, const char* title) {
        auto source = agent_system::AcquireCover({slot, revision});
        // Detach before the bounded cache may replace the old source's pixels.
        lv_image_set_src(picture, nullptr);
        const auto* pixels = source ? cache.Get(source, edge) : nullptr;
        if (pixels) { lv_image_set_src(picture, pixels); lv_obj_center(picture); }
        Show(picture, pixels); Show(glyph, !pixels);
        uint32_t hash = 5381;
        for (const auto* p = reinterpret_cast<const unsigned char*>(title); *p; ++p) hash = hash * 33 + *p;
        constexpr uint32_t colors[]{0x443027, 0x2E3A6E, 0x493C58, 0x314934};
        lv_obj_set_style_bg_color(box, lv_color_hex(colors[hash % 4]), 0);
    }
} grid_covers[4], detail_cover, carousel_cover;

void Create(lv_obj_t* parent, AmoledFonts& fonts, void (*bind)(lv_obj_t*, int)) {
    root = Box(parent, 0, 0, 360, 360, 0, kBg);
    home = Box(root, 0, 0, 360, 360, 0, kBg);
    Text(Label(home, &lv_font_montserrat_16, 62, 30, 236, 24, kText), "Sugarat Launcher");
    hero = Box(home, 50, 64, 260, 96, 24, 0x443027); bind(hero, 3);
    installed_label = Label(hero, fonts.Small(), 18, 10, 145, 20, kBrand, false);
    count_label = Label(hero, &lv_font_montserrat_36, 18, 31, 145, 42, kText, false);
    free_label = Label(hero, fonts.Small(), 18, 73, 145, 20, kMuted, false);
    usage_ring = Ring(hero, 172, 10, 76, 7, 0x5A3E31);
    usage_label = Label(hero, &lv_font_montserrat_16, 178, 10, 64, 76, kText);
    wifi_title = Label(hero, fonts.Body(), 18, 6, 208, 24, kBrand, false);
    wifi_step = Label(hero, fonts.Small(), 18, 33, 224, 19, kText, false);
    wifi_ap = Label(hero, fonts.Small(), 18, 53, 224, 19, kText, false);
    wifi_url = Label(hero, fonts.Small(), 18, 73, 224, 19, kMuted, false);
    wifi_arrow = Label(hero, &lv_font_montserrat_14, 228, 6, 20, 24, kBrand);
    Text(wifi_arrow, LV_SYMBOL_RIGHT);
    const uint32_t colors[]{kAccent, 0x7F6CF0, 0x514B43};
    const char* symbols[]{LV_SYMBOL_LIST, LV_SYMBOL_DOWNLOAD, LV_SYMBOL_SETTINGS};
    for (int i = 0; i < 3; ++i) {
        home_tiles[i] = Box(home, 49 + 90 * i, 172, 82, 104, 22, kCard);
        bind(home_tiles[i], i);
        auto* badge = Box(home_tiles[i], 17, 12, 48, 48, 16, colors[i]);
        Text(Label(badge, &lv_font_montserrat_24, 0, 0, 48, 48, 0xFFFDF8), symbols[i]);
        home_names[i] = Label(home_tiles[i], fonts.Small(), 2, 73, 78, 24, kText);
    }
    status = Label(home, fonts.Small(), 67, 297, 226, 24, kMuted);

    apps = Box(root, 0, 0, 360, 360, 0, kBg); Back(apps, bind);
    grid_title = Label(apps, fonts.Title(), 108, 42, 177, 28, kText);
    for (int i = 0; i < 4; ++i) {
        grid_tiles[i] = Box(apps, 62 + (i % 2) * 122, 82 + (i / 2) * 112, 114, 104, 20, kCard);
        bind(grid_tiles[i], i); grid_covers[i].Create(grid_tiles[i], 25, 7, 64);
        grid_names[i] = Label(grid_tiles[i], fonts.Small(), 4, 77, 106, 22, kText);
    }
    previous = Box(apps, 108, 310, 36, 32, 16, kCard); bind(previous, 4);
    next = Box(apps, 216, 310, 36, 32, 16, kCard); bind(next, 5);
    Text(Label(previous, &lv_font_montserrat_16, 0, 0, 36, 32, kText), LV_SYMBOL_LEFT);
    Text(Label(next, &lv_font_montserrat_16, 0, 0, 36, 32, kText), LV_SYMBOL_RIGHT);
    page_label = Label(apps, &lv_font_montserrat_14, 145, 310, 70, 32, kMuted);
    empty = Box(apps, 60, 106, 240, 184, 24, kCard);
    Text(Label(empty, &lv_font_montserrat_36, 0, 16, 240, 46, kBrand), LV_SYMBOL_LIST);
    empty_text = Label(empty, fonts.Body(), 12, 76, 216, 30, kText);
    empty_button = Box(empty, 44, 123, 152, 44, 22, kAccent); bind(empty_button, 0);
    empty_button_text = Label(empty_button, fonts.Body(), 0, 0, 152, 44, kText);

    details = Box(root, 0, 0, 360, 360, 0, kBg); detail_back = Back(details, bind);
    Text(Label(details, fonts.Small(), 109, 45, 177, 24, kMuted), "Sugarat Launcher");
    detail_cover.Create(details, 128, 84, 104);
    detail_name = Label(details, fonts.Title(), 60, 196, 240, 44, kText);
    lv_label_set_long_mode(detail_name, LV_LABEL_LONG_WRAP);
    detail_version = Label(details, fonts.Small(), 80, 245, 200, 20, kMuted);
    detail_hint = Label(details, fonts.Small(), 60, 228, 240, 36, kMuted);
    lv_label_set_long_mode(detail_hint, LV_LABEL_LONG_WRAP);
    launch = Box(details, 64, 270, 112, 44, 22, kAccent); bind(launch, 0);
    remove = Box(details, 184, 270, 112, 44, 22, kCard); bind(remove, 1);
    launch_text = Label(launch, fonts.Body(), 0, 0, 112, 44, kText);
    remove_text = Label(remove, fonts.Body(), 0, 0, 112, 44, kMuted);

    download = Box(root, 0, 0, 360, 360, 0, kBg); Back(download, bind);
    download_title = Label(download, fonts.Title(), 111, 43, 140, 28, kText);
    auto* help = Box(download, 254, 40, 36, 36, 18, kCard); bind(help, 12);
    lv_obj_set_ext_click_area(help, 10);
    Text(Label(help, &lv_font_montserrat_20, 0, 0, 36, 36, kBrand), "?");
    download_hint = Label(download, fonts.Small(), 52, 82, 256, 23, kMuted);
    for (int i = 0; i < 6; ++i) {
        code_boxes[i] = Box(download, 58 + i * 41, 111, 38, 44, 10, kCard);
        lv_obj_set_style_border_width(code_boxes[i], 1, 0);
        code_labels[i] = Label(code_boxes[i], &lv_font_montserrat_24, 0, 0, 38, 44, kText);
    }
    const int keys[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
    for (int i = 0; i < 12; ++i) {
        auto* key = Box(download, 67 + (i % 3) * 78, 168 + (i / 3) * 41, 70, 36, 16,
                        keys[i] == 11 ? kAccent : kCard);
        bind(key, keys[i]);
        auto* label = Label(key, keys[i] == 11 ? fonts.Small() : &lv_font_montserrat_20,
                            0, 0, 70, 36, kText);
        char digit[2]{static_cast<char>('0' + keys[i]), 0};
        Text(label, keys[i] == 10 ? LV_SYMBOL_BACKSPACE : keys[i] == 11 ? "" : digit);
        if (keys[i] == 11) { query_key = key; query_text = label; }
    }

    busy = Box(root, 0, 0, 360, 360, 0, kBg);
    progress_ring = Ring(busy, 46, 46, 268, 8, 0x38352F);
    progress_label = Label(busy, &lv_font_montserrat_36, 80, 130, 200, 48, kText);
    busy_title = Label(busy, fonts.Title(), 70, 188, 220, 56, kText);
    lv_label_set_long_mode(busy_title, LV_LABEL_LONG_WRAP);
    busy_detail = Label(busy, fonts.Small(), 78, 244, 204, 44, kMuted);
    lv_label_set_long_mode(busy_detail, LV_LABEL_LONG_WRAP);
    diagnostics = Box(root, 0, 0, 360, 360, 0, kBg);
    Back(diagnostics, bind);
    test_title = Label(diagnostics, fonts.Title(), 108, 42, 180, 28, kText);
    test_hint = Label(diagnostics, fonts.Small(), 52, 78, 256, 36, kMuted);
    lv_label_set_long_mode(test_hint, LV_LABEL_LONG_WRAP);
    touch_plot = Box(diagnostics, 0, 108, 360, 180, 0, kBg);
    for (int i = 0; i < 5; ++i) {
        touch_targets[i] = Box(touch_plot, sugarat_factory::kTouchTargets[i][0]-24,
                              sugarat_factory::kTouchTargets[i][1]-108-24, 48, 48, 24, kCard);
        lv_obj_set_style_border_width(touch_targets[i], 2, 0);
        touch_numbers[i] = Label(touch_targets[i], &lv_font_montserrat_16, 0, 0, 48, 48, kText);
        char number[2]{char('1'+i),0}; Text(touch_numbers[i], number);
    }
    // The touch marker may be anywhere on the physical display, outside the plot.
    touch_dot = Box(diagnostics, 0, 0, 10, 10, 5, kText);
    touch_pass = Label(touch_plot, fonts.Title(), 50, 45, 260, 40, 0x72C69C);
    level_plot = Box(diagnostics, 100, 125, 160, 160, 80, kCard);
    lv_obj_set_style_border_color(level_plot, lv_color_hex(kMuted), 0);
    lv_obj_set_style_border_width(level_plot, 1, 0);
    Box(level_plot, 79, 5, 1, 150, 0, 0x514B43);
    Box(level_plot, 5, 79, 150, 1, 0, 0x514B43);
    auto* center = Box(level_plot, 64, 64, 32, 32, 16, kCard);
    lv_obj_set_style_border_width(center, 2, 0);
    lv_obj_set_style_border_color(center, lv_color_hex(0x72C69C), 0);
    level_ball = Box(level_plot, 70, 70, 20, 20, 10, kBrand);
    test_info = Label(diagnostics, fonts.Small(), 60, 293, 240, 22, kText);
    auto* retry = Box(diagnostics, 122, 320, 116, 30, 15, kCard); bind(retry, 0);
    retry_text = Label(retry, fonts.Small(), 0, 0, 116, 30, kText);
    carousel = Box(root, 0, 0, 360, 360, 0, 0x080D16);
    Box(carousel, 163, 19, 34, 3, 2, 0x526173);
    carousel_cover.Create(carousel, 64, 52, 232);
    bind(carousel_cover.box, 0);
    carousel_plus[0] = Box(carousel_cover.box, 59, 110, 114, 12, 6, 0xD8F7A4);
    carousel_plus[1] = Box(carousel_cover.box, 110, 59, 12, 114, 6, 0xD8F7A4);
    lv_obj_set_style_text_font(carousel_cover.glyph, &lv_font_montserrat_48, 0);
    lv_label_set_long_mode(carousel_cover.glyph, LV_LABEL_LONG_WRAP);
    carousel_name = Label(carousel, fonts.Title(), 75, 290, 210, 28, kText);
    carousel_hint = Label(carousel, fonts.Small(), 91, 318, 178, 20, kMuted);
    bind(carousel_hint, 3);
    for (auto& dot : carousel_dots)
        dot = Box(carousel, 0, 337, 4, 4, 2, 0x526173);
    action_shade = Box(carousel, 0, 0, 360, 360, 0, 0);
    lv_obj_set_style_bg_opa(action_shade, LV_OPA_60, 0);
    bind(action_shade, -1);
    action_popup = Box(action_shade, 46, 91, 268, 184, 26, 0x202735);
    action_name = Label(action_popup, fonts.Title(), 24, 20, 220, 36, kText);
    action_close = Box(action_popup, 224, 10, 32, 32, 16, 0x303B49);
    bind(action_close, -1);
    Text(Label(action_close, &lv_font_montserrat_20, 0, 0, 32, 32, kText), LV_SYMBOL_CLOSE);
    action_version = Label(action_popup, fonts.Small(), 24, 61, 220, 32, kMuted);
    action_launch = Box(action_popup, 18, 116, 110, 46, 23, 0xD8F7A4);
    action_remove = Box(action_popup, 140, 116, 110, 46, 23, 0x472932);
    bind(action_launch, 0);
    bind(action_remove, 1);
    action_launch_text = Label(action_launch, fonts.Body(), 0, 0, 110, 46, 0x162218);
    action_remove_text = Label(action_remove, fonts.Body(), 0, 0, 110, 46, 0xFFC0C8);
    action_wait = Label(action_popup, &lv_font_montserrat_36, 110, 122, 48, 44, kBrand);
    Text(action_wait, LV_SYMBOL_REFRESH);
    Show(action_wait, false);
    color_screen = Box(root, 0, 0, 360, 360, 0, 0x000000);
    Show(root, false);
}
bool Accepts(const amoled_ui::Screen& screen) {
    return screen.kind == amoled_ui::View::kCarousel ||
           screen.kind == amoled_ui::View::kAppRemove ||
           screen.kind == amoled_ui::View::kHardwareTest ||
           screen.kind == amoled_ui::View::kFactoryHome ||
           screen.kind == amoled_ui::View::kAppGrid ||
           screen.kind == amoled_ui::View::kDownloadCode ||
           screen.kind == amoled_ui::View::kDownloadDetail ||
           screen.kind == amoled_ui::View::kAppActions ||
           (screen.kind == amoled_ui::View::kInfo && screen.busy);
}
void Update(const amoled_ui::Screen& screen) {
    using amoled_ui::Tr;
    const bool is_home = screen.kind == amoled_ui::View::kFactoryHome;
    const bool is_grid = screen.kind == amoled_ui::View::kAppGrid;
    const bool is_busy = screen.kind == amoled_ui::View::kInfo && screen.busy;
    const bool is_download = screen.kind == amoled_ui::View::kDownloadCode;
    const bool is_test = screen.kind == amoled_ui::View::kHardwareTest;
    const bool is_colors =
        is_test && screen.hardware_test.kind == amoled_ui::HardwareTest::Kind::kColors;
    const bool is_carousel = screen.kind == amoled_ui::View::kCarousel ||
                             screen.kind == amoled_ui::View::kAppActions ||
                             screen.kind == amoled_ui::View::kAppRemove;
    Show(root, true); Show(home, is_home); Show(apps, is_grid);
    Show(busy, is_busy); Show(download, is_download);
    Show(details, !is_home && !is_grid && !is_busy && !is_download && !is_test && !is_carousel);
    Show(diagnostics, is_test && !is_colors);
    Show(color_screen, is_colors);
    Show(carousel, is_carousel);
    char text[96];
    if (is_colors) {
        constexpr uint32_t colors[]{0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0xD8B4FE};
        static_assert(sizeof colors / sizeof colors[0] == amoled_ui::HardwareTest::kColorCount);
        lv_obj_set_style_bg_color(
            color_screen, lv_color_hex(colors[std::clamp(screen.hardware_test.color_index, 0,
                                                       amoled_ui::HardwareTest::kColorCount - 1)]),
            0);
    } else if (is_carousel) {
        const bool actions = screen.kind != amoled_ui::View::kCarousel;
        const bool removing = screen.kind == amoled_ui::View::kAppRemove;
        carousel_cover.Update(screen.add_card ? -1 : screen.cover_slot, screen.cover_revision, 232,
                              screen.title);
        if (screen.add_card) {
            Show(carousel_cover.picture, false);
            Show(carousel_cover.glyph, false);
        } else if (!strcmp(screen.app_id, "agent.status") ||
                   !strcmp(screen.app_id, "agent.voice")) {
            lv_obj_set_style_text_font(carousel_cover.glyph, &lv_font_montserrat_36, 0);
            Text(carousel_cover.glyph, "AGENT\nLINK");
            lv_obj_set_style_bg_color(carousel_cover.box, lv_color_hex(0x163842), 0);
        } else {
            lv_obj_set_style_text_font(carousel_cover.glyph, &lv_font_montserrat_48, 0);
            Text(carousel_cover.glyph, LV_SYMBOL_FILE);
        }
        for (auto* plus : carousel_plus)
            Show(plus, screen.add_card);
        amoled_ui::CenterLabelVertically(carousel_cover.glyph);
        Text(carousel_name, removing ? screen.detail : screen.title);
        Text(carousel_hint, screen.job_status[0] ? screen.job_status
                            : !screen.wifi_connected
                                ? Tr("未联网 · 点此配网", "Offline - set up WiFi")
                                : Tr("左右滑动 · 轻点管理", "Swipe to browse - tap to manage"));
        const int count = std::min<size_t>(screen.card_count, 15);
        for (int i = 0; i < 15; ++i) {
            Show(carousel_dots[i], i < count);
            const bool selected = i == int(screen.card_index);
            lv_obj_set_pos(carousel_dots[i], 180 - (count * 9 - 5) / 2 + i * 9, 337);
            lv_obj_set_style_bg_color(carousel_dots[i],
                                      lv_color_hex(selected ? 0xD8F7A4 : 0x526173), 0);
        }
        Show(action_shade, actions);
        if (actions) {
            Show(action_launch, !screen.launching);
            Show(action_remove, !screen.launching);
            Show(action_wait, screen.launching);
            Show(action_close, !screen.launching);
            lv_obj_set_x(action_name, screen.launching ? 24 : 46);
            lv_obj_set_width(action_name, screen.launching ? 220 : 176);
            lv_obj_set_height(action_version, screen.launching ? 56 : 32);
            lv_label_set_long_mode(action_version, screen.launching ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
            Text(action_name, screen.launching ? Tr("正在启动应用…", "Starting app...")
                             : removing ? Tr("删除这个应用？", "Delete this app?") : screen.title);
            Text(action_version, screen.launching ? Tr("请稍候，设备正在重启。", "Please wait. The device is restarting.") : screen.detail);
            Text(action_launch_text, removing ? Tr("取消", "Cancel") : Tr("启动", "Launch"));
            Text(action_remove_text, removing ? Tr("确认删除", "Delete") : Tr("删除", "Delete"));
        }
    } else if (is_test) {
        const auto& test = screen.hardware_test;
        const bool touch = test.kind == amoled_ui::HardwareTest::Kind::kTouch;
        Text(test_title, screen.title); Text(test_hint, screen.detail);
        Text(test_info, !touch && !test.valid ? "X --  Y --" : screen.info);
        Text(retry_text, Tr("重新测试", "Restart test"));
        lv_obj_set_style_text_color(test_hint, lv_color_hex(test.passed ? 0x72C69C : kMuted), 0);
        Show(touch_plot, touch); Show(level_plot, !touch);
        Show(touch_dot, touch && test.x >= 0);
        if (touch) {
            lv_obj_set_pos(touch_dot, test.x-5, test.y-5);
            Show(touch_pass, test.passed);
            Text(touch_pass, Tr("触摸测试通过", "Touch test passed"));
            for (int i = 0; i < 5; ++i) {
                Show(touch_targets[i], !test.passed);
                const auto color = i < test.completed ? 0x72C69C : i == test.completed ? kBrand : 0x514B43;
                lv_obj_set_style_border_color(touch_targets[i], lv_color_hex(color), 0);
                lv_obj_set_style_text_color(touch_numbers[i], lv_color_hex(color), 0);
            }
        } else {
            Show(level_ball, test.valid);
            float x = test.tilt_x * 4, y = test.tilt_y * 4;
            const float radius = std::sqrt(x*x+y*y);
            if (radius > 62) { x *= 62/radius; y *= 62/radius; }
            lv_obj_set_pos(level_ball, 70+int(std::lround(x)), 70+int(std::lround(y)));
            lv_obj_set_style_bg_color(level_ball, lv_color_hex(test.centered ? 0x72C69C : kBrand), 0);
        }
    } else if (is_download) {
        Text(download_title, Tr("下载应用", "Download"));
        Text(download_hint, Tr("输入平台上的六位下载码", "Enter the 6-digit download code"));
        const size_t length = strlen(screen.download_code);
        for (int i = 0; i < 6; ++i) {
            char digit[2]{i < int(length) ? screen.download_code[i] : char(0), 0};
            Text(code_labels[i], digit);
            lv_obj_set_style_border_color(code_boxes[i], lv_color_hex(i == int(length) ? kBrand : kCard), 0);
        }
        Text(query_text, Tr("查询", "Find"));
        lv_obj_set_style_bg_color(query_key, lv_color_hex(length == 6 ? kAccent : kCard), 0);
        if (length == 6) lv_obj_remove_state(query_key, LV_STATE_DISABLED);
        else lv_obj_add_state(query_key, LV_STATE_DISABLED);
    } else if (is_home) {
        const bool guide = !screen.wifi_connected;
        for (auto* obj : {installed_label, count_label, free_label, usage_ring, usage_label}) Show(obj, !guide);
        for (auto* obj : {wifi_title, wifi_step, wifi_ap, wifi_url, wifi_arrow}) Show(obj, guide);
        if (guide) lv_obj_add_flag(hero, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(hero, LV_OBJ_FLAG_CLICKABLE);
        Text(wifi_title, screen.wifi_setup_active ? Tr("请先完成配网", "Set up WiFi first")
                                                 : Tr("请先连接 WiFi", "Connect WiFi first"));
        Text(wifi_step, screen.wifi_setup_active ? Tr("1 手机连接设备热点", "1 Join the device hotspot")
                                                : Tr("下载应用需要联网", "Downloads need WiFi"));
        Text(wifi_ap, screen.wifi_setup_active
            ? (screen.wifi_setup_ssid[0] ? screen.wifi_setup_ssid : Tr("热点启动中…", "Starting hotspot..."))
            : Tr("点击进入设置 → 网络与配网", "Tap for network settings"));
        Text(wifi_url, screen.wifi_setup_active ? Tr("2 打开 192.168.4.1", "2 Open 192.168.4.1")
                                               : Tr("已安装应用仍可使用", "Installed apps stay available"));
        Text(installed_label, Tr("已安装应用", "Installed apps"));
        snprintf(text, sizeof text, "%u", unsigned(screen.card_count)); Text(count_label, text);
        const auto used = std::min(screen.storage_used, screen.storage_total);
        snprintf(text, sizeof text, Tr("可用 %.1f MB", "Free %.1f MB"), (screen.storage_total - used) / 1048576.0);
        Text(free_label, text);
        const int percent = screen.storage_total ? uint64_t(used) * 100 / screen.storage_total : 0;
        lv_arc_set_value(usage_ring, percent);
        snprintf(text, sizeof text, "%d%%", percent); Text(usage_label, text);
        Text(home_names[0], Tr("我的应用", "My apps"));
        Text(home_names[1], Tr("下载应用", "Download")); Text(home_names[2], Tr("设置", "Settings"));
        char battery[12];
        if (screen.battery_percent >= 0) snprintf(battery, sizeof battery, "%d%%", screen.battery_percent);
        else snprintf(battery, sizeof battery, "--%%");
        snprintf(text, sizeof text, "%s  ·  %s", battery, screen.wifi_connected ? "WiFi"
                 : screen.wifi_setup_active ? Tr("等待配网", "WiFi setup") : Tr("未连接 WiFi", "No WiFi"));
        Text(status, screen.job_status[0] ? screen.job_status : text);
    } else if (is_grid) {
        Text(grid_title, screen.title);
        for (int i = 0; i < 4; ++i) {
            Show(grid_tiles[i], i < screen.row_count);
            if (i >= screen.row_count) continue;
            Text(grid_names[i], screen.rows[i]);
            grid_covers[i].Update(screen.grid[i].cover_slot, screen.grid[i].cover_revision, 64, screen.rows[i]);
        }
        Show(empty, screen.row_count == 0);
        Text(empty_text, Tr("还没有应用", "No apps installed"));
        Text(empty_button_text, Tr("去下载", "Find apps"));
        Show(previous, screen.card_index > 0); Show(next, screen.card_index + 1 < screen.card_count);
        Show(page_label, screen.row_count > 0);
        snprintf(text, sizeof text, "%u / %u", unsigned(screen.card_index + 1), unsigned(screen.card_count));
        Text(page_label, text);
    } else if (is_busy) {
        lv_arc_set_value(progress_ring, screen.progress < 0 ? 0 : std::min(100, screen.progress));
        if (screen.progress >= 0) snprintf(text, sizeof text, "%d%%", std::min(100, screen.progress));
        else snprintf(text, sizeof text, "...");
        Text(progress_label, text); Text(busy_title, screen.title);
        Text(busy_detail, screen.progress >= 0 ? screen.info : screen.detail);
    } else {
        const bool actions = screen.kind == amoled_ui::View::kAppActions;
        lv_obj_set_y(detail_name, actions ? 196 : 194);
        lv_obj_set_height(detail_name, actions ? 44 : 24);
        lv_label_set_long_mode(detail_name, actions ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
        Text(detail_name, screen.title); Text(detail_version, screen.detail);
        detail_cover.Update(actions ? screen.cover_slot : -1, screen.cover_revision, 104, screen.title);
        Show(detail_version, actions); Show(detail_hint, !actions);
        amoled_ui::CenterLabelVertically(detail_name);
        Text(detail_hint, screen.detail);
        Show(remove, true); Show(detail_back, true);
        Text(launch_text, actions ? Tr("启动", "Open") : screen.rows[0]);
        Text(remove_text, actions ? Tr("卸载", "Remove") : Tr("返回", "Back"));
    }
}
void Hide() { Show(root, false); }
bool CaptureTestTap(int x, int y) {
    return y >= 80 && !(x >= 122 && x < 238 && y >= 320 && y < 350);
}
}  // namespace
const amoled_ui::CardRenderer& Renderer() {
    static const amoled_ui::CardRenderer renderer{
        Create, Update, Hide, Accepts, &kPalette, CaptureTestTap, {true, 18, 10, true, true}};
    return renderer;
}
}  // namespace amoled_factory
