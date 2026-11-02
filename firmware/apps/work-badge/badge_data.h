#pragma once
// What the badge shows about its wearer, and how it survives a reboot.
//
// The App owns this data: it arrives over agent_link as writes to the `badge.*` OUT endpoints
// (see work_badge.cc) and is mirrored into NVS, so a badge that is switched off overnight or
// upgraded over the air comes back showing the same person.
//
// Set() updates memory and notifies the storage worker. Copy() takes a coherent
// snapshot for the UI; no filesystem/flash operations run on the transport task.

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
constexpr size_t kFieldCount = static_cast<size_t>(Field::kCount);
struct Snapshot { char values[kFieldCount][kMaxFieldLen + 1]; };

class Data {
public:
    static Data& Instance();

    // Load every field from NVS; missing fields keep their built-in placeholder.
    void Load();

    // Replace one field and queue persistence (it completes asynchronously).
    // Over-long input is truncated on a UTF-8 boundary rather than cut mid-character.
    void Set(Field f, const char* utf8, size_t len);

    void Copy(Snapshot& out) const;

    // Bumped on every Set(): the UI task polls it and redraws only when it changes.
    uint32_t Revision() const;

private:
    Data() = default;
};

}  // namespace badge
