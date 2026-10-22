/**
 * session2
 *
 * Re-implements the session layer behind the verbatim-preserved `Session_*`
 * facade (include/net/session_manager.h — that header stays the public
 * contract). This header carries only the session2-specific surface:
 *
 *   - The 5-step nonce handshake replaces the legacy Hello/HelloAck.
 *   - `Session2_Terminate` is the single teardown funnel: exactly one
 *     function executes network teardown; its callers are a fixed
 *     allowlist (supervisor Dead/ProgressDeadline, confirmed desync,
 *     protocol violation, user cancel/quit) plus the local fail-closed
 *     PacingClockDead terminal.
 *   - `session_id` (fnv1a64 of the handshake nonces) qualifies every
 *     gameplay-phase packet.
 */

#pragma once

#include <stdint.h>

namespace Net {

// Typed terminal causes for the single teardown funnel. Every terminal is
// logged with its name and, where a peer is still reachable, sent as a
// reliable Disconnect with a self-describing human string.
enum class Session2TerminalReason : uint8_t {
    UserCancel = 0,      // local user canceled / quit netplay
    GameExit,            // WM_CLOSE fast-exit goodbye path
    SupervisorDead,      // 20 s protocol silence (the supervisor's verdict)
    ProgressDeadline,    // 20 s zero canonical progress while pinging
    PacingClockDead,     // scheduler dead-clock latch (fail closed)
    HandshakeRefused,    // fail-closed refusal at the 5-step handshake
    TransportFailed,     // worker/transport error underneath the session
    ProtocolViolation,   // reserved: engine ingest Conflict/InvalidValue
    ConfirmedDesync,     // reserved: SyncHash mismatch terminal
};

const char* Session2TerminalReasonName(Session2TerminalReason reason);

/// The single teardown funnel. Sends a reliable Disconnect carrying
/// `detail` when a peer is connected, requests transport disconnect/destroy,
/// and lands the state machine in Idle (user-initiated reasons) or Failed
/// (fault reasons, with `detail` as the surfaced error text).
/// Callers are a fixed allowlist — do not add call sites casually; the
/// kill-path CI gate audits the UI-level funnel and this list is reviewed
/// with it.
void Session2_Terminate(Session2TerminalReason reason, const char* detail);

/// Session identity minted by the 5-step handshake:
/// fnv1a64(client_nonce || host_nonce || host_seed). 0 = no established
/// session. Stable for the whole session (epochs rotate inside it).
uint64_t Session2_GetSessionId();

} // namespace Net
