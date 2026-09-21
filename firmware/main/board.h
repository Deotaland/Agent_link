#pragma once

#include <cstddef>
#include <cstdint>

#include "agent_link_caps.h"  // agent_cap_t：The Board shares a "vocabulary list" with the platform

void* create_board();

class Board {
public:
    virtual ~Board() = default;

    // Display name / BLE broadcast name prefix
    virtual const char* Name() const = 0;

    // This board's capabilities
    virtual uint32_t Capabilities() const = 0;

    // Hardware model, reported to the App and checked on every OTA image it pushes, so a build
    // for other hardware cannot be flashed here. Boards sharing a PCB must return the same
    // string - rorolee-s3 and work-badge are the same board, both "RRL-01". Default: Name().
    virtual const char* Model() const { return Name(); }

    // Agent → Device: agent_link callbacks
    virtual void PlayAudio(const uint8_t* pcm16, size_t bytes) { (void)pcm16; (void)bytes; }
    virtual void AudioEnd() {}
    virtual void ShowText(const char* utf8) { (void)utf8; }
    virtual void Vibrate(uint32_t duration_ms) { (void)duration_ms; }
    virtual void SetLed(uint32_t rgb) { (void)rgb; }   // RGB 0x00RRGGBB; the SDK's led0 endpoint routes here

    // The Agent asks the board to start or stop listening (commands 0x3C/0x3D). Override on a board
    // with a mic to drive its agent_link_asr_start() / asr_push() / asr_end() loop; max_ms is a
    // suggested cap, 0 meaning "until stopped". Only reached when AGENT_CAP_MIC is advertised.
    virtual void OnListen(bool start, uint32_t max_ms) { (void)start; (void)max_ms; }

    // The App link came up or went down. Override to reflect it locally (status icon, idle
    // screen, powering something down while disconnected). Default: nothing.
    virtual void OnLinkState(bool connected) { (void)connected; }

    // Device → Agent: Status Query & Reporting
    virtual int GetBatteryLevel() { return -1; }
    //
    virtual bool IsCharging() { return false; }

    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }
};

// Each board implementation must register itself with the framework by placing the following macro at the end of its .cc file:
#define DECLARE_BOARD(BOARD_CLASS) \
    void* create_board() { return new BOARD_CLASS(); }
