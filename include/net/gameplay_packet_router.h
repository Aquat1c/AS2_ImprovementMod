/**
 * Alice Senki 2 - Gameplay Packet Router (re0.7 M0 extraction)
 *
 * Regime-independent dispatch of session packets during the gameplay-owned
 * callback window. Extracted from online_wiring's OnGameplayPacket so packet
 * routing has a single home; this file is the seed of the re0.7
 * `net/packet_router` single-owner dispatcher (master plan §2.3).
 *
 * Engine-touching packet types (engine input data, startup barrier) are
 * delegated back to online_wiring; everything else routes straight to the
 * owning KEEP-side module.
 */

#pragma once

#include "net/protocol.h"

#include <stddef.h>

namespace Net {

/// Packet callback registered via Session_SetPacketCallback while the
/// gameplay regime owns dispatch. Signature matches Net::PacketCallback.
void GameplayPacketRouter_OnPacket(PacketType type, const void* payload, size_t payloadLen);

} // namespace Net
