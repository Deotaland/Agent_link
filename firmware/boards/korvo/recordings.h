#pragma once
// Recordings: the on-card recording store, and the App commands that read it.
//
// agent_link does NOT implement 0x04 ListRecordings — see IsProductionOnlyCommand() in
// agent_link.cpp: those command IDs belong to the RoRoLee production firmware, and the SDK answers
// 1001 UnknownCommand rather than fake a success the App would act on. A board may claim them
// though: on_command is consulted first, and returning true from it wins. That is what this does.
//
// Wire format is docs/ble_sdk.md 4.6. One thing does NOT match that document: it budgets a page at
// ATT_MTU - 20, but the SDK hands on_command a fixed 128-byte response buffer, so the real budget
// is that buffer. Paging is unaffected — the App follows next_offset either way; pages are just
// smaller than the document's arithmetic suggests.

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
};
