/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Deotaland
 * Layout and palette inspired by suoyousheng-bot/sugarat-sdgoods, launcher c07957f.
 */
#include "factory_view.h"
#include "amoled_fonts.h"
#include "amoled_cover_cache.h"
#include "agent_cover.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace amoled_factory {
namespace {
constexpr uint32_t kBg = 0x1D1D1B, kCard = 0x2B2925, kText = 0xF3F0E8;
constexpr uint32_t kMuted = 0xAAA49B, kAccent = 0xC54B2C, kBrand = 0xF29A72;
constexpr amoled_ui::CardRenderer::Palette kPalette{kBg, kCard, kText, kMuted, kBrand, 0x514B43};
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
    auto* button = Box(parent, 70, 40, 36, 36, 18, kCard); bind(button, -1);
    Text(Label(button, &lv_font_montserrat_20, 0, 8, 36, 24, kText), LV_SYMBOL_LEFT);
    return button;
}

// Each visible image owns one cache entry: replacing a grid page cannot evict
// a descriptor that another LVGL image is still displaying.
struct CoverView {
    lv_obj_t *box = nullptr, *picture = nullptr, *glyph = nullptr;
    AmoledCoverCache cache{1};
    void Create(lv_obj_t* parent, int x, int y, int edge) {
        box = Box(parent, x, y, edge, edge, 18, 0x443027);
        picture = lv_image_create(box); lv_obj_remove_flag(picture, LV_OBJ_FLAG_CLICKABLE);
        glyph = Label(box, &lv_font_montserrat_24, 0, 0, edge, 30, kBrand);
        Text(glyph, LV_SYMBOL_FILE); lv_obj_center(glyph);
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
} grid_covers[4], detail_cover;

void Create(lv_obj_t* parent, AmoledFonts& fonts, void (*bind)(lv_obj_t*, int)) {
    root = Box(parent, 0, 0, 360, 360, 0, kBg);
    home = Box(root, 0, 0, 360, 360, 0, kBg);
    Text(Label(home, &lv_font_montserrat_16, 62, 30, 236, 24, kText), "Agent_link Launcher");
    hero = Box(home, 50, 64, 260, 96, 24, 0x443027); bind(hero, 3);
    installed_label = Label(hero, fonts.Small(), 18, 10, 145, 20, kBrand, false);
    count_label = Label(hero, &lv_font_montserrat_36, 18, 31, 145, 42, kText, false);
    free_label = Label(hero, fonts.Small(), 18, 73, 145, 20, kMuted, false);
    usage_ring = Ring(hero, 172, 10, 76, 7, 0x5A3E31);
    usage_label = Label(hero, &lv_font_montserrat_16, 178, 37, 64, 24, kText);
    wifi_title = Label(hero, fonts.Body(), 18, 6, 208, 24, kBrand, false);
    wifi_step = Label(hero, fonts.Small(), 18, 33, 224, 19, kText, false);
    wifi_ap = Label(hero, fonts.Small(), 18, 53, 224, 19, kText, false);
    wifi_url = Label(hero, fonts.Small(), 18, 73, 224, 19, kMuted, false);
    wifi_arrow = Label(hero, &lv_font_montserrat_14, 228, 12, 20, 20, kBrand);
    Text(wifi_arrow, LV_SYMBOL_RIGHT);
    const uint32_t colors[]{kAccent, 0x7F6CF0, 0x514B43};
    const char* symbols[]{LV_SYMBOL_LIST, LV_SYMBOL_DOWNLOAD, LV_SYMBOL_SETTINGS};
    for (int i = 0; i < 3; ++i) {
        home_tiles[i] = Box(home, 49 + 90 * i, 172, 82, 104, 22, kCard);
        bind(home_tiles[i], i);
        auto* badge = Box(home_tiles[i], 17, 12, 48, 48, 16, colors[i]);
        Text(Label(badge, &lv_font_montserrat_24, 0, 11, 48, 30, 0xFFFDF8), symbols[i]);
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
    Text(Label(previous, &lv_font_montserrat_16, 0, 7, 36, 22, kText), LV_SYMBOL_LEFT);
    Text(Label(next, &lv_font_montserrat_16, 0, 7, 36, 22, kText), LV_SYMBOL_RIGHT);
    page_label = Label(apps, &lv_font_montserrat_14, 145, 316, 70, 22, kMuted);
    empty = Box(apps, 60, 106, 240, 184, 24, kCard);
    Text(Label(empty, &lv_font_montserrat_36, 0, 16, 240, 46, kBrand), LV_SYMBOL_LIST);
    empty_text = Label(empty, fonts.Body(), 12, 76, 216, 30, kText);
    empty_button = Box(empty, 44, 123, 152, 44, 22, kAccent); bind(empty_button, 0);
    empty_button_text = Label(empty_button, fonts.Body(), 0, 10, 152, 26, kText);

    details = Box(root, 0, 0, 360, 360, 0, kBg); detail_back = Back(details, bind);
    Text(Label(details, fonts.Small(), 109, 45, 177, 24, kMuted), "Agent_link Launcher");
    detail_cover.Create(details, 128, 84, 104);
    detail_name = Label(details, fonts.Title(), 60, 201, 240, 40, kText);
    lv_label_set_long_mode(detail_name, LV_LABEL_LONG_WRAP);
    detail_version = Label(details, fonts.Small(), 80, 245, 200, 20, kMuted);
    detail_hint = Label(details, fonts.Small(), 60, 214, 240, 48, kMuted);
    lv_label_set_long_mode(detail_hint, LV_LABEL_LONG_WRAP);
    launch = Box(details, 64, 270, 112, 44, 22, kAccent); bind(launch, 0);
    remove = Box(details, 184, 270, 112, 44, 22, kCard); bind(remove, 1);
    launch_text = Label(launch, fonts.Body(), 0, 10, 112, 26, kText);
    remove_text = Label(remove, fonts.Body(), 0, 10, 112, 26, kMuted);

    download = Box(root, 0, 0, 360, 360, 0, kBg); Back(download, bind);
    download_title = Label(download, fonts.Title(), 111, 43, 140, 28, kText);
    auto* help = Box(download, 254, 40, 36, 36, 18, kCard); bind(help, 12);
    Text(Label(help, &lv_font_montserrat_20, 0, 7, 36, 26, kBrand), "?");
    download_hint = Label(download, fonts.Small(), 52, 82, 256, 23, kMuted);
    for (int i = 0; i < 6; ++i) {
        code_boxes[i] = Box(download, 58 + i * 41, 111, 38, 44, 10, kCard);
        lv_obj_set_style_border_width(code_boxes[i], 1, 0);
        code_labels[i] = Label(code_boxes[i], &lv_font_montserrat_24, 0, 7, 38, 30, kText);
    }
    const int keys[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
    for (int i = 0; i < 12; ++i) {
        auto* key = Box(download, 67 + (i % 3) * 78, 168 + (i / 3) * 41, 70, 36, 16,
                        keys[i] == 11 ? kAccent : kCard);
        bind(key, keys[i]);
        auto* label = Label(key, keys[i] == 11 ? fonts.Small() : &lv_font_montserrat_20,
                            0, 5, 70, 27, kText);
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
    Show(root, false);
}
bool Accepts(const amoled_ui::Screen& screen) {
    return screen.kind == amoled_ui::View::kFactoryHome || screen.kind == amoled_ui::View::kAppGrid ||
           screen.kind == amoled_ui::View::kDownloadCode || screen.kind == amoled_ui::View::kDownloadDetail ||
           screen.kind == amoled_ui::View::kAppActions ||
           (screen.kind == amoled_ui::View::kInfo && screen.busy);
}
void Update(const amoled_ui::Screen& screen) {
    using amoled_ui::Tr;
    const bool is_home = screen.kind == amoled_ui::View::kFactoryHome;
    const bool is_grid = screen.kind == amoled_ui::View::kAppGrid;
    const bool is_busy = screen.kind == amoled_ui::View::kInfo && screen.busy;
    const bool is_download = screen.kind == amoled_ui::View::kDownloadCode;
    Show(root, true); Show(home, is_home); Show(apps, is_grid);
    Show(busy, is_busy); Show(download, is_download);
    Show(details, !is_home && !is_grid && !is_busy && !is_download);
    char text[96];
    if (is_download) {
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
        Text(detail_name, screen.title); Text(detail_version, screen.detail);
        detail_cover.Update(actions ? screen.cover_slot : -1, screen.cover_revision, 104, screen.title);
        Show(detail_version, actions); Show(detail_hint, !actions);
        lv_obj_set_y(detail_name, actions ? 201 : 184);
        Text(detail_hint, screen.detail);
        Show(remove, true); Show(detail_back, true);
        Text(launch_text, actions ? Tr("启动", "Open") : screen.rows[0]);
        Text(remove_text, actions ? Tr("卸载", "Remove") : Tr("返回", "Back"));
    }
}
void Hide() { Show(root, false); }
}  // namespace
const amoled_ui::CardRenderer& Renderer() {
    static const amoled_ui::CardRenderer renderer{Create, Update, Hide, Accepts, &kPalette};
    return renderer;
}
}  // namespace amoled_factory
