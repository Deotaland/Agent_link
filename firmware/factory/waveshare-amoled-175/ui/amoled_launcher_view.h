#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "lvgl.h"
#include "amoled_fonts.h"
#include "agent_cover.h"
#include "amoled_cover_cache.h"

struct AmoledLauncherCard {
    bool actions = false, add = false;
    size_t index = 0, count = 1;
    std::shared_ptr<const agent_system::CoverPixels> image;
    const char *name = "", *detail = "", *app_id = "", *launch = "", *remove = "";
};

// A full-screen presentation layer. The controller still owns selection/install decisions.
class AmoledLauncherView {
   public:
    void Create(lv_obj_t* parent, AmoledFonts& fonts, void (*bind)(lv_obj_t*, int)) {
        root_ = Box(parent, 0, 0, 466, 466, 0, 0x000000);
        auto* pull = Box(root_, 212, 39, 42, 4, 2, 0x41464F);
        (void)pull;
        card_ = Box(root_, 105, 87, 256, 256, LV_RADIUS_CIRCLE, 0x141C24);
        // Converted covers already carry a circular alpha mask. Children fit
        // within the card, so no extra offscreen rounded-clipping layer is needed.
        lv_obj_set_style_border_width(card_, 1, 0);
        lv_obj_set_style_border_color(card_, lv_color_hex(0x313B45), 0);
        bind(card_, 0);
        picture_ = lv_image_create(card_);
        lv_obj_remove_flag(picture_, LV_OBJ_FLAG_CLICKABLE);
        fallback_ = Label(card_, &lv_font_montserrat_48, 0xE8E0FF, 60, 76, 136, 100);
        plus_h_ = Box(card_, 70, 119, 116, 18, 9, 0xD8F7A4);
        plus_v_ = Box(card_, 119, 70, 18, 116, 9, 0xD8F7A4);
        name_ = Label(root_, fonts.Title(), 0xF5F5F7, 113, 359, 240, 35);
        lv_label_set_long_mode(name_, LV_LABEL_LONG_DOT);
        detail_ = Label(root_, fonts.Small(), 0x909AA7, 125, 315, 216, 20);
        left_ = Label(root_, &lv_font_montserrat_20, 0x5C6573, 47, 204, 24, 32);
        right_ = Label(root_, &lv_font_montserrat_20, 0x5C6573, 394, 204, 24, 32);
        Text(left_, LV_SYMBOL_LEFT);
        Text(right_, LV_SYMBOL_RIGHT);
        for (auto& dot : dots_)
            dot = Box(root_, 0, 408, 6, 6, 3, 0x3B414B);
        close_ = Box(root_, 93, 112, 36, 36, 18, 0x10151C);
        bind(close_, -1);
        auto* close_label = Label(close_, &lv_font_montserrat_20, 0xBCC4CD, 0, 5, 36, 26);
        Text(close_label, LV_SYMBOL_LEFT);
        launch_ = Box(root_, 97, 340, 132, 50, 25, 0xD8F7A4);
        bind(launch_, 0);
        remove_ = Box(root_, 237, 340, 132, 50, 25, 0x2A171C);
        bind(remove_, 1);
        launch_label_ = Label(launch_, fonts.Body(), 0x162218, 0, 12, 132, 28);
        remove_label_ = Label(remove_, fonts.Body(), 0xFF8D99, 0, 12, 132, 28);
        Visible(root_, false);
    }
    void Hide() {
        Visible(root_, false);
    }
    lv_obj_t* CardObject() const {
        return card_;
    }
    lv_obj_t* LaunchObject() const {
        return launch_;
    }
    lv_obj_t* RemoveObject() const {
        return remove_;
    }
    void Update(const AmoledLauncherCard& state) {
        Visible(root_, true);
        const bool actions = state.actions;
        lv_obj_set_pos(card_, actions ? 143 : 105, actions ? 88 : 87);
        lv_obj_set_size(card_, actions ? 180 : 256, actions ? 180 : 256);
        lv_obj_set_style_radius(card_, LV_RADIUS_CIRCLE, 0);
        if (actions)
            lv_obj_remove_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        else
            lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
        Text(name_, state.name);
        lv_obj_set_y(name_, actions ? 279 : 359);
        Text(detail_, state.detail);
        Visible(detail_, actions);
        Visible(close_, actions);
        Visible(launch_, actions);
        Visible(remove_, actions);
        Text(launch_label_, state.launch);
        Text(remove_label_, state.remove);
        const bool image = !state.add && bool(state.image);
        Visible(picture_, image);
        Visible(fallback_, !state.add && !image);
        Visible(plus_h_, state.add);
        Visible(plus_v_, state.add);
        lv_obj_set_style_bg_color(card_, lv_color_hex(state.add ? 0x141F1A : 0x171622), 0);
        if (image && (image_ != state.image || actions != actions_)) {
            lv_image_cache_drop(&image_dsc_);
            lv_image_set_src(picture_, nullptr);
            image_ = state.image;
            image_dsc_ = {};
            image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
            image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565A8;
            image_dsc_.header.w = agent_system::kCoverEdge;
            image_dsc_.header.h = agent_system::kCoverEdge;
            image_dsc_.header.stride = agent_system::kCoverEdge * 2;
            image_dsc_.data_size = agent_system::kCoverPixelBytes;
            image_dsc_.data = image_->Data();
            const auto* cached = cover_cache_.Get(image_, actions ? 180 : 256);
            lv_image_set_src(picture_, cached ? cached : &image_dsc_);
            lv_image_set_scale(
                picture_, cached ? 256
                                 : ((actions ? 180 : 256) * 256 + agent_system::kCoverEdge / 2) /
                                       agent_system::kCoverEdge);
            lv_image_set_antialias(picture_, true);
            lv_obj_center(picture_);
        }
        if (!image && image_) {
            lv_image_cache_drop(&image_dsc_);
            lv_image_set_src(picture_, nullptr);
            image_.reset();
        }
        if (!image && !state.add) {
            Text(fallback_,
                 !strcmp(state.app_id, "agent.voice") ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE);
            lv_obj_center(fallback_);
        }
        Visible(left_, !actions && state.index > 0);
        Visible(right_, !actions && state.index + 1 < state.count);
        const int count = static_cast<int>(state.count > 4 ? 4 : state.count);
        int x = 233 - (count * 6 + (count - 1) * 10 + 12) / 2;
        for (int i = 0; i < 4; ++i) {
            Visible(dots_[i], !actions && i < count);
            const bool selected = static_cast<size_t>(i) == state.index;
            lv_obj_set_pos(dots_[i], x, 408);
            lv_obj_set_width(dots_[i], selected ? 18 : 6);
            lv_obj_set_style_bg_color(dots_[i], lv_color_hex(selected ? 0xD8F7A4 : 0x3B414B), 0);
            x += (selected ? 18 : 6) + 10;
        }
        if (!actions && !actions_ && index_ != static_cast<size_t>(-1) && index_ != state.index) {
            lv_anim_delete(card_, nullptr);
            lv_anim_t animation;
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, card_);
            lv_anim_set_values(&animation, state.index > index_ ? 18 : -18, 0);
            lv_anim_set_duration(&animation, 140);
            lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&animation, [](void* object, int32_t value) {
                lv_obj_set_style_translate_x(static_cast<lv_obj_t*>(object), value, 0);
            });
            lv_anim_start(&animation);
        } else if (actions || actions_) {
            lv_anim_delete(card_, nullptr);
            lv_obj_set_style_translate_x(card_, 0, 0);
        }
        index_ = state.index;
        actions_ = actions;
    }

   private:
    static void Visible(lv_obj_t* object, bool on) {
        if (on)
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    static void Text(lv_obj_t* label, const char* text) {
        if (strcmp(lv_label_get_text(label), text))
            lv_label_set_text(label, text);
    }
    static lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, int radius, uint32_t color) {
        auto* object = lv_obj_create(parent);
        lv_obj_remove_style_all(object);
        lv_obj_set_pos(object, x, y);
        lv_obj_set_size(object, w, h);
        lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(object, radius, 0);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        return object;
    }
    static lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int x, int y,
                           int w, int h) {
        auto* label = lv_label_create(parent);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_size(label, w, h);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        return label;
    }
    lv_obj_t *root_ = nullptr, *card_ = nullptr, *picture_ = nullptr, *fallback_ = nullptr,
             *plus_h_ = nullptr, *plus_v_ = nullptr;
    lv_obj_t *name_ = nullptr, *detail_ = nullptr, *left_ = nullptr, *right_ = nullptr,
             *close_ = nullptr;
    lv_obj_t *launch_ = nullptr, *remove_ = nullptr, *launch_label_ = nullptr,
             *remove_label_ = nullptr, *dots_[4]{};
    lv_image_dsc_t image_dsc_{};
    AmoledCoverCache cover_cache_;
    std::shared_ptr<const agent_system::CoverPixels> image_;
    size_t index_ = static_cast<size_t>(-1);
    bool actions_ = false;
};
