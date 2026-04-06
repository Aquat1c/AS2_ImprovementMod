/**
 * Alice Senki 2 - State Digest for Desync Detection
 *
 * Captures a lightweight snapshot of gameplay state and compares it
 * with the remote peer's digest to detect simulation desync early.
 */

#pragma once

#include "packet_codec.h"
#include <stdint.h>

namespace StateDigest {

// Capture the current game state into a StateDigestPayload.
// Interactive gameplay is preferred; outside that window it falls back to a
// lighter checksum instead of a full rollback savestate CRC.
void Capture(PacketCodec::StateDigestPayload* out);

// Record a locally-sent digest so the matching remote frame can be compared
// later even if the packet arrives several frames afterward.
void RecordLocal(const PacketCodec::StateDigestPayload* digest);

// Compare local and remote digests.  Returns true if they match.
// Logs detailed mismatch info on first divergence.
bool Compare(const PacketCodec::StateDigestPayload* local,
             const PacketCodec::StateDigestPayload* remote);

// Compare an incoming remote digest against the matching locally-recorded frame.
// Returns true on match or when no local frame is available yet.
bool CompareRemote(const PacketCodec::StateDigestPayload* remote);

// Consecutive mismatch counter (resets on match or session reset).
uint32_t GetDesyncCount();
void     ResetDesyncCount();

} // namespace StateDigest
