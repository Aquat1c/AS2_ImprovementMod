/**
 * Alice Senki 2 - Session Manager
 *
 * Owns the session state machine and handshake logic on top of the
 * ENet transport layer. Manages a single peer connection.
 *
 * State machine:
 *   Idle -> Connecting -> Handshaking -> Connected -> Ready
 *                                                  -> Disconnecting -> Idle
 *                                    -> Failed
 *
 * The session manager does NOT own gameplay or rollback state.
 * Higher layers query session state and send/receive through it.
 */

#pragma once

#include "net/session_types.h"
#include "net/protocol.h"

namespace Net {

// ============================================================================
// Session Snapshot (read-only view for UI / other modules)
// ============================================================================

struct SessionSnapshot {
    bool            active;
    SessionState    state;
    SessionRole     role;
    uint16_t        local_listen_port;
    char            local_nickname[64];
    PeerInfo        remote_peer;
    ConnectionStats stats;
    bool            local_ready;
    bool            remote_ready;
    char            status_text[128];
    char            error_text[128];
};

// M1 facade freeze (docs/re0.7/API_FREEZE.md): session2 must reproduce these
// shapes byte-for-byte. If one of these fires, a preserved contract changed —
// fix the change, not the assert. Values are MSVC Win32 default packing.
static_assert(sizeof(PeerInfo) == 96,
    "PeerInfo shape is frozen for the re0.7 rebuild (inventory §2.1)");
static_assert(sizeof(ConnectionStats) == 40,
    "ConnectionStats shape is frozen for the re0.7 rebuild (inventory §2.1)");
static_assert(sizeof(SessionSnapshot) == 472,
    "SessionSnapshot shape is frozen for the re0.7 rebuild (inventory §2.1)");

// ============================================================================
// Lifecycle
// ============================================================================

/// Initialize the session manager. Call once at mod startup.
void Session_Init();

/// Shut down the session manager. Call once at mod shutdown.
void Session_Shutdown();

// ============================================================================
// Session Control
// ============================================================================

/// Start hosting on the configured port. Waits for an incoming connection.
bool Session_StartHost(const SessionConfig* config);

/// Start joining a remote host at the configured address.
bool Session_StartJoin(const SessionConfig* config);

/// Cancel the current session (from any state). Returns to Idle.
void Session_Cancel();

// Best-effort goodbye for the WM_CLOSE fast-exit path: sends a Disconnect
// packet to the peer and waits briefly (bounded) for delivery.
void Session_NotifyGameExit();

// Milliseconds since ANY packet arrived from the peer (any channel/type).
// Returns 0xFFFFFFFF if nothing was ever received. Used to keep handshake
// timeouts from killing a peer that is provably alive.
uint32_t Session_GetMsSinceLastInbound();

/// Signal that this peer is ready (transitions Connected -> Ready when both ready).
void Session_SignalReady();

// ============================================================================
// Per-Frame Update
// ============================================================================

/// Poll ENet, process events, advance state machine. Call once per game frame.
/// ENet servicing runs on a dedicated network thread; this drains queued
/// transport events and runs game-thread packet callbacks safely.
void Session_Update();

// ============================================================================
// Sending (for higher layers)
// ============================================================================

/// Send a typed packet to the connected peer.
/// Returns false if no peer is connected or send fails.
bool Session_SendPacket(uint8_t channel, PacketType type,
                        const void* payload, size_t payloadLen, bool reliable);

// ============================================================================
// Receive Callback
// ============================================================================

/// Callback type for packets the session manager doesn't handle internally.
/// Called during Session_Update() for gameplay/debug packets.
typedef void (*PacketCallback)(PacketType type, const void* payload, size_t payloadLen);

/// Register a callback for packets not handled by the session layer.
/// Only one callback can be active at a time.
/// Reliable control packets received before a callback is installed are
/// deferred and flushed in order when a callback becomes available.
void Session_SetPacketCallback(PacketCallback cb);

// ============================================================================
// Queries
// ============================================================================

/// Get a read-only snapshot of the current session state.
void Session_GetSnapshot(SessionSnapshot* out);

/// Get current session state.
SessionState Session_GetState();

/// Get current session role.
SessionRole Session_GetRole();

/// Is the session in a connected state (Handshaking, Connected, or Ready)?
bool Session_IsConnected();

/// Get remote peer info (valid after handshake).
const PeerInfo* Session_GetRemotePeer();

/// Get connection statistics.
void Session_GetStats(ConnectionStats* out);

} // namespace Net
