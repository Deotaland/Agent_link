#pragma once
// Recordings: the on-card recording store, and the App commands that read it.
//
// agent_link does NOT implement 0x04 ListRecordings — see IsProductionOnlyCommand() in
// agent_link.cpp: those command IDs belong to the RoRoLee production firmware, and the SDK answers
// 1001 UnknownCommand rather than fake a success the App would act on. A board may claim them
// though: on_command is consulted first, and returning true from it wins. That is what this does.
//
// Response layout, one page per command:
//   [entry_count(2 LE)] [entries...] [has_more(1)] [total_count(2 LE)] [next_offset(2 LE)]
// and one entry:
//   [name_len(1)] [name(UTF-8, no NUL)] [size_bytes(4 LE)] [mtime_sec(4 LE)] [duration_ms(4 LE)]
// The request is 0, 3 or 4 bytes: [offset(2 LE)] [max_entries(1), 0 = let the device decide]
// [scope(1), 0 = recordings, 1 = messages].
//
// A page is budgeted against the response buffer the SDK provides (AGENT_LINK_CMD_RESP_MAX),
// not against the MTU, so pages are small; the App just follows next_offset until has_more is 0.

#include <cstddef>
#include <cstdint>

#include "wav_recorder.h"

class Recordings {
public:
    struct Config {
        const char*         dir;            // e.g. "/sdcard/records"
        const char*         messages_dir;   // e.g. "/sdcard/records/messages" (scope=1)
        uint32_t            sample_rate;    // to turn a file size into a duration
        WavRecorder::Format format;
    };

    void Init(const Config& cfg) { cfg_ = cfg; }

    // 0x04 ListRecordings. Returns false when the command is malformed or the store is unreadable,
    // with *error set to the code the App expects (1004 InvalidPayload / 1003 BusinessFailed);
    // the caller turns that into a failure ACK.
    bool HandleList(const uint8_t* payload, size_t len,
                    uint8_t* resp, size_t resp_cap, size_t* resp_len, uint16_t* error);

private:
    Config cfg_ = {};

    // Scratch for one entry, held here rather than on the stack. HandleList runs on the transport's
    // own task (see agent_output_cb_t::on_command), whose stack is small and already deep by the
    // time a command reaches us — 650 bytes of locals plus the VFS/FATFS/SDMMC call chain is what
    // overflowed it. Safe as members: the SDK serialises commands onto that one task.
    char rel_[sizeof("messages/") + 256] = {};
    char abs_[384] = {};
};
