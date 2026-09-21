// Recordings: 0x04 ListRecordings over the on-card store. See recordings.h.

#include "recordings.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <strings.h>   // strcasecmp

#include "esp_log.h"

namespace {
constexpr const char* TAG = "recordings";

// Tail of the response after the entries: has_more(1) + total_count(2) + next_offset(2).
constexpr size_t kTailBytes  = 5;
constexpr size_t kCountBytes = 2;   // entry_count, at the front

bool EndsWithWav(const char* name) {
    const size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".wav") == 0;
}

void Put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);       p[1] = static_cast<uint8_t>(v >> 8);
}
void Put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);       p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}
}  // namespace

bool Recordings::HandleList(const uint8_t* payload, size_t len,
                            uint8_t* resp, size_t resp_cap, size_t* resp_len, uint16_t* error) {
    *resp_len = 0;

    // Payload is 0 / 3 / 4 bytes; anything else is malformed. 0 and 3 are the older App's shapes.
    uint16_t offset = 0;
    uint8_t  max_entries = 0;
    uint8_t  scope = 0;
    if (len == 0) {
        // defaults
    } else if (len == 3 || len == 4) {
        offset      = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
        max_entries = payload[2];
        if (len == 4) scope = payload[3];
    } else {
        *error = 1004;   // InvalidPayload
        return false;
    }

    const char* dir = (scope == 1) ? cfg_.messages_dir : cfg_.dir;
    if (!dir) { *error = 1003; return false; }

    DIR* d = opendir(dir);
    if (!d) {
        // No card, or the directory has never been created. Either way the store is unreadable.
        ESP_LOGW(TAG, "cannot open %s", dir);
        *error = 1003;   // BusinessFailed
        return false;
    }

    // One pass: count everything (for total_count) and serialise the window starting at `offset`
    // for as long as the response buffer has room. scope=0 lists the flat top level — the .wav
    // filter skips the messages/ subdirectory on its own; scope=1 lists only messages/.
    const size_t budget = (resp_cap > kCountBytes + kTailBytes) ? resp_cap - kCountBytes - kTailBytes : 0;
    size_t   used        = 0;
    uint16_t entry_count = 0;   // entries actually serialised into this page
    uint16_t consumed    = 0;   // window positions passed, INCLUDING any skipped -- next_offset
                                //   must advance past those or the App re-requests them forever
    uint32_t total       = 0;
    uint16_t index       = 0;
    bool     has_more    = false;
    uint8_t* out         = resp + kCountBytes;

    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (!EndsWithWav(e->d_name)) continue;
        ++total;

        const uint16_t this_index = index++;
        if (this_index < offset) continue;                     // before the requested window
        if (max_entries > 0 && entry_count >= max_entries) { has_more = true; continue; }

        // scope=1 entries are reported with their messages/ prefix, so the name can go straight
        // back as the rel_path of a download or delete without the App re-assembling it.
        // Sized for the worst case d_name (255 bytes) plus that prefix, which is also what keeps
        // -Wformat-truncation quiet.
        if (scope == 1) snprintf(rel_, sizeof(rel_), "messages/%s", e->d_name);
        else            snprintf(rel_, sizeof(rel_), "%s", e->d_name);

        // name_len on the wire is a single byte. A name that will not fit is skipped, not
        // truncated: the App sends this string straight back as the rel_path of a download or a
        // delete, so a shortened one is worse than an absent one — it would just miss.
        const size_t name_len = strlen(rel_);
        if (name_len > 255) {
            ESP_LOGW(TAG, "skipping a %u-byte name: too long for the wire format",
                     static_cast<unsigned>(name_len));
            ++consumed;
            continue;
        }

        if (snprintf(abs_, sizeof(abs_), "%s/%s", dir, e->d_name) >= static_cast<int>(sizeof(abs_))) {
            ++consumed;
            continue;                                          // path too long to even stat
        }
        struct stat st = {};
        if (stat(abs_, &st) != 0) { ++consumed; continue; }     // vanished between readdir and stat

        const size_t need = 1 + name_len + 4 + 4 + 4;
        if (used + need > budget) { has_more = true; continue; }   // keep counting for total_count

        out[used++] = static_cast<uint8_t>(name_len);
        memcpy(out + used, rel_, name_len);                      used += name_len;
        Put32(out + used, static_cast<uint32_t>(st.st_size));   used += 4;
        // No RTC and no 0x0B SetDeviceTime here (the SDK answers that 1001), so this is whatever
        // FATFS stamped — usually a fixed epoch. Reported honestly rather than faked.
        Put32(out + used, static_cast<uint32_t>(st.st_mtime));  used += 4;
        Put32(out + used, WavRecorder::DurationMsFromSize(
                              static_cast<uint32_t>(st.st_size), cfg_.sample_rate, cfg_.format));
        used += 4;
        ++entry_count;
        ++consumed;
    }
    closedir(d);

    Put16(resp, entry_count);
    uint8_t* tail = out + used;
    tail[0] = has_more ? 1 : 0;
    Put16(tail + 1, static_cast<uint16_t>(total > 0xFFFF ? 0xFFFF : total));
    // next_offset must not stall: when the buffer could not hold even one entry we return the
    // offset unchanged and entry_count 0, which the documented App contract reads as "give up,
    // renegotiate a bigger MTU" rather than loop.
    Put16(tail + 3, has_more ? static_cast<uint16_t>(offset + consumed) : 0);

    *resp_len = kCountBytes + used + kTailBytes;
    ESP_LOGI(TAG, "list scope=%u offset=%u -> %u entries, total=%lu, more=%d (%uB)",
             scope, offset, entry_count, static_cast<unsigned long>(total), has_more,
             static_cast<unsigned>(*resp_len));
    return true;
}
