#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include "paper4_input_ui.h"

namespace paper4 {
enum class Action { None, Up, Down, Confirm, Back };

// Only KEY1 has a hold action. A key held across boot must first be released.
class Button {
public:
    Action Poll(bool down, int64_t now, Action tap, bool back_on_hold = false) {
        if (!initialized_) {
            initialized_ = true;
            raw_ = stable_ = down;
            armed_ = !down;
            changed_ = now;
        }
        if (down != raw_) { raw_ = down; changed_ = now; }
        if (stable_ != raw_ && now - changed_ >= 30000) {
            stable_ = raw_;
            if (stable_) { pressed_ = now; held_ = false; }
            else {
                if (!armed_) { armed_ = true; return Action::None; }
                if (!held_) return back_on_hold && now - pressed_ >= 1000000 ? Action::Back : tap;
            }
        }
        if (back_on_hold && armed_ && stable_ && raw_ && !held_ && now - pressed_ >= 1000000) {
            held_ = true;
            return Action::Back;
        }
        return Action::None;
    }
private:
    bool initialized_ = false, raw_ = false, stable_ = false, armed_ = false, held_ = false;
    int64_t changed_ = 0, pressed_ = 0;
};

class Confirm {
public:
    void Request(uint32_t view) { target_ = view; }
    void Cancel() { target_ = 0; }
    bool Pending() const { return target_ != 0; }
    bool Take(uint32_t shown, uint32_t current) {
        if (!target_) return false;
        if (current != target_) { Cancel(); return false; }
        if (shown != target_) return false;
        Cancel();
        return true;
    }
private:
    uint32_t target_ = 0;
};

inline int MonthDays(int year, int month) {
    constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    return days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
inline void ClampDate(std::tm& value) {
    value.tm_mday = std::clamp(value.tm_mday, 1, MonthDays(value.tm_year + 1900, value.tm_mon + 1));
}
inline void MoveMonth(int& year, int& month, int delta) {
    int index = std::clamp((year - 2000) * 12 + month - 1 + delta, 0, 1199);
    year = 2000 + index / 12;
    month = index % 12 + 1;
}
class Alarm {
public:
    bool Due(const std::tm& now, bool enabled, int hour, int minute) {
        const int day = (now.tm_year + 1900) * 10000 + (now.tm_mon + 1) * 100 + now.tm_mday;
        if (!enabled || now.tm_year < 120 || now.tm_hour != hour || now.tm_min != minute || day == day_) return false;
        day_ = day;
        return true;
    }
private:
    int day_ = 0;
};

struct Frame {
    uint32_t view = 1;
    std::string title, status, detail, body, footer;
    std::vector<std::string> rows;
    int selected = -1;
    int calendar_year = 0, calendar_month = 0, today = 0;
    int page = 0;
    uint32_t epoch = 0;
    bool touch = false;
    InputView input;
    bool Same(const Frame& other) const {
        return title == other.title && status == other.status && detail == other.detail &&
               body == other.body && footer == other.footer && rows == other.rows && selected == other.selected &&
               calendar_year == other.calendar_year && calendar_month == other.calendar_month && today == other.today &&
               page == other.page && epoch == other.epoch && touch == other.touch && input.Same(other.input);
    }
};

// E-paper presents an activation code, not a live countdown from each bind-status poll.
inline std::string ActivationDetail(const char* code, const char* hint) {
    return code && *code ? "请在平台输入下方激活码" : hint ? hint : "";
}

class HeaderCache {
public:
    const std::string& Read(const std::string& value, int64_t now, bool hold) {
        if (!valid_ || hold != hold_ || (!hold && now >= next_)) {
            value_ = value;
            next_ = now + 60000000;
            valid_ = true;
        }
        hold_ = hold;
        return value_;
    }
private:
    bool valid_ = false, hold_ = false;
    int64_t next_ = 0;
    std::string value_;
};

// Coalesce key bursts; retain the latest page while a synchronous panel refresh runs.
class Refresh {
public:
    bool Due(int64_t now, uint32_t revision) {
        if (revision != pending_) { pending_ = revision; changed_ = now; }
        return now >= retry_ && now >= next_ && now - changed_ >= 300000;
    }
    void Complete(int64_t now) { next_ = now + 1000000; retry_ = 0; }
    void Failed(int64_t now) { retry_ = now + 60000000; }
private:
    uint32_t pending_ = 0;
    int64_t changed_ = 0, next_ = 0, retry_ = 0;
};
} // namespace paper4
