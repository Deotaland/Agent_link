#pragma once
#include <cstdint>
enum agent_link_phase_t { AGENT_LINK_PHASE_IDLE, AGENT_LINK_PHASE_SETUP, AGENT_LINK_PHASE_CONNECTING, AGENT_LINK_PHASE_PAIRING,
                          AGENT_LINK_PHASE_BLOCKED, AGENT_LINK_PHASE_CONNECTED, AGENT_LINK_PHASE_READY };
struct agent_link_status_t {
    agent_link_phase_t phase;
    int transport;
    char title[32], hint[112], code[8];
    uint32_t expires_s;
    int detail;
};
