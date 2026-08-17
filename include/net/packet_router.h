/**
 * Alice Senki 2 - packet_router (re0.7 M3, master plan §2.3)
 *
 * The single dispatch owner for session packets. Promoted from the M0
 * gameplay_packet_router extraction: the routing table now merges the
 * pregame regime (previously pregame_sync's private Session callback) and
 * the gameplay regime (previously online_wiring's registration) into one
 * owner that session2 registers once at Session_Init and never hands off.
 * This kills the callback-handoff race class outright — e.g. palette packets
 * 50-52 are routed identically in every regime by construction.
 *
 * Routing is by packet type, regime-independent:
 *   - TransitionBarrier packets are offered first (reachable in every regime)
 *   - pregame-machine-owned types -> PregameSync_OnSessionPacket
 *   - frontend lockstep / palette / debug types -> owning module handlers
 *   - engine sinks (InputStream, GekkoReady) -> OnlineWiring_Handle* (the
 *     handlers self-guard: pre-live InputStream drops with logging, startup
 *     barrier ignores pre-boundary READYs) until the engine2 cutover
 *   - unknown types: log + count, never terminal (forward compat within a
 *     protocol version)
 *
 * Deferred-flush semantics for packets arriving before any sink is installed
 * stay behind Session_SetPacketCallback in session2 (same API home as
 * before); with the router registered at init that window no longer occurs
 * in practice.
 */

#pragma once

#include "net/protocol.h"

#include <stddef.h>

namespace Net {

/// The single Session packet sink. Registered by session2 at Session_Init;
/// signature matches Net::PacketCallback.
void PacketRouter_OnPacket(PacketType type, const void* payload, size_t payloadLen);

} // namespace Net
