#include "net/connection_supervisor.h"

#include <windows.h>
#include <cstdio>

#include "net/session_manager.h"
#include "net/session2.h"
#include "net/churn_pause.h"
#include "net/netplay_menu_controller.h"
#include "net/protocol.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"
#include "ui/log_window.h"

namespace Net {

namespace {

// Default thresholds (ms), re-based on protocol-level silence (INV-14):
// Healthy <1 s, Degraded 1-5 s, Interrupted 5-20 s, Dead >=20 s. Sane
// because ENet pings every 150 ms, so 1 s of protocol silence is genuinely
// abnormal. Dead is deliberately far above the 0.6 backend's legacy 5 s
// timer — the supervisor, not the rollback backend, decides when a session
// is over.
constexpr uint32_t kDefaultDegradedMs    = 1000;
constexpr uint32_t kDefaultInterruptedMs = 5000;
constexpr uint32_t kDefaultDeadMs        = 20000;

// Progress deadline (§2.4): during active gameplay with a transmitting peer,
// zero canonical-frame progress for 8 s warns (HUD consumes the query), 20 s
// fires a Dead-equivalent teardown with the distinct reason ProgressDeadline.
// This catches a wedged-but-pinging peer; it cannot fire on user-paced
// screens (no rollback session running there) and defers to the silence
// ladder whenever the transport itself is quiet (risk R-9).
constexpr uint32_t kProgressWarnMs = 8000;
constexpr uint32_t kProgressDeadMs = 20000;

// Heartbeat: while we haven't heard from the peer recently, send a reliable
// control-channel ping so the peer's own supervisor sees liveness even when
// no other traffic flows (menus, loading screens, mutual stalls).
constexpr uint32_t kHeartbeatSilenceGateMs = 500;
constexpr uint32_t kHeartbeatIntervalMs    = 250;

bool     s_initialized       = false;
bool     s_supervising       = false;
bool     s_deadReported      = false;
uint32_t s_degradedMs        = kDefaultDegradedMs;
uint32_t s_interruptedMs     = kDefaultInterruptedMs;
uint32_t s_deadMs            = kDefaultDeadMs;
uint32_t s_lastSilenceMs     = 0;
DWORD    s_lastHeartbeatTick = 0;
DWORD    s_supervisionStart  = 0;
uint32_t s_pingId            = 0;
ConnectionHealth s_health    = ConnectionHealth::Healthy;

// Progress-deadline tracking (§2.4).
int32_t  s_progressLastFrame     = -1;
DWORD    s_progressAnchorTick    = 0;
bool     s_progressWarned        = false;
bool     s_progressDeadlineFired = false;
uint32_t s_progressStallMs       = 0;

void SetHealth(ConnectionHealth next, uint32_t silenceMs) {
    if (s_health == next) return;
    LOG_NETPLAY(LOG_INFO,
        "[ConnSup] Health %s -> %s (inbound silence %ums)",
        ConnectionHealthName(s_health),
        ConnectionHealthName(next),
        silenceMs);
    Rollback::NetplayLog_StateChange("CONNSUP", -1,
        "ConnectionHealth",
        ConnectionHealthName(s_health),
        ConnectionHealthName(next),
        "silence");
    s_health = next;
}

void SendHeartbeat() {
    PingPayload payload{};
    payload.ping_id = ++s_pingId;
    payload.send_time_ms = GetTickCount();
    Session_SendPacket(CHANNEL_CONTROL, PacketType::Ping,
                       &payload, sizeof(payload), true);
}

void ResetProgressTracking() {
    s_progressLastFrame  = -1;
    s_progressAnchorTick = 0;
    s_progressWarned     = false;
    s_progressStallMs    = 0;
}

// §2.4 progress deadline. Counts only while: a rollback session is running
// (gameplay), the transport is provably transmitting (silence below the
// Interrupted band — otherwise the silence ladder owns the verdict, R-9),
// and no external suspension (churn pause) legitimately holds the sim.
void UpdateProgressDeadline(uint32_t silenceMs, DWORD now) {
    if (s_progressDeadlineFired) {
        return;
    }

    if (!Rollback::RollbackSession_IsSessionRunning() ||
        silenceMs >= s_interruptedMs) {
        ResetProgressTracking();
        return;
    }

    ChurnPauseSnapshot churn{};
    ChurnPause_GetSnapshot(&churn);
    if (churn.force_hold_active || churn.local_grace_active || churn.remote_grace_active) {
        ResetProgressTracking();   // ExternalSuspension — not a wedge
        return;
    }

    const int32_t frame = Rollback::RollbackSession_GetCurrentFrame();
    if (frame != s_progressLastFrame || s_progressAnchorTick == 0) {
        s_progressLastFrame  = frame;
        s_progressAnchorTick = now;
        s_progressWarned     = false;
        s_progressStallMs    = 0;
        return;
    }

    s_progressStallMs = (now >= s_progressAnchorTick) ? (now - s_progressAnchorTick) : 0;

    if (s_progressStallMs >= kProgressDeadMs) {
        s_progressDeadlineFired = true;
        char reason[128];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                    "Opponent's game stopped responding (%u s without progress)",
                    s_progressStallMs / 1000u);
        LOG_NETPLAY(LOG_ERROR, "[ConnSup] ProgressDeadline: %s (frame=%d)", reason, frame);
        Rollback::NetplayLog_Write("CONNSUP", frame, "PROGRESS-DEAD: %s silence=%ums",
            reason, silenceMs);
        Rollback::NetplayLog_Flush();
        // Typed terminal through the single teardown funnel (reasoned
        // Disconnect reaches the still-pinging peer), then the UI funnel.
        Session2_Terminate(Session2TerminalReason::ProgressDeadline, reason);
        NetMenu::HandleDisconnection(reason);
        return;
    }

    if (!s_progressWarned && s_progressStallMs >= kProgressWarnMs) {
        s_progressWarned = true;
        LOG_NETPLAY(LOG_WARNING,
            "[ConnSup] Progress stall warning: no canonical progress for %ums (frame=%d, silence=%ums)",
            s_progressStallMs, frame, silenceMs);
        Rollback::NetplayLog_Write("CONNSUP", frame,
            "PROGRESS-WARN: no canonical progress for %ums silence=%ums",
            s_progressStallMs, silenceMs);
    }
}

}  // namespace

const char* ConnectionHealthName(ConnectionHealth health) {
    switch (health) {
        case ConnectionHealth::Healthy:     return "Healthy";
        case ConnectionHealth::Degraded:    return "Degraded";
        case ConnectionHealth::Interrupted: return "Interrupted";
        case ConnectionHealth::Dead:        return "Dead";
    }
    return "?";
}

void ConnectionSupervisor_Init() {
    s_initialized = true;
    s_supervising = false;
    s_health = ConnectionHealth::Healthy;
    s_deadReported = false;
    LOG_NETPLAY(LOG_INFO, "[ConnSup] Initialized (degraded=%ums interrupted=%ums dead=%ums)",
        s_degradedMs, s_interruptedMs, s_deadMs);
}

void ConnectionSupervisor_Shutdown() {
    s_initialized = false;
    s_supervising = false;
}

void ConnectionSupervisor_OnSessionStart() {
    if (!s_initialized) return;
    s_supervising = true;
    s_deadReported = false;
    s_health = ConnectionHealth::Healthy;
    s_lastSilenceMs = 0;
    s_lastHeartbeatTick = 0;
    s_supervisionStart = GetTickCount();
    s_progressDeadlineFired = false;
    ResetProgressTracking();
    LOG_NETPLAY(LOG_INFO, "[ConnSup] Supervision started");
}

void ConnectionSupervisor_OnSessionEnd(const char* reason) {
    if (!s_supervising) return;
    s_supervising = false;
    s_health = ConnectionHealth::Healthy;
    s_deadReported = false;
    s_progressDeadlineFired = false;
    ResetProgressTracking();
    LOG_NETPLAY(LOG_INFO, "[ConnSup] Supervision ended: %s", reason ? reason : "?");
}

void ConnectionSupervisor_FrameUpdate() {
    if (!s_initialized || !s_supervising) return;

    SessionSnapshot snap{};
    Session_GetSnapshot(&snap);

    // Only supervise live peer sessions; local teardown paths call
    // OnSessionEnd, but be defensive about missed notifications.
    if (!snap.active ||
        (snap.state != SessionState::Connected &&
         snap.state != SessionState::Ready)) {
        return;
    }

    const uint32_t silenceMs = Session_GetMsSinceLastInbound();
    s_lastSilenceMs = silenceMs;

    // Grace right after supervision starts: last-inbound may predate the
    // session (or be unset). Don't classify until 1s in.
    const DWORD now = GetTickCount();
    if (s_supervisionStart != 0 && (now - s_supervisionStart) < 1000) {
        return;
    }

    if (silenceMs >= s_deadMs) {
        SetHealth(ConnectionHealth::Dead, silenceMs);
    } else if (silenceMs >= s_interruptedMs) {
        SetHealth(ConnectionHealth::Interrupted, silenceMs);
    } else if (silenceMs >= s_degradedMs) {
        SetHealth(ConnectionHealth::Degraded, silenceMs);
    } else {
        SetHealth(ConnectionHealth::Healthy, silenceMs);
    }

    // Keep transmitting during silence so the peer's supervisor sees us.
    if (silenceMs >= kHeartbeatSilenceGateMs) {
        if (s_lastHeartbeatTick == 0 ||
            (now - s_lastHeartbeatTick) >= kHeartbeatIntervalMs) {
            s_lastHeartbeatTick = now;
            SendHeartbeat();
        }
    } else {
        s_lastHeartbeatTick = 0;
    }

    // The single network-originated teardown in the mod (INV-1a / INV-12).
    if (s_health == ConnectionHealth::Dead && !s_deadReported) {
        s_deadReported = true;
        char reason[128];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                    "Connection lost (no data from peer for %u seconds)",
                    silenceMs / 1000u);
        LOG_NETPLAY(LOG_ERROR, "[ConnSup] %s", reason);
        Rollback::NetplayLog_Write("CONNSUP", -1, "DEAD: %s", reason);
        Rollback::NetplayLog_Flush();
        Session2_Terminate(Session2TerminalReason::SupervisorDead, reason);
        NetMenu::HandleDisconnection(reason);
        return;
    }

    // §2.4 progress deadline: the second supervisor input (wedged-but-pinging
    // peer). Distinct typed reason; same once-only teardown discipline.
    UpdateProgressDeadline(silenceMs, now);
}

ConnectionHealth ConnectionSupervisor_GetHealth() {
    return s_supervising ? s_health : ConnectionHealth::Healthy;
}

bool ConnectionSupervisor_IsPeerAlive() {
    const ConnectionHealth h = ConnectionSupervisor_GetHealth();
    return h == ConnectionHealth::Healthy || h == ConnectionHealth::Degraded;
}

bool ConnectionSupervisor_IsInterrupted() {
    return ConnectionSupervisor_GetHealth() == ConnectionHealth::Interrupted;
}

bool ConnectionSupervisor_IsDead() {
    return ConnectionSupervisor_GetHealth() == ConnectionHealth::Dead;
}

uint32_t ConnectionSupervisor_GetInboundSilenceMs() {
    return s_supervising ? s_lastSilenceMs : 0;
}

uint32_t ConnectionSupervisor_GetProgressStallMs() {
    return s_supervising ? s_progressStallMs : 0;
}

bool ConnectionSupervisor_IsProgressStallWarned() {
    return s_supervising && s_progressWarned;
}

void ConnectionSupervisor_SetThresholds(uint32_t degradedMs,
                                        uint32_t interruptedMs,
                                        uint32_t deadMs) {
    if (degradedMs)    s_degradedMs = degradedMs;
    if (interruptedMs) s_interruptedMs = interruptedMs;
    if (deadMs)        s_deadMs = deadMs;
    LOG_NETPLAY(LOG_INFO, "[ConnSup] Thresholds set: degraded=%ums interrupted=%ums dead=%ums",
        s_degradedMs, s_interruptedMs, s_deadMs);
}

}  // namespace Net
