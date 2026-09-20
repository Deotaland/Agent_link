// agent_link OTA engine — internal interface (not part of the public SDK surface).
//
// Split of responsibilities:
//   agent_link.cpp    decodes 0x37 / 0x56, calls Start()/Abort(), and installs the event sink
//                     used to notify 0x38 progress.
//   transport_ble.cpp pushes the firmware bytes that arrive on the data channel into
//                     FeedData(), asks WantsMore() before replenishing peer credit, and
//                     installs the hook the worker uses to lift that back-pressure.
//   ota_service.cpp   owns the flash side: erase, write, SHA-256, boot switch, reboot.
//
// Public board-facing API lives in include/agent_link_ota.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent_link_ota.h"

namespace agentlink {
namespace ota {

// Protocol error codes
constexpr uint16_t kErrSuccess           = 0;
constexpr uint16_t kErrBusinessFailed    = 1003;
constexpr uint16_t kErrInvalidPayload    = 1004;
constexpr uint16_t kErrConfigFailed      = 1005;
constexpr uint16_t kErrVersionIncompat   = 2030;
constexpr uint16_t kErrFirmwareTooLarge  = 2031;
constexpr uint16_t kErrPartitionFail     = 2032;
constexpr uint16_t kErrWriteFail         = 2033;
constexpr uint16_t kErrSha256Mismatch    = 2034;
constexpr uint16_t kErrAborted           = 2035;
constexpr uint16_t kErrImageInvalid      = 2036;
constexpr uint16_t kErrModelMismatch     = 2037;

constexpr uint8_t  kEvtOtaProgress       = 0x38;

// Sends one control-plane event (installed by the core, which owns the transport).
using EventSink = void (*)(uint8_t event_id, const uint8_t* payload, size_t len);
// Lifts RX back-pressure on the data channel (installed by the transport).
using ResumeRxHook = void (*)(void);

void SetEventSink(EventSink sink);
void SetResumeRxHook(ResumeRxHook hook);

// Device model used for the 0x37 model check and reported in 0x01 / DIS 0x2A24.
void        SetDeviceModel(const char* model);
const char* DeviceModel();

// Validate and open a session: partition + buffer + worker task. The heavy work (erasing the
// target partition takes 2-4s) happens in the worker, so the 0x37 ACK goes out immediately.
// @param model     model string sent by the App, or nullptr when it sent none.
bool Start(uint32_t fw_size, const uint8_t sha256[32], const char* version,
           const char* model, uint16_t* err_code);

// Feed firmware bytes (called from the transport RX context — never blocks).
// @return bytes accepted; 0 when no session is receiving.
size_t FeedData(const uint8_t* data, size_t len);

// RX gate: false = hold off replenishing peer credit, the buffer is nearly full.
// Always true when no upgrade is receiving, so non-OTA traffic is untouched.
bool WantsMore();

// Cooperative abort (App's 0x56, link loss, board request). No-op when idle.
void Abort(const char* reason);

bool IsBusy();
void GetStatus(agent_ota_status_t* out);

// The link came up / went down. Link loss aborts a running upgrade; a link that stays up
// long enough is what confirms a freshly booted image (rollback protection).
void OnLinkState(bool connected);

}  // namespace ota
}  // namespace agentlink
