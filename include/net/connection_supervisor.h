#pragma once

#include <cstdint>

namespace Net {

// ============================================================================
// ConnectionSupervisor — the single liveness authority for the peer session.
//
// Design invariant (see docs/AS2_0_7_Netplay_Session_Resilience_Rework_Plan.md,
// INV-1/INV-2): only the supervisor may declare a session dead for network
// reasons. Every other layer consumes the published health verdict instead of
// running its own silence timer.
//
//   Healthy      normal operation
//   Degraded     elevated inbound silence (>= degraded threshold); timers that
//                gate handshakes should extend, pacing may adapt
//   Interrupted  no inbound for >= interrupted threshold; gameplay should
//                freeze-and-wait, all phase timers pause, we keep transmitting
//   Dead         no inbound for >= dead threshold; the ONLY network-originated
//                teardown trigger
// ============================================================================

enum class ConnectionHealth : uint8_t {
    Healthy = 0,
    Degraded,
    Interrupted,
    Dead,
};

const char* ConnectionHealthName(ConnectionHealth health);

void ConnectionSupervisor_Init();
void ConnectionSupervisor_Shutdown();

// Call once per frame from the main loop, before the netplay layers update.
void ConnectionSupervisor_FrameUpdate();

// Reset supervision for a fresh session (called when a session reaches
// Connected/Ready) and stop supervising (session ended locally).
void ConnectionSupervisor_OnSessionStart();
void ConnectionSupervisor_OnSessionEnd(const char* reason);

ConnectionHealth ConnectionSupervisor_GetHealth();

// Convenience predicates.
bool ConnectionSupervisor_IsPeerAlive();   // Healthy or Degraded
bool ConnectionSupervisor_IsInterrupted();
bool ConnectionSupervisor_IsDead();

// Milliseconds of inbound silence the current verdict is based on.
// Since re0.7 M3 this is PROTOCOL-level silence (INV-14): ENet acks/pings
// and authenticated autopunch keepalives all count, via transport2.
uint32_t ConnectionSupervisor_GetInboundSilenceMs();

// §2.4 progress deadline (re0.7 M3): while gameplay runs with a transmitting
// peer, milliseconds of zero canonical-frame progress (0 when progressing or
// not applicable). At 8 s the warn flag latches ("opponent's game stopped
// responding" — HUD consumer arrives at M6); at 20 s the supervisor fires a
// Dead-equivalent teardown with the distinct reason ProgressDeadline.
uint32_t ConnectionSupervisor_GetProgressStallMs();
bool ConnectionSupervisor_IsProgressStallWarned();

// Threshold overrides (ms). Pass 0 to keep a value unchanged.
void ConnectionSupervisor_SetThresholds(uint32_t degradedMs,
                                        uint32_t interruptedMs,
                                        uint32_t deadMs);

}  // namespace Net
