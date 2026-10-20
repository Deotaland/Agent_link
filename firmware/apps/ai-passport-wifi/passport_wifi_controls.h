#pragma once
#include <cstdint>

namespace passport_wifi {
enum class Key { None, Up, Down, Ok };
enum class Gesture { None, Up, Down, Click, Hold };

// Debounced samples enter here. A hold consumes its release; volume repeats while held.
class Controls {
public:
    Gesture Update(Key key, uint32_t now) {
        if (key != held_) {
            const bool click = held_ == Key::Ok && !long_;
            held_ = key; since_ = repeat_ = now; long_ = false;
            if (click && key == Key::None) return Gesture::Click;
            if (key == Key::Up) return Gesture::Up;
            if (key == Key::Down) return Gesture::Down;
        }
        if (key == Key::Ok && !long_ && uint32_t(now - since_) >= 700) {
            long_ = true; return Gesture::Hold;
        }
        if ((key == Key::Up || key == Key::Down) && uint32_t(now - since_) >= 500 &&
            uint32_t(now - repeat_) >= 180) {
            repeat_ = now; return key == Key::Up ? Gesture::Up : Gesture::Down;
        }
        return Gesture::None;
    }
private:
    Key held_ = Key::None;
    uint32_t since_ = 0, repeat_ = 0;
    bool long_ = false;
};
}  // namespace passport_wifi
