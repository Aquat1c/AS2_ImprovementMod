#include "net/connection_supervisor.h"

#include <windows.h>
#include <cstdio>

#include "net/session_manager.h"
#include "net/network_thread.h"
#include "net/netplay_menu_controller.h"
#include "net/protocol.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

namespace Net {

namespace {

// Default thresholds (ms). Dead is deliberately far above GekkoNet's legacy
// 5s timer — the supervisor, not Gekko, decides when a session is over.
constexpr uint32_t kDefaultDegradedMs    = 1000;
constexpr uint32_t kDefaultInterruptedMs = 3000;
constexpr uint32_t kDefaultDeadMs        = 20000;

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
    LOG_NETPLAY(LOG_INFO, "[ConnSup] Supervision started");
}

void ConnectionSupervisor_OnSessionEnd(const char* reason) {
    if (!s_supervising) return;
    s_supervising = false;
    s_health = ConnectionHealth::Healthy;
    s_deadReported = false;
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

    // The single network-originated teardown in the mod (INV-1a).
    if (s_health == ConnectionHealth::Dead && !s_deadReported) {
        s_deadReported = true;
        char reason[128];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE,
                    "Connection lost (no data from peer for %u seconds)",
                    silenceMs / 1000u);
        LOG_NETPLAY(LOG_ERROR, "[ConnSup] %s", reason);
        Rollback::NetplayLog_Write("CONNSUP", -1, "DEAD: %s", reason);
        Rollback::NetplayLog_Flush();
        NetMenu::HandleDisconnection(reason);
    }
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
