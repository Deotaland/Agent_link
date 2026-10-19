#pragma once
// What the badge shows about its wearer, and how it survives a reboot.
//
// The App owns this data: it arrives over agent_link as writes to the `badge.*` OUT endpoints
// (see work_badge.cc) and is mirrored into NVS, so a badge that is switched off overnight or
// upgraded over the air comes back showing the same person.
//
// Threading: Set() runs on the transport task, the UI task only reads. Every field is a fixed
// buffer written in one memcpy and the UI redraws whole strings, so a racing update can only
// ever show the previous or the new value, never a mix - no lock on the render path.

#include <cstddef>
#include <cstdint>

namespace badge {

// Which line of the badge a value belongs to. Keep in sync with kFieldKeys in the .cc.
enum class Field : uint8_t {
    kName = 0,     // "ZHANG WEI"      - the big line
    kTitle,        // "Firmware Engineer"
    kDepartment,   // "R&D"
    kEmployeeId,   // "EMP-00421"
    kCompany,      // "DEOTALAND"      - header strip
    kCount,
};

constexpr size_t kMaxFieldLen = 47;  // bytes, excluding the terminator

class Data {
public:
    static Data& Instance();

    // Load every field from NVS; missing fields keep their built-in placeholder.
    void Load();

    // Replace one field (UTF-8, not required to be NUL-terminated) and persist it.
    // Over-long input is truncated on a UTF-8 boundary rather than cut mid-character.
    void Set(Field f, const char* utf8, size_t len);

    const char* Get(Field f) const;

    // Bumped on every Set(): the UI task polls it and redraws only when it changes.
    uint32_t Revision() const;

private:
    Data() = default;
};

}  // namespace badge
