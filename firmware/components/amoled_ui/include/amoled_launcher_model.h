#pragma once
#include <cstddef>

// The add card is always the final page, including on a device with no apps.
class AmoledLauncherModel {
   public:
    void SetCount(size_t installed) {
        installed_ = installed;
        if (index_ > installed_)
            index_ = installed_;
    }
    bool Move(int direction, bool wrap = false) {
        if (wrap && installed_ && direction) {
            index_ = direction > 0 ? (index_ + 1) % Pages() : (index_ + Pages() - 1) % Pages();
            return true;
        }
        if (direction < 0 && index_) {
            --index_;
            return true;
        }
        if (direction > 0 && index_ < installed_) {
            ++index_;
            return true;
        }
        return false;
    }
    size_t Index() const {
        return index_;
    }
    size_t Pages() const {
        return installed_ + 1;
    }
    bool IsAdd() const {
        return index_ == installed_;
    }

   private:
    size_t index_ = 0, installed_ = 0;
};
