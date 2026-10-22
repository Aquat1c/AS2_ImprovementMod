/**
 * packet_router
 *
 * The single dispatch owner for session packets; session2 registers it once
 * at Session_Init and never hands it off. Routing is by packet type,
 * regime-independent:
 *   - TransitionBarrier packets are offered first (reachable in every regime)
 *   - pregame-machine-owned types -> PregameSync_OnSessionPacket
 *   - frontend lockstep / palette / debug types -> owning module handlers
 *   - engine sinks: InputStream -> OnlineWiring_HandleEngineDataPacket
 *     (self-guarding: pre-live drops with logging; session_id enforced at
 *     the typed engine2 ingest), SyncHash -> RollbackSession (session_id
 *     gated here). The startup barrier rides TransitionBarrier GameplayStart.
 *   - ResyncRequest/Reply -> frontend interrogation handlers
 *   - unknown types: log + count, never terminal (forward compat within a
 *     protocol version)
 *
 * Deferred-flush semantics for packets arriving before any sink is installed
 * stay behind Session_SetPacketCallback in session2.
 */

#pragma once

#include "net/protocol.h"

#include <stddef.h>

namespace Net {

/// The single Session packet sink. Registered by session2 at Session_Init;
/// signature matches Net::PacketCallback.
void PacketRouter_OnPacket(PacketType type, const void* payload, size_t payloadLen);

} // namespace Net
