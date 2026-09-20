// App -> device audio downlink: codec negotiation, IMA-ADPCM decode, and 0x20 flow control.
// Internal to the component; the board-facing contract never changes (on_audio_out always
// receives PCM16 16kHz mono, whatever went over the wire).
// The App picks the codec from the version the device reports in 0x01: <=1.7.2 -> raw PCM, >=1.7.3 -> ADPCM.
// agent_link reports 2.x, so the App sends us ADPCM
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace agentlink {
namespace audio {

// Wire format of the bytes arriving on the data channel. Matches the `downlink_codec` byte of
// 0x05 VoiceReply / 0x60 StartTranslation.
enum Codec : uint8_t {
    kCodecPcm16    = 0,   // raw PCM 16kHz/16-bit/mono/LE, 32.0 KB/s
    kCodecImaAdpcm = 1,   // IMA-ADPCM 256B blocks, 8.1 KB/s
};

// Decoded PCM16 handed onward (the core forwards it to the board's on_audio_out).
using PcmSink = void (*)(const uint8_t* pcm16, size_t bytes);
// One control-plane event out (the core frames it and notifies 0xFFC4).
using EventSink = void (*)(uint8_t event_id, const uint8_t* payload, size_t len);

void SetSinks(PcmSink pcm, EventSink evt);

// Remember the codec announced by 0x05 / 0x60. Returns false for an out-of-range value (the
// caller answers 1004). Sticky for the whole connection; a change mid-session is ignored,
bool SetCodec(uint8_t codec);
uint8_t CodecValue();

// 0x05 status=2: a downlink session is starting. Resets the block reassembly and the
// flow-control estimate, and starts the 0x20 ticker.
void Arm();
// 0x05 status=3 / link down / board request: session over. Emits a final RESUME if we had the
// App paused, otherwise the App would stay muted into the next utterance.
void Disarm();
bool IsArmed();

// Bytes off the data channel (transport task; never blocks).
void Feed(const uint8_t* data, size_t len);

// How many ms of PCM the board's play buffer holds. Sets the flow-control watermarks: the App
// is paused before we are further ahead of real time than the board can store. Default 400ms.
void SetBufferMs(uint32_t playable_ms);

// Link up/down: down disarms and resets the codec to raw PCM
void OnLinkState(bool connected);

}  // namespace audio
}  // namespace agentlink
