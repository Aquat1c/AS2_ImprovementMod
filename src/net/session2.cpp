/**
 * Alice Senki 2 - session2 (Implementation, re0.7 M3)
 *
 * Re-implementation of the session layer behind the verbatim-preserved
 * `Session_*` facade (include/net/session_manager.h). Descends from
 * session_manager.cpp (deleted at M3) with the §2.4 changes:
 *
 *   - transport2 worker underneath (protocol_silence_ms liveness, INV-14)
 *   - 5-step nonce handshake (§4.2) replacing legacy Hello/HelloAck:
 *     SessionHello → SessionOffer → SessionAck → SessionConfirm →
 *     SessionConfirmAck, echo-verbatim verified (INV-13: confirm packets are
 *     built from the received BYTES, never recomputed), 200 ms per-step
 *     resend, 10 s step / 30 s total timeouts, fail-closed refusal naming
 *     the exact mismatching field (C-3)
 *   - PeerIdentity exchange after Connected fills the preserved PeerInfo
 *     contract (full nickname / round option / frame timing / HUD style)
 *   - Session2_Terminate teardown funnel (INV-12 caller allowlist) including
 *     the PacingClockDead local fail-closed terminal (M2 obligation)
 *   - packet_router is the default packet sink, registered once at init and
 *     never handed off (§2.3)
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>

#include "net/session_manager.h"
#include "net/session2.h"
#include "net/connection_supervisor.h"
#include "net/transition_barrier.h"
#include "net/transport2.h"
#include "net/nat_traversal.h"
#include "net/game_settings_sync.h"
#include "net/packet_router.h"
#include "net/netplay_menu_controller.h"
#include "patches/frame_scheduler.h"
#include "ui/netplay_hud_style.h"
#include "patches/memory_utils.h"
#include "patches/tick_hooks.h"
#include "log_window.h"
#include "rollback/netplay_log.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <random>
#include <vector>

namespace Net {

// ============================================================================
// Internal state
// ============================================================================

static SessionState   s_state = SessionState::Idle;
static SessionRole    s_role  = SessionRole::None;
static SessionConfig  s_config;
static PeerInfo       s_remotePeer;
static ConnectionStats s_stats;
static ConnectionStats s_lastLiveStats;
static bool           s_lastLiveStatsValid = false;
static uintptr_t      s_peerToken = 0;
static char           s_statusText[128] = "";
static char           s_errorText[128]  = "";
static PacketCallback s_packetCallback  = nullptr;
static bool           s_localReady      = false;
static bool           s_remoteReady     = false;
static bool           s_localNatInfoSent = false;
static bool           s_localIdentitySent = false;
static DWORD          s_stateEnteredAt  = 0;  // GetTickCount when state entered
static uint32_t       s_activeSessionToken = 0;
static bool           s_joinFallbackAttempted = false;
static bool           s_joinUsingRelay = false;
static char           s_activeJoinHost[96] = "";
static uint16_t       s_activeJoinPort = 0;

static uint32_t       s_lastInboundDropCount  = 0;
static uint32_t       s_lastOutboundDropCount = 0;
static DWORD          s_lastQueueSpikeLogAt   = 0;
static DWORD          s_lastInboundSilenceLogAt = 0;
static DWORD          s_lastDrainLagLogAt     = 0;
static DWORD          s_lastSessionUpdateTick = 0;
static bool           s_localBuildHashCached  = false;
static uint32_t       s_localBuildHash        = 0;
static bool           s_pacingClockTerminalFired = false;

// --- v2 handshake state (§4.2) ---

enum class HandshakeStep : uint8_t {
    None = 0,
    // Client (joiner)
    AwaitOffer,      // Hello sent, resending until Offer
    AwaitConfirm,    // Ack sent, resending until Confirm
    // Host
    AwaitHello,      // ENet connected, waiting for the client's Hello
    AwaitAck,        // Offer sent, resending until Ack
    AwaitConfirmAck, // Confirm sent, resending until ConfirmAck
    Done,
};

static const char* HandshakeStepName(HandshakeStep step) {
    switch (step) {
        case HandshakeStep::None:            return "None";
        case HandshakeStep::AwaitOffer:      return "AwaitOffer";
        case HandshakeStep::AwaitConfirm:    return "AwaitConfirm";
        case HandshakeStep::AwaitHello:      return "AwaitHello";
        case HandshakeStep::AwaitAck:        return "AwaitAck";
        case HandshakeStep::AwaitConfirmAck: return "AwaitConfirmAck";
        case HandshakeStep::Done:            return "Done";
    }
    return "?";
}

constexpr DWORD kHandshakeResendIntervalMs = 200;    // §4.2 per-step resend
constexpr DWORD kHandshakeStepTimeoutMs    = 10000;  // §4.2 per-step cap
constexpr DWORD kHandshakeTotalTimeoutMs   = 30000;  // §4.2 whole-handshake cap

static HandshakeStep       s_hsStep = HandshakeStep::None;
static DWORD               s_hsStepEnteredAt = 0;
static DWORD               s_hsLastResendAt = 0;
static SessionHelloPayload s_hsHelloSent{};      // client: bytes we sent (echo check)
static SessionOfferPayload s_hsOfferSent{};      // host: bytes we sent (echo check)
static SessionOfferPayload s_hsOfferReceived{};  // client: bytes received (echoed in Ack)
static uint64_t            s_hsClientNonce = 0;
static uint64_t            s_hsHostNonce = 0;
static uint32_t            s_hsHostSeed = 0;
static uint64_t            s_sessionId = 0;

constexpr int    MAX_DEFERRED_CONTROL_PACKETS = 256;      // §2.3 bound
constexpr size_t MAX_DEFERRED_CONTROL_BYTES   = 256 * 1024;

struct BuildFingerprintComponent {
    char     name[16];
    uint32_t crc32;
    uint32_t size_bytes;
    uint32_t present;
};

struct DeferredControlPacket {
    PacketType type;
    uint8_t    channel_id;
    size_t     payload_len;
    uint8_t    payload[MAX_PAYLOAD_SIZE];
};

static DeferredControlPacket s_deferredControlPackets[MAX_DEFERRED_CONTROL_PACKETS];
static int                  s_deferredControlCount = 0;
static size_t               s_deferredControlBytes = 0;

// ============================================================================
// Helpers
// ============================================================================

static void SetState(SessionState newState) {
    if (s_state == newState) return;
    LOG_INFO("[Session] State: %s -> %s", SessionStateName(s_state), SessionStateName(newState));
    Rollback::NetplayLog_StateChange(
        "SESSION", -1,
        "state",
        SessionStateName(s_state),
        SessionStateName(newState),
        "session2 transition"
    );
    s_state = newState;
    s_stateEnteredAt = GetTickCount();

    // The supervisor owns liveness for the life of the peer connection.
    if (newState == SessionState::Connected) {
        ConnectionSupervisor_OnSessionStart();
    } else if (newState == SessionState::Idle ||
               newState == SessionState::Failed) {
        ConnectionSupervisor_OnSessionEnd(SessionStateName(newState));
        TransitionBarrier_Reset(SessionStateName(newState));
    }
}

static void SetError(const char* msg) {
    snprintf(s_errorText, sizeof(s_errorText), "%s", msg);
    LOG_ERROR("[Session] Error: %s", msg);
    Rollback::NetplayLog_Write("SESSION", -1, "ERROR: %s", msg);
    GameSettingsSync_RestoreLocalSession(msg ? msg : "session error");
    TickHooks_ClearFrameLimiter60FpsSessionOverride(msg ? msg : "session error");
    Nat_StopServices();
    SetState(SessionState::Failed);
}

static bool IsCompatibilityDisconnectData(uint32_t data) {
    return data == static_cast<uint32_t>(DisconnectReason::VersionMismatch);
}

static bool IsUserCancelDisconnectData(uint32_t data) {
    return data == static_cast<uint32_t>(DisconnectReason::UserCancel);
}

static bool IsBusyDisconnectData(uint32_t data) {
    return data == static_cast<uint32_t>(DisconnectReason::Busy);
}

static bool HasUsefulStats(const ConnectionStats& stats) {
    return stats.rtt_ms > 0.0f ||
           stats.rtt_variance_ms > 0.0f ||
           stats.packets_sent != 0 ||
           stats.packets_received != 0 ||
           stats.packets_lost != 0 ||
           stats.bytes_sent != 0 ||
           stats.bytes_received != 0;
}

static FrameTimingMode LocalFrameTimingMode() {
    return IsFrameLimiter60FpsPatchEnabled()
        ? FrameTimingMode::Proper60
        : FrameTimingMode::Vanilla58_8;
}

// §2.8.2 cadence profile rationals, handshake-carried and fail-closed
// compared (both peers must match; QOH99 delta #8).
static void LocalCadenceRational(uint16_t* outNum, uint16_t* outDen) {
    if (LocalFrameTimingMode() == FrameTimingMode::Proper60) {
        *outNum = 1;
        *outDen = 60;    // period = QPF * 1 / 60  (60.000 Hz)
    } else {
        *outNum = 17;
        *outDen = 1000;  // period = QPF * 17 / 1000  (58.82 Hz)
    }
}

static void ClearStatsForNewSession() {
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_lastLiveStats, 0, sizeof(s_lastLiveStats));
    s_lastLiveStatsValid = false;
}

static void RememberLiveStats() {
    if (!HasUsefulStats(s_stats)) {
        return;
    }
    s_lastLiveStats = s_stats;
    s_lastLiveStatsValid = true;
}

static ConnectionStats GetBestStatsSnapshot() {
    if (HasUsefulStats(s_stats) || !s_lastLiveStatsValid) {
        return s_stats;
    }
    return s_lastLiveStats;
}

static void ResetHandshakeState() {
    s_hsStep = HandshakeStep::None;
    s_hsStepEnteredAt = 0;
    s_hsLastResendAt = 0;
    memset(&s_hsHelloSent, 0, sizeof(s_hsHelloSent));
    memset(&s_hsOfferSent, 0, sizeof(s_hsOfferSent));
    memset(&s_hsOfferReceived, 0, sizeof(s_hsOfferReceived));
    s_hsClientNonce = 0;
    s_hsHostNonce = 0;
    s_hsHostSeed = 0;
    s_sessionId = 0;
}

static void ResetState() {
    TickHooks_ClearFrameLimiter60FpsSessionOverride("session reset");
    s_state = SessionState::Idle;
    s_role  = SessionRole::None;
    memset(&s_remotePeer, 0, sizeof(s_remotePeer));
    memset(&s_stats, 0, sizeof(s_stats));
    s_peerToken = 0;
    s_statusText[0] = '\0';
    s_errorText[0]  = '\0';
    s_localReady  = false;
    s_remoteReady = false;
    s_localNatInfoSent = false;
    s_localIdentitySent = false;
    s_stateEnteredAt = 0;
    s_joinFallbackAttempted = false;
    s_joinUsingRelay = false;
    s_activeJoinHost[0] = '\0';
    s_activeJoinPort = 0;
    s_deferredControlCount = 0;
    s_deferredControlBytes = 0;
    s_lastInboundDropCount = 0;
    s_lastOutboundDropCount = 0;
    s_lastQueueSpikeLogAt = 0;
    s_lastInboundSilenceLogAt = 0;
    s_lastDrainLagLogAt = 0;
    s_lastSessionUpdateTick = 0;
    s_pacingClockTerminalFired = false;
    ResetHandshakeState();
    Nat_ClearRemoteHint();
}

static bool ReadEntireFile(const char* path, std::vector<uint8_t>* outBytes) {
    if (!path || !path[0] || !outBytes) {
        return false;
    }

    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }

    if (static_cast<unsigned long long>(fileSize.QuadPart) >
        static_cast<unsigned long long>(SIZE_MAX)) {
        CloseHandle(file);
        return false;
    }

    const size_t size = static_cast<size_t>(fileSize.QuadPart);
    outBytes->assign(size, 0);

    size_t totalRead = 0;
    while (totalRead < size) {
        const DWORD toRead = static_cast<DWORD>(
            (size - totalRead) > static_cast<size_t>(0x400000)
                ? 0x400000
                : (size - totalRead));
        DWORD chunkRead = 0;
        if (!ReadFile(file, outBytes->data() + totalRead, toRead, &chunkRead, nullptr) ||
            chunkRead == 0) {
            CloseHandle(file);
            return false;
        }
        totalRead += static_cast<size_t>(chunkRead);
    }

    CloseHandle(file);
    return true;
}

static bool GetSessionModuleDirectory(char* outDir, size_t outCap) {
    if (!outDir || outCap == 0) {
        return false;
    }

    HMODULE selfModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&GetSessionModuleDirectory),
            &selfModule)) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(selfModule, modulePath, MAX_PATH) == 0) {
        return false;
    }

    char* lastSlash = strrchr(modulePath, '\\');
    if (!lastSlash) {
        lastSlash = strrchr(modulePath, '/');
    }
    if (!lastSlash) {
        return false;
    }

    *lastSlash = '\0';
    strncpy_s(outDir, outCap, modulePath, _TRUNCATE);
    return true;
}

static bool BuildFingerprintFromFile(const char* dir,
                                     const char* fileName,
                                     const char* label,
                                     BuildFingerprintComponent* outComponent) {
    if (!dir || !fileName || !label || !outComponent) {
        return false;
    }

    char path[MAX_PATH] = {};
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, fileName);

    std::vector<uint8_t> bytes;
    if (!ReadEntireFile(path, &bytes)) {
        Rollback::NetplayLog_Write(
            "SESSION", -1,
            "ERROR: Exact build lock failed to read %s at %s",
            label,
            path);
        return false;
    }

    if (bytes.size() > static_cast<size_t>(UINT32_MAX)) {
        Rollback::NetplayLog_Write(
            "SESSION", -1,
            "ERROR: Exact build lock file too large for %s at %s (%zu bytes)",
            label,
            path,
            bytes.size());
        return false;
    }

    memset(outComponent, 0, sizeof(*outComponent));
    strncpy_s(outComponent->name, sizeof(outComponent->name), label, _TRUNCATE);
    outComponent->crc32 = bytes.empty() ? 0u : CalcCRC32(bytes.data(), bytes.size());
    outComponent->size_bytes = static_cast<uint32_t>(bytes.size());
    outComponent->present = 1;
    return true;
}

static bool ComputeLocalBuildHash(uint32_t* outHash) {
    if (!outHash) {
        return false;
    }

    if (s_localBuildHashCached) {
        *outHash = s_localBuildHash;
        return true;
    }

    char moduleDir[MAX_PATH] = {};
    if (!GetSessionModuleDirectory(moduleDir, sizeof(moduleDir))) {
        Rollback::NetplayLog_Write(
            "SESSION", -1,
            "ERROR: Exact build lock failed to resolve session module directory");
        return false;
    }

    BuildFingerprintComponent components[3] = {};
    if (!BuildFingerprintFromFile(moduleDir, "as2_rollback.dll", "as2_rollback", &components[0]) ||
        !BuildFingerprintFromFile(moduleDir, "d3d9.dll", "d3d9", &components[1]) ||
        !BuildFingerprintFromFile(moduleDir, "wsock32.dll", "wsock32", &components[2])) {
        return false;
    }

    s_localBuildHash = CalcCRC32(components, sizeof(components));
    s_localBuildHashCached = true;

    Rollback::NetplayLog_Write(
        "SESSION", -1,
        "Local exact build hash=0x%08X components=[%s crc=0x%08X size=%u, %s crc=0x%08X size=%u, %s crc=0x%08X size=%u]",
        s_localBuildHash,
        components[0].name,
        components[0].crc32,
        components[0].size_bytes,
        components[1].name,
        components[1].crc32,
        components[1].size_bytes,
        components[2].name,
        components[2].crc32,
        components[2].size_bytes);

    *outHash = s_localBuildHash;
    return true;
}

static bool DeferControlPacket(uint8_t channelID, PacketType type,
                               const void* payload, size_t payloadLen) {
    if (channelID != CHANNEL_CONTROL) {
        return false;
    }
    if (payloadLen > MAX_PAYLOAD_SIZE) {
        return false;
    }
    if (s_deferredControlCount >= MAX_DEFERRED_CONTROL_PACKETS ||
        s_deferredControlBytes + payloadLen > MAX_DEFERRED_CONTROL_BYTES) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Deferred control packet queue full: type=%s ch=%u payload=%zu queued=%d bytes=%zu state=%s role=%s",
            PacketTypeName(type),
            channelID,
            payloadLen,
            s_deferredControlCount,
            s_deferredControlBytes,
            SessionStateName(s_state),
            SessionRoleName(s_role));
        Rollback::NetplayLog_Flush();
        return false;
    }

    DeferredControlPacket* slot = &s_deferredControlPackets[s_deferredControlCount++];
    slot->type = type;
    slot->channel_id = channelID;
    slot->payload_len = payloadLen;
    if (payloadLen > 0 && payload) {
        memcpy(slot->payload, payload, payloadLen);
    }
    s_deferredControlBytes += payloadLen;

    Rollback::NetplayLog_Write("SESSION", -1,
        "Deferred control packet awaiting callback: type=%s ch=%u payload=%zu queued=%d state=%s role=%s",
        PacketTypeName(type),
        channelID,
        payloadLen,
        s_deferredControlCount,
        SessionStateName(s_state),
        SessionRoleName(s_role));
    Rollback::NetplayLog_Flush();
    return true;
}

static void FlushDeferredControlPackets() {
    if (!s_packetCallback || s_deferredControlCount <= 0) {
        return;
    }

    const int queued = s_deferredControlCount;
    Rollback::NetplayLog_Write("SESSION", -1,
        "Flushing %d deferred control packets to callback 0x%llX",
        queued,
        (unsigned long long)(uintptr_t)s_packetCallback);
    Rollback::NetplayLog_Flush();

    s_deferredControlCount = 0;
    s_deferredControlBytes = 0;
    for (int i = 0; i < queued; i++) {
        const DeferredControlPacket* packet = &s_deferredControlPackets[i];
        s_packetCallback(packet->type, packet->payload, packet->payload_len);
    }
}

static void UpdateStatusText() {
    switch (s_state) {
        case SessionState::Idle:
            snprintf(s_statusText, sizeof(s_statusText), "No session");
            break;
        case SessionState::Connecting:
            snprintf(s_statusText, sizeof(s_statusText), "%s: Connecting...",
                     SessionRoleName(s_role));
            break;
        case SessionState::Handshaking:
            snprintf(s_statusText, sizeof(s_statusText), "%s: Handshaking...",
                     SessionRoleName(s_role));
            break;
        case SessionState::Connected:
            snprintf(s_statusText, sizeof(s_statusText), "Connected to %s (RTT: %.0fms, FPS: %s)",
                     s_remotePeer.nickname,
                     s_stats.rtt_ms,
                     FrameTimingModeDisplayName(LocalFrameTimingMode()));
            break;
        case SessionState::Ready:
            snprintf(s_statusText, sizeof(s_statusText), "Ready (both peers, FPS: %s)",
                     FrameTimingModeDisplayName(LocalFrameTimingMode()));
            break;
        case SessionState::Disconnecting:
            snprintf(s_statusText, sizeof(s_statusText), "Disconnecting...");
            break;
        case SessionState::Failed:
            snprintf(s_statusText, sizeof(s_statusText), "Error: %s", s_errorText);
            break;
    }
}

static void NotePacketSent(uint8_t channel, PacketType type,
                           size_t payloadLen, bool reliable,
                           const char* context) {
    (void)channel;
    (void)type;
    (void)reliable;
    (void)context;
    s_stats.bytes_sent += sizeof(PacketType) + payloadLen;
    RememberLiveStats();
}

static void NotePacketReceived(uint8_t channel, PacketType type, size_t payloadLen) {
    (void)channel;
    (void)type;
    s_stats.packets_received++;
    s_stats.bytes_received += sizeof(PacketType) + payloadLen;
    RememberLiveStats();
}

static uint32_t NextSessionToken() {
    s_activeSessionToken++;
    if (s_activeSessionToken == 0) {
        s_activeSessionToken = 1;
    }
    return s_activeSessionToken;
}

static bool QueueTypedPacket(uint8_t channel, PacketType type,
                             const void* payload, size_t payloadLen,
                             bool reliable, const char* context) {
    if (s_activeSessionToken == 0) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "DROP send (no active session token): ch=%u type=%s payload=%zu reliable=%d state=%s",
            channel,
            PacketTypeName(type),
            payloadLen,
            reliable ? 1 : 0,
            SessionStateName(s_state));
        return false;
    }

    const bool queued = Transport2_SendPacket(
        s_activeSessionToken,
        channel,
        type,
        payload,
        payloadLen,
        reliable);
    if (queued) {
        NotePacketSent(channel, type, payloadLen, reliable, context);
    } else {
        Rollback::NetplayLog_Write("SESSION", -1,
            "ERROR: outbound queue rejected send ch=%u type=%s payload=%zu reliable=%d token=%u",
            channel,
            PacketTypeName(type),
            payloadLen,
            reliable ? 1 : 0,
            s_activeSessionToken);
    }
    return queued;
}

static bool HasRelayConfigured(const SessionConfig* cfg) {
    return cfg &&
           cfg->nat.relay_host[0] != '\0' &&
           cfg->nat.relay_port > 0;
}

static bool HasTrafficRelayConfigured(const SessionConfig*) {
    // The relay endpoint is currently an autopunch rendezvous server. It is not
    // a data-forwarding ENet relay, so session fallback must not connect to it.
    return false;
}

static void GetPunchRelayEndpoint(const SessionConfig* cfg,
                                  char* outHost, size_t outHostCap,
                                  uint16_t* outPort) {
    if (!outHost || outHostCap == 0 || !outPort) {
        return;
    }

    outHost[0] = '\0';
    *outPort = 0;

    if (HasRelayConfigured(cfg)) {
        strncpy_s(outHost, outHostCap, cfg->nat.relay_host, _TRUNCATE);
        *outPort = cfg->nat.relay_port;
        return;
    }

    strncpy_s(outHost, outHostCap, "delthas.fr", _TRUNCATE);
    *outPort = 14763;
}

static bool StartJoinAttempt(const char* host, uint16_t port,
                             bool usingRelay, const char* reason) {
    if (!host || !host[0] || port == 0 || s_activeSessionToken == 0) {
        return false;
    }

    s_joinUsingRelay = usingRelay;
    strncpy_s(s_activeJoinHost, sizeof(s_activeJoinHost), host, _TRUNCATE);
    s_activeJoinPort = port;
    Nat_SetRemoteHint(s_activeJoinHost, s_activeJoinPort);
    const bool wantsHolePunch = (!usingRelay && s_config.nat.enable_hole_punch);
    const bool canHolePunch = Nat_IsHolePunchBackendAvailable();
    const bool useHolePunch = wantsHolePunch && canHolePunch;
    char punchRelayHost[96] = {};
    uint16_t punchRelayPort = 0;
    GetPunchRelayEndpoint(&s_config, punchRelayHost, sizeof(punchRelayHost), &punchRelayPort);

    Rollback::NetplayLog_Write("SESSION", -1,
        "Join attempt begin: token=%u host=%s port=%u via=%s reason=%s hole_punch_req=%d hole_punch_use=%d punch_relay=%s:%u",
        s_activeSessionToken,
        s_activeJoinHost,
        s_activeJoinPort,
        usingRelay ? "relay" : "direct",
        reason ? reason : "?",
        wantsHolePunch ? 1 : 0,
        useHolePunch ? 1 : 0,
        punchRelayHost,
        (unsigned)punchRelayPort);

    if (wantsHolePunch && !canHolePunch) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Hole-punch requested but no backend is available; continuing with direct connect");
    }

    return Transport2_StartJoin(s_activeSessionToken,
                                s_config.listen_port,
                                s_activeJoinHost,
                                s_activeJoinPort,
                                useHolePunch,
                                punchRelayHost,
                                punchRelayPort);
}

static bool TryRelayFallback(const char* reason) {
    if (s_role != SessionRole::Join) {
        return false;
    }
    if (s_joinUsingRelay) {
        return false;
    }
    if (s_joinFallbackAttempted) {
        return false;
    }
    if (s_config.connect_preference != ConnectPreference::AutoDirectThenRelay) {
        return false;
    }
    if (!HasTrafficRelayConfigured(&s_config)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Traffic relay fallback unavailable; relay endpoint is reserved for autopunch rendezvous");
        return false;
    }

    s_joinFallbackAttempted = true;

    Rollback::NetplayLog_Write("SESSION", -1,
        "Direct join failed -> relay fallback: relay=%s:%u reason=%s",
        s_config.nat.relay_host,
        s_config.nat.relay_port,
        reason ? reason : "?");

    Transport2_RequestDestroyHost(s_activeSessionToken);
    Transport2_ClearQueues(s_activeSessionToken);

    if (!StartJoinAttempt(s_config.nat.relay_host, s_config.nat.relay_port, true, reason)) {
        return false;
    }

    s_stateEnteredAt = GetTickCount();
    return true;
}

static void SendNatInfo() {
    if (s_localNatInfoSent || s_state != SessionState::Connected) {
        return;
    }

    NatSnapshot nat{};
    Nat_GetSnapshot(&nat);

    NatInfoPayload payload{};
    payload.upnp_status = (uint8_t)nat.upnp_status;
    payload.stun_status = (uint8_t)nat.stun_status;
    payload.listen_port = s_config.listen_port;
    payload.external_port = nat.stun_external_port;
    strncpy_s(payload.external_ip, sizeof(payload.external_ip),
              nat.external_ip[0] ? nat.external_ip : nat.stun_endpoint,
              _TRUNCATE);

    if (nat.upnp_enabled) payload.flags |= NAT_INFO_FLAG_UPNP_ENABLED;
    if (nat.upnp_status == NatStatus::Mapped) payload.flags |= NAT_INFO_FLAG_UPNP_MAPPED;
    if (nat.stun_enabled) payload.flags |= NAT_INFO_FLAG_STUN_ENABLED;
    if (nat.stun_status == StunStatus::Available) payload.flags |= NAT_INFO_FLAG_STUN_OK;
    if (nat.hole_punch_enabled) payload.flags |= NAT_INFO_FLAG_HOLE_PUNCH;
    if (HasTrafficRelayConfigured(&s_config)) payload.flags |= NAT_INFO_FLAG_RELAY_FALLBACK;
    if (nat.prefer_portforwarded_direct) payload.flags |= NAT_INFO_FLAG_PREFER_DIRECT;
    if (nat.allow_ipv6_endpoint) payload.flags |= NAT_INFO_FLAG_IPV6_ENDPOINTS;
    if (nat.turn_enabled) payload.extra_flags |= NAT_INFO_EX_FLAG_TURN_ENABLED;
    if (nat.pcp_enabled) payload.extra_flags |= NAT_INFO_EX_FLAG_PCP_ENABLED;

    if (QueueTypedPacket(CHANNEL_CONTROL, PacketType::NatInfo,
                         &payload, sizeof(payload), true, "nat-info")) {
        s_localNatInfoSent = true;
        Rollback::NetplayLog_Write("SESSION", -1,
            "Local NatInfo sent: flags=0x%02X extra=0x%02X upnp=%s pcp=%s stun=%s ext_ip=%s stun_ep=%s join_via=%s",
            payload.flags,
            payload.extra_flags,
            NatStatusName(nat.upnp_status),
            NatStatusName(nat.pcp_status),
            StunStatusName(nat.stun_status),
            nat.external_ip,
            nat.stun_endpoint,
            s_joinUsingRelay ? "relay" : "direct");
    }
}

static void FlushNatTraversalOutboundSignals() {
    if (s_state != SessionState::Connected &&
        s_state != SessionState::Ready &&
        s_state != SessionState::Handshaking) {
        return;
    }

    NatSignalMessage msg{};
    while (Nat_TryPopOutboundSignal(&msg)) {
        NatTraversalSignalPayload payload{};
        payload.signal_type = (uint8_t)msg.type;
        size_t textLen = 0;
        while (textLen < sizeof(payload.text) - 1 && msg.text[textLen] != '\0') {
            textLen++;
        }
        payload.text_len = (uint16_t)textLen;
        if (payload.text_len > 0) {
            memcpy(payload.text, msg.text, payload.text_len);
        }
        payload.text[payload.text_len] = '\0';

        const size_t wireLen =
            offsetof(NatTraversalSignalPayload, text) + payload.text_len + 1;
        if (!QueueTypedPacket(CHANNEL_CONTROL,
                              PacketType::NatTraversalSignal,
                              &payload,
                              wireLen,
                              true,
                              "nat-traversal-signal")) {
            Rollback::NetplayLog_Write("SESSION", -1,
                "Failed to send NAT traversal signal: type=%s len=%u",
                NatSignalTypeName((NatSignalType)payload.signal_type),
                (unsigned)payload.text_len);
            break;
        }

        Rollback::NetplayLog_Verbose("SESSION", -1,
            "Sent NAT traversal signal: type=%s len=%u",
            NatSignalTypeName((NatSignalType)payload.signal_type),
            (unsigned)payload.text_len);
    }
}

// ============================================================================
// v2 handshake (§4.2 — 5-step nonce exchange)
// ============================================================================

static uint64_t Fnv1a64(const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// session_id = fnv1a64(client_nonce || host_nonce || host_seed), all
// little-endian in wire order.
static uint64_t ComputeSessionId(uint64_t clientNonce, uint64_t hostNonce,
                                 uint32_t hostSeed) {
    uint8_t buf[20];
    memcpy(buf, &clientNonce, 8);
    memcpy(buf + 8, &hostNonce, 8);
    memcpy(buf + 16, &hostSeed, 4);
    return Fnv1a64(buf, sizeof(buf));
}

// Fresh nonzero nonce per attempt (C-7: replays from prior sessions are
// structurally inert).
static uint64_t GenerateNonce() {
    std::random_device rd;
    uint64_t value = ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    value ^= (uint64_t)qpc.QuadPart * 0x9E3779B97F4A7C15ull;
    if (value == 0) {
        value = 1;
    }
    return value;
}

static void EnterHandshakeStep(HandshakeStep step) {
    if (s_hsStep == step) return;
    Rollback::NetplayLog_Write("SESSION", -1,
        "Handshake step: %s -> %s role=%s",
        HandshakeStepName(s_hsStep),
        HandshakeStepName(step),
        SessionRoleName(s_role));
    s_hsStep = step;
    s_hsStepEnteredAt = GetTickCount();
    s_hsLastResendAt = 0;  // send immediately on the next pump
}

// Fail-closed refusal at the handshake (C-3): reliable Disconnect naming the
// exact field, then ENet-level disconnect with the compatibility data word.
static void RefuseHandshake(const char* msg) {
    Rollback::NetplayLog_Write("SESSION", -1,
        "Handshake refusal: %s state=%s role=%s token=%u",
        msg ? msg : "?",
        SessionStateName(s_state),
        SessionRoleName(s_role),
        s_activeSessionToken);
    Session2_Terminate(Session2TerminalReason::HandshakeRefused, msg);
}

static void SendSessionHello(const char* context) {
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::SessionHello,
                          &s_hsHelloSent, sizeof(s_hsHelloSent), true, context)) {
        SetError("Failed to send SessionHello");
        return;
    }
    Rollback::NetplayLog_Verbose("SESSION", -1,
        "Sent SessionHello (%s): ver=%u hash=0x%08X cadence=%u/%u nonce=0x%016llX nick=%s",
        context,
        s_hsHelloSent.proto_ver,
        s_hsHelloSent.build_hash,
        s_hsHelloSent.cadence_num,
        s_hsHelloSent.cadence_den,
        (unsigned long long)s_hsHelloSent.client_nonce,
        s_hsHelloSent.nickname);
}

static void BeginClientHandshake() {
    memset(&s_hsHelloSent, 0, sizeof(s_hsHelloSent));
    s_hsClientNonce = GenerateNonce();
    s_hsHelloSent.proto_ver = PROTOCOL_VERSION;
    s_hsHelloSent.build_hash = s_config.build_hash;
    LocalCadenceRational(&s_hsHelloSent.cadence_num, &s_hsHelloSent.cadence_den);
    s_hsHelloSent.client_nonce = s_hsClientNonce;
    strncpy_s(s_hsHelloSent.nickname, sizeof(s_hsHelloSent.nickname),
              s_config.nickname, _TRUNCATE);

    EnterHandshakeStep(HandshakeStep::AwaitOffer);
    SendSessionHello("handshake-begin");
    s_hsLastResendAt = GetTickCount();
}

static void SendSessionOffer(const char* context) {
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::SessionOffer,
                          &s_hsOfferSent, sizeof(s_hsOfferSent), true, context)) {
        SetError("Failed to send SessionOffer");
    }
}

static void SendSessionAck(const char* context) {
    // INV-13: the Ack is built from the received Offer BYTES, never recomputed.
    SessionAckPayload ack{};
    ack.offer_echo = s_hsOfferReceived;
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::SessionAck,
                          &ack, sizeof(ack), true, context)) {
        SetError("Failed to send SessionAck");
    }
}

static void SendSessionConfirm(const char* context) {
    SessionConfirmPayload confirm{};
    confirm.session_id = s_sessionId;
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::SessionConfirm,
                          &confirm, sizeof(confirm), true, context)) {
        SetError("Failed to send SessionConfirm");
    }
}

static void SendSessionConfirmAck(const char* context) {
    SessionConfirmAckPayload ack{};
    ack.session_id = s_sessionId;
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::SessionConfirmAck,
                          &ack, sizeof(ack), true, context)) {
        SetError("Failed to send SessionConfirmAck");
    }
}

// One-shot identity exchange after Connected: fills the preserved PeerInfo
// contract (full nickname / round option / frame timing / HUD style) that the
// wire-frozen v2 handshake payloads deliberately do not carry.
static void SendPeerIdentity() {
    if (s_localIdentitySent ||
        (s_state != SessionState::Connected && s_state != SessionState::Ready)) {
        return;
    }

    PeerIdentityPayload identity{};
    strncpy_s(identity.nickname, sizeof(identity.nickname), s_config.nickname, _TRUNCATE);
    identity.listen_port = s_config.listen_port;
    identity.round_count = GameSettingsSync_ReadRoundOption();
    identity.frame_timing_mode = (uint8_t)LocalFrameTimingMode();
    identity.hud_style_valid = 1;
    NetplayHudStyle::WireStyle wire{};
    NetplayHudStyle::PackWire(&wire);
    identity.hud_trail_r = wire.trail_r;
    identity.hud_trail_g = wire.trail_g;
    identity.hud_trail_b = wire.trail_b;
    identity.hud_text_r = wire.text_r;
    identity.hud_text_g = wire.text_g;
    identity.hud_text_b = wire.text_b;
    identity.hud_trail_length = wire.trail_length;
    identity.hud_score_r = wire.score_r;
    identity.hud_score_g = wire.score_g;
    identity.hud_score_b = wire.score_b;
    identity.hud_font_size = wire.font_size;
    identity.hud_vertical_position = 0;

    if (QueueTypedPacket(CHANNEL_CONTROL, PacketType::PeerIdentity,
                         &identity, sizeof(identity), true, "peer-identity")) {
        s_localIdentitySent = true;
        Rollback::NetplayLog_Write("SESSION", -1,
            "Sent PeerIdentity: nick=%s listen_port=%u rounds=%u timing=%s",
            identity.nickname,
            identity.listen_port,
            identity.round_count,
            FrameTimingModeName((FrameTimingMode)identity.frame_timing_mode));
    }
}

static void OnPeerIdentity(const void* payload, size_t payloadLen) {
    if (payloadLen < sizeof(PeerIdentityPayload)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "PeerIdentity payload too small: got=%zu expected=%zu",
            payloadLen, sizeof(PeerIdentityPayload));
        return;
    }

    const PeerIdentityPayload* identity =
        static_cast<const PeerIdentityPayload*>(payload);

    char remoteNickname[sizeof(s_remotePeer.nickname)] = {};
    memcpy(remoteNickname, identity->nickname,
           sizeof(remoteNickname) < sizeof(identity->nickname)
               ? sizeof(remoteNickname)
               : sizeof(identity->nickname));
    remoteNickname[sizeof(remoteNickname) - 1] = '\0';

    const uint8_t normalizedRoundOption = GameSettingsSync_NormalizeRoundOption(
        identity->round_count, "peer identity");
    const bool validTiming = FrameTimingMode_IsValid(identity->frame_timing_mode);

    s_remotePeer.valid = true;
    s_remotePeer.protocol_version = PROTOCOL_VERSION;   // handshake-verified
    s_remotePeer.build_hash = s_config.build_hash;      // handshake-verified equal
    s_remotePeer.listen_port = identity->listen_port;
    s_remotePeer.round_count_valid = true;
    s_remotePeer.round_count = normalizedRoundOption;
    s_remotePeer.frame_timing_valid = validTiming;
    s_remotePeer.frame_timing_mode = identity->frame_timing_mode;
    memset(s_remotePeer.nickname, 0, sizeof(s_remotePeer.nickname));
    strncpy_s(s_remotePeer.nickname, sizeof(s_remotePeer.nickname),
              remoteNickname, _TRUNCATE);

    if (identity->hud_style_valid) {
        s_remotePeer.hud_style_valid = true;
        s_remotePeer.hud_trail_r = identity->hud_trail_r;
        s_remotePeer.hud_trail_g = identity->hud_trail_g;
        s_remotePeer.hud_trail_b = identity->hud_trail_b;
        s_remotePeer.hud_text_r = identity->hud_text_r;
        s_remotePeer.hud_text_g = identity->hud_text_g;
        s_remotePeer.hud_text_b = identity->hud_text_b;
        s_remotePeer.hud_trail_length = identity->hud_trail_length;
        s_remotePeer.hud_score_r = identity->hud_score_r;
        s_remotePeer.hud_score_g = identity->hud_score_g;
        s_remotePeer.hud_score_b = identity->hud_score_b;
        s_remotePeer.hud_font_size = identity->hud_font_size;
        s_remotePeer.hud_vertical_position = identity->hud_vertical_position;
    }

    Rollback::NetplayLog_Write("SESSION", -1,
        "Remote PeerIdentity: nick=%s listen_port=%u rounds_raw=%u rounds_to_win=%d timing=%s hud=%d",
        s_remotePeer.nickname,
        s_remotePeer.listen_port,
        s_remotePeer.round_count,
        GameSettingsSync_RoundsToWin(s_remotePeer.round_count),
        validTiming ? FrameTimingModeName((FrameTimingMode)identity->frame_timing_mode) : "invalid",
        identity->hud_style_valid ? 1 : 0);

    // Round option stays host-authoritative exactly as before (the joiner
    // aligned at legacy HelloAck; now at the host's identity packet). Frame
    // timing was already fail-closed verified equal at the handshake, so the
    // override below is a formality that keeps the old logs/semantics.
    if (s_role == SessionRole::Join && validTiming) {
        const FrameTimingMode hostTiming = (FrameTimingMode)identity->frame_timing_mode;
        TickHooks_SetFrameLimiter60FpsSessionOverride(
            hostTiming == FrameTimingMode::Proper60,
            "host identity timing");
        GameSettingsSync_ApplyRoundOption(normalizedRoundOption, "host identity");
        Rollback::NetplayLog_Write("SESSION", -1,
            "Join aligned FPS timing to host: timing=%s display=%s",
            FrameTimingModeName(hostTiming),
            FrameTimingModeDisplayName(hostTiming));
    }
}

// Minimal PeerInfo fill straight from the handshake so status text has a
// nickname before the identity packet lands.
static void SeedPeerInfoFromHandshake(const char* shortNickname) {
    s_remotePeer.valid = true;
    s_remotePeer.protocol_version = PROTOCOL_VERSION;
    s_remotePeer.build_hash = s_config.build_hash;
    memset(s_remotePeer.nickname, 0, sizeof(s_remotePeer.nickname));
    if (shortNickname) {
        strncpy_s(s_remotePeer.nickname, sizeof(s_remotePeer.nickname),
                  shortNickname, _TRUNCATE);
    }
}

static void EnterConnected(const char* how) {
    Rollback::NetplayLog_Write("SESSION", -1,
        "Handshake complete (%s): session_id=0x%016llX role=%s",
        how,
        (unsigned long long)s_sessionId,
        SessionRoleName(s_role));
    EnterHandshakeStep(HandshakeStep::Done);
    SetState(SessionState::Connected);
    SendPeerIdentity();
    SendNatInfo();
}

// --- Handshake packet handlers -------------------------------------------

static void OnSessionHello(const void* payload, size_t payloadLen) {
    if (s_role != SessionRole::Host) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionHello in role=%s", SessionRoleName(s_role));
        return;
    }
    if (payloadLen < sizeof(SessionHelloPayload)) {
        RefuseHandshake("Handshake refused: SessionHello payload too small");
        return;
    }

    const SessionHelloPayload* hello =
        static_cast<const SessionHelloPayload*>(payload);

    if (s_hsStep == HandshakeStep::AwaitAck ||
        s_hsStep == HandshakeStep::AwaitConfirmAck ||
        s_hsStep == HandshakeStep::Done) {
        // Client resend raced our Offer: accept only the identical Hello and
        // let the step's own resend timer re-deliver the Offer.
        if (memcmp(hello, &s_hsOfferSent.hello_echo, sizeof(*hello)) != 0) {
            RefuseHandshake("Handshake refused: divergent SessionHello resend");
        }
        return;
    }

    if (s_hsStep != HandshakeStep::AwaitHello) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionHello in handshake step %s", HandshakeStepName(s_hsStep));
        return;
    }

    // Fail-closed validation naming the exact field (C-3).
    char msg[128];
    if (hello->proto_ver != PROTOCOL_VERSION) {
        snprintf(msg, sizeof(msg),
                 "Handshake refused: protocol version mismatch (local=%u remote=%u)",
                 PROTOCOL_VERSION, hello->proto_ver);
        RefuseHandshake(msg);
        return;
    }
    if (hello->build_hash != s_config.build_hash) {
        snprintf(msg, sizeof(msg),
                 "Handshake refused: build hash mismatch (local=0x%08X remote=0x%08X)",
                 s_config.build_hash, hello->build_hash);
        RefuseHandshake(msg);
        return;
    }
    uint16_t localNum = 0, localDen = 0;
    LocalCadenceRational(&localNum, &localDen);
    if (hello->cadence_num != localNum || hello->cadence_den != localDen) {
        snprintf(msg, sizeof(msg),
                 "Handshake refused: cadence profile mismatch (local=%u/%u remote=%u/%u)",
                 localNum, localDen, hello->cadence_num, hello->cadence_den);
        RefuseHandshake(msg);
        return;
    }
    if (hello->client_nonce == 0) {
        RefuseHandshake("Handshake refused: zero client nonce");
        return;
    }

    s_hsClientNonce = hello->client_nonce;
    s_hsHostNonce = GenerateNonce();
    std::random_device rd;
    s_hsHostSeed = (uint32_t)rd();

    // INV-13: the Offer echoes the received Hello BYTES verbatim.
    memset(&s_hsOfferSent, 0, sizeof(s_hsOfferSent));
    memcpy(&s_hsOfferSent.hello_echo, hello, sizeof(s_hsOfferSent.hello_echo));
    s_hsOfferSent.host_nonce = s_hsHostNonce;
    s_hsOfferSent.host_seed = s_hsHostSeed;
    strncpy_s(s_hsOfferSent.host_nickname, sizeof(s_hsOfferSent.host_nickname),
              s_config.nickname, _TRUNCATE);

    char shortNick[sizeof(hello->nickname) + 1] = {};
    memcpy(shortNick, hello->nickname, sizeof(hello->nickname));
    SeedPeerInfoFromHandshake(shortNick);

    Rollback::NetplayLog_Write("SESSION", -1,
        "Accepted SessionHello: nick=%s ver=%u hash=0x%08X cadence=%u/%u nonce=0x%016llX",
        shortNick,
        hello->proto_ver,
        hello->build_hash,
        hello->cadence_num,
        hello->cadence_den,
        (unsigned long long)hello->client_nonce);

    EnterHandshakeStep(HandshakeStep::AwaitAck);
    SendSessionOffer("hello-accepted");
    s_hsLastResendAt = GetTickCount();
}

static void OnSessionOffer(const void* payload, size_t payloadLen) {
    if (s_role != SessionRole::Join) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionOffer in role=%s", SessionRoleName(s_role));
        return;
    }
    if (payloadLen < sizeof(SessionOfferPayload)) {
        RefuseHandshake("Handshake refused: SessionOffer payload too small");
        return;
    }

    const SessionOfferPayload* offer =
        static_cast<const SessionOfferPayload*>(payload);

    if (s_hsStep == HandshakeStep::AwaitConfirm || s_hsStep == HandshakeStep::Done) {
        // Host resend raced our Ack: re-echo the identical Offer.
        if (memcmp(offer, &s_hsOfferReceived, sizeof(*offer)) == 0) {
            SendSessionAck("offer-resend");
        } else {
            RefuseHandshake("Handshake refused: divergent SessionOffer resend");
        }
        return;
    }

    if (s_hsStep != HandshakeStep::AwaitOffer) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionOffer in handshake step %s", HandshakeStepName(s_hsStep));
        return;
    }

    // Echo-verbatim check (INV-13): our own Hello bytes must come back exact.
    if (memcmp(&offer->hello_echo, &s_hsHelloSent, sizeof(s_hsHelloSent)) != 0) {
        RefuseHandshake("Handshake refused: SessionOffer hello-echo mismatch");
        return;
    }
    if (offer->host_nonce == 0) {
        RefuseHandshake("Handshake refused: zero host nonce");
        return;
    }

    s_hsOfferReceived = *offer;
    s_hsHostNonce = offer->host_nonce;
    s_hsHostSeed = offer->host_seed;
    s_sessionId = ComputeSessionId(s_hsClientNonce, s_hsHostNonce, s_hsHostSeed);

    char shortNick[sizeof(offer->host_nickname) + 1] = {};
    memcpy(shortNick, offer->host_nickname, sizeof(offer->host_nickname));
    SeedPeerInfoFromHandshake(shortNick);

    Rollback::NetplayLog_Write("SESSION", -1,
        "Accepted SessionOffer: host_nick=%s host_nonce=0x%016llX host_seed=0x%08X session_id=0x%016llX",
        shortNick,
        (unsigned long long)s_hsHostNonce,
        s_hsHostSeed,
        (unsigned long long)s_sessionId);

    EnterHandshakeStep(HandshakeStep::AwaitConfirm);
    SendSessionAck("offer-accepted");
    s_hsLastResendAt = GetTickCount();
}

static void OnSessionAck(const void* payload, size_t payloadLen) {
    if (s_role != SessionRole::Host) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionAck in role=%s", SessionRoleName(s_role));
        return;
    }
    if (payloadLen < sizeof(SessionAckPayload)) {
        RefuseHandshake("Handshake refused: SessionAck payload too small");
        return;
    }

    const SessionAckPayload* ack = static_cast<const SessionAckPayload*>(payload);

    if (s_hsStep == HandshakeStep::AwaitConfirmAck || s_hsStep == HandshakeStep::Done) {
        // Client resend raced our Confirm; the resend timer re-delivers it.
        return;
    }

    if (s_hsStep != HandshakeStep::AwaitAck) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionAck in handshake step %s", HandshakeStepName(s_hsStep));
        return;
    }

    // Echo-verbatim check (INV-13): our own Offer bytes must come back exact.
    if (memcmp(&ack->offer_echo, &s_hsOfferSent, sizeof(s_hsOfferSent)) != 0) {
        RefuseHandshake("Handshake refused: SessionAck offer-echo mismatch");
        return;
    }

    // The Ack's arrival fixes the peer endpoint (§4.2 step 4).
    s_sessionId = ComputeSessionId(s_hsClientNonce, s_hsHostNonce, s_hsHostSeed);

    EnterHandshakeStep(HandshakeStep::AwaitConfirmAck);
    SendSessionConfirm("ack-accepted");
    s_hsLastResendAt = GetTickCount();
}

static void OnSessionConfirm(const void* payload, size_t payloadLen) {
    if (s_role != SessionRole::Join) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionConfirm in role=%s", SessionRoleName(s_role));
        return;
    }
    if (payloadLen < sizeof(SessionConfirmPayload)) {
        RefuseHandshake("Handshake refused: SessionConfirm payload too small");
        return;
    }

    const SessionConfirmPayload* confirm =
        static_cast<const SessionConfirmPayload*>(payload);

    if (s_hsStep == HandshakeStep::Done) {
        // Our ConfirmAck was lost; the host is resending Confirm. Re-ack.
        if (confirm->session_id == s_sessionId) {
            SendSessionConfirmAck("confirm-resend");
        }
        return;
    }

    if (s_hsStep != HandshakeStep::AwaitConfirm) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionConfirm in handshake step %s", HandshakeStepName(s_hsStep));
        return;
    }

    // Final echo check: both sides computed the same id from the same nonces.
    if (confirm->session_id != s_sessionId) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Handshake refused: session_id mismatch (local=0x%016llX remote=0x%016llX)",
                 (unsigned long long)s_sessionId,
                 (unsigned long long)confirm->session_id);
        RefuseHandshake(msg);
        return;
    }

    SendSessionConfirmAck("confirm-accepted");
    EnterConnected("client confirm");
}

static void OnSessionConfirmAck(const void* payload, size_t payloadLen) {
    if (s_role != SessionRole::Host) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionConfirmAck in role=%s", SessionRoleName(s_role));
        return;
    }
    if (payloadLen < sizeof(SessionConfirmAckPayload)) {
        RefuseHandshake("Handshake refused: SessionConfirmAck payload too small");
        return;
    }

    const SessionConfirmAckPayload* ack =
        static_cast<const SessionConfirmAckPayload*>(payload);

    if (s_hsStep == HandshakeStep::Done) {
        return;  // duplicate — harmless
    }

    if (s_hsStep != HandshakeStep::AwaitConfirmAck) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Ignoring SessionConfirmAck in handshake step %s", HandshakeStepName(s_hsStep));
        return;
    }

    if (ack->session_id != s_sessionId) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Handshake refused: session_id mismatch (local=0x%016llX remote=0x%016llX)",
                 (unsigned long long)s_sessionId,
                 (unsigned long long)ack->session_id);
        RefuseHandshake(msg);
        return;
    }

    EnterConnected("host confirm-ack");
}

// Per-step 200 ms resend driver (§4.2). Runs from Session_Update while the
// handshake is live; the resend always re-sends the CURRENT step's packet.
static void PumpHandshakeResends() {
    if (s_state != SessionState::Handshaking) {
        return;
    }
    if (s_hsStep == HandshakeStep::None ||
        s_hsStep == HandshakeStep::AwaitHello ||
        s_hsStep == HandshakeStep::Done) {
        return;  // nothing to resend in these steps
    }

    const DWORD now = GetTickCount();
    if (s_hsLastResendAt != 0 && (now - s_hsLastResendAt) < kHandshakeResendIntervalMs) {
        return;
    }
    s_hsLastResendAt = now;

    switch (s_hsStep) {
        case HandshakeStep::AwaitOffer:      SendSessionHello("resend");   break;
        case HandshakeStep::AwaitConfirm:    SendSessionAck("resend");     break;
        case HandshakeStep::AwaitAck:        SendSessionOffer("resend");   break;
        case HandshakeStep::AwaitConfirmAck: SendSessionConfirm("resend"); break;
        default: break;
    }
}

// ============================================================================
// Event handlers
// ============================================================================

static void OnTransportConnected(uintptr_t peerToken) {
    s_peerToken = peerToken;
    LOG_INFO("[Session] ENet connected (peer 0x%llX)", (unsigned long long)peerToken);
    Rollback::NetplayLog_Write("SESSION", -1,
        "ENet connected: peer=0x%llX role=%s token=%u",
        (unsigned long long)peerToken,
        SessionRoleName(s_role),
        s_activeSessionToken);

    if (s_state == SessionState::Connecting) {
        SetState(SessionState::Handshaking);
        if (s_role == SessionRole::Join) {
            BeginClientHandshake();   // client speaks first (§4.2 step 2)
        } else {
            EnterHandshakeStep(HandshakeStep::AwaitHello);
        }
    }
}

static void OnTransportDisconnect(uintptr_t peerToken, uint32_t data, DWORD transportTickMs) {
    const DWORD now = GetTickCount();
    const DWORD consumeLagMs = now - transportTickMs;
    LOG_INFO("[Session] ENet disconnected (peer 0x%llX, data=%u, lag=%lums)",
             (unsigned long long)peerToken, data, (unsigned long)consumeLagMs);
    Rollback::NetplayLog_Write("SESSION", -1,
        "ENet disconnected: peer=0x%llX data=%u state=%s token=%u consume_lag=%lums",
        (unsigned long long)peerToken,
        data,
        SessionStateName(s_state),
        s_activeSessionToken,
        (unsigned long)consumeLagMs);
    s_peerToken = 0;

    if (IsCompatibilityDisconnectData(data) &&
        s_state != SessionState::Idle &&
        s_state != SessionState::Failed &&
        s_state != SessionState::Disconnecting) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Compatibility disconnect received: peer=0x%llX data=%u state=%s role=%s token=%u",
            (unsigned long long)peerToken,
            data,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
        SetError("Remote peer rejected session due to version/build mismatch");
        return;
    }

    if (IsBusyDisconnectData(data) &&
        s_state != SessionState::Idle &&
        s_state != SessionState::Failed &&
        s_state != SessionState::Disconnecting) {
        SetError("Host is busy with another session");
        return;
    }

    if (IsUserCancelDisconnectData(data) &&
        s_state != SessionState::Idle &&
        s_state != SessionState::Failed &&
        s_state != SessionState::Disconnecting) {
        SetError("Remote canceled the session");
        return;
    }

    if ((s_state == SessionState::Connecting || s_state == SessionState::Handshaking) &&
        TryRelayFallback("transport-disconnect")) {
        LOG_INFO("[Session] Direct connect dropped; retrying via relay fallback");
        return;
    }

    if (s_state == SessionState::Disconnecting) {
        ResetState();
    } else if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
        SetError("Peer disconnected unexpectedly");
    }
}

static void OnPacketReceived(uintptr_t peerToken, uint8_t channelID,
                             const void* data, size_t length,
                             DWORD transportTickMs) {
    if (!ValidatePacketSize(data, length)) {
        LOG_WARN("[Session] Received too-small packet (len=%zu)", length);
        Rollback::NetplayLog_Write("SESSION", -1,
            "DROP recv too-small: peer=0x%llX ch=%u len=%zu state=%s role=%s token=%u",
            (unsigned long long)peerToken,
            channelID,
            length,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
        return;
    }

    const DWORD now = GetTickCount();
    const DWORD consumeLagMs = now - transportTickMs;
    if (consumeLagMs >= 50 && (now - s_lastDrainLagLogAt) >= 250) {
        s_lastDrainLagLogAt = now;
        Rollback::NetplayLog_Write("SESSION", -1,
            "Inbound packet consume lag: %lums peer=0x%llX ch=%u len=%zu token=%u",
            (unsigned long)consumeLagMs,
            (unsigned long long)peerToken,
            channelID,
            length,
            s_activeSessionToken);
    }

    PacketType type = ReadPacketType(data);
    const void* payload = GetPayloadPtr(data);
    size_t payloadLen = GetPayloadSize(length);

    NotePacketReceived(channelID, type, payloadLen);

    switch (type) {
        case PacketType::Ping: {
            // Supervisor heartbeat — answer so the sender's app-level silence
            // clears too. Handled here so no packet-callback owner has to.
            if (payloadLen >= sizeof(PingPayload)) {
                const PingPayload* ping = static_cast<const PingPayload*>(payload);
                PongPayload pong{};
                pong.ping_id = ping->ping_id;
                pong.original_send_time_ms = ping->send_time_ms;
                pong.responder_time_ms = GetTickCount();
                Session_SendPacket(CHANNEL_CONTROL, PacketType::Pong,
                                   &pong, sizeof(pong), true);
            }
            return;
        }

        case PacketType::Pong:
            // Liveness evidence only; inbound timestamps already updated.
            return;

        // --- v2 handshake (§4.2) ---
        case PacketType::SessionHello:
            if (s_state == SessionState::Connecting && s_role == SessionRole::Host) {
                // Host received Hello before we noticed the connect event.
                s_peerToken = peerToken;
                SetState(SessionState::Handshaking);
                EnterHandshakeStep(HandshakeStep::AwaitHello);
            }
            if (s_state == SessionState::Handshaking) {
                OnSessionHello(payload, payloadLen);
            }
            return;

        case PacketType::SessionOffer:
            if (s_state == SessionState::Handshaking) {
                OnSessionOffer(payload, payloadLen);
            }
            return;

        case PacketType::SessionAck:
            if (s_state == SessionState::Handshaking) {
                OnSessionAck(payload, payloadLen);
            }
            return;

        case PacketType::SessionConfirm:
            // Also handled after Connected: a lost ConfirmAck makes the host
            // resend Confirm — re-ack idempotently (see OnSessionConfirm).
            if (s_state == SessionState::Handshaking ||
                s_state == SessionState::Connected ||
                s_state == SessionState::Ready) {
                OnSessionConfirm(payload, payloadLen);
            }
            return;

        case PacketType::SessionConfirmAck:
            if (s_state == SessionState::Handshaking) {
                OnSessionConfirmAck(payload, payloadLen);
            }
            return;

        case PacketType::PeerIdentity:
            OnPeerIdentity(payload, payloadLen);
            return;

        case PacketType::NatInfo:
            if (payloadLen >= sizeof(NatInfoPayload)) {
                const NatInfoPayload* np = static_cast<const NatInfoPayload*>(payload);
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Remote NatInfo: flags=0x%02X extra=0x%02X upnp=%u stun=%u listen=%u external_port=%u external_ip=%s",
                    np->flags,
                    np->extra_flags,
                    np->upnp_status,
                    np->stun_status,
                    np->listen_port,
                    np->external_port,
                    np->external_ip);
            } else {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Remote NatInfo payload too small: got=%zu expected=%zu",
                    payloadLen,
                    sizeof(NatInfoPayload));
            }
            break;

        case PacketType::NatTraversalSignal:
            if (payloadLen >= offsetof(NatTraversalSignalPayload, text)) {
                const NatTraversalSignalPayload* np =
                    static_cast<const NatTraversalSignalPayload*>(payload);
                size_t textLen = np->text_len;
                if (textLen >= NAT_SIGNAL_TEXT_MAX) {
                    textLen = NAT_SIGNAL_TEXT_MAX - 1;
                }
                if (textLen > payloadLen - offsetof(NatTraversalSignalPayload, text)) {
                    textLen = payloadLen - offsetof(NatTraversalSignalPayload, text);
                }

                NatSignalMessage msg{};
                msg.type = (NatSignalType)np->signal_type;
                if (textLen > 0) {
                    memcpy(msg.text, np->text, textLen);
                }
                msg.text[textLen] = '\0';
                Nat_SubmitRemoteSignal(&msg);

                Rollback::NetplayLog_Verbose("SESSION", -1,
                    "Received NAT traversal signal: type=%s len=%u",
                    NatSignalTypeName(msg.type),
                    (unsigned)textLen);
            } else {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "NatTraversalSignal payload too small: got=%zu expected_at_least=%zu",
                    payloadLen,
                    offsetof(NatTraversalSignalPayload, text));
            }
            break;

        case PacketType::Ready:
            s_remoteReady = true;
            LOG_INFO("[Session] Remote peer signaled Ready");
            Rollback::NetplayLog_Write("SESSION", -1,
                "Remote Ready received: local_ready=%d remote_ready=%d",
                s_localReady ? 1 : 0, s_remoteReady ? 1 : 0);
            if (s_localReady && s_remoteReady && s_state == SessionState::Connected) {
                SetState(SessionState::Ready);
            }
            break;

        case PacketType::Disconnect: {
            const char* reason = "Remote disconnected";
            if (payloadLen >= sizeof(DisconnectPayload)) {
                const DisconnectPayload* dp = static_cast<const DisconnectPayload*>(payload);
                reason = dp->message;
            }
            LOG_INFO("[Session] Received Disconnect: %s", reason);
            Rollback::NetplayLog_Write("SESSION", -1,
                "Remote Disconnect received: %s", reason);
            if (s_activeSessionToken != 0) {
                Transport2_RequestDisconnect(s_activeSessionToken, 0, true);
                Transport2_RequestDestroyHost(s_activeSessionToken);
                Transport2_ClearQueues(s_activeSessionToken);
            }
            s_peerToken = 0;
            SetError(reason && reason[0] ? reason : "Remote disconnected");
            break;
        }

        default:
            // Forward to the packet sink (packet_router by default; an
            // explicitly installed callback overrides it).
            if (s_packetCallback) {
                s_packetCallback(type, payload, payloadLen);
            } else {
                if (!DeferControlPacket(channelID, type, payload, payloadLen)) {
                    Rollback::NetplayLog_Write("SESSION", -1,
                        "Unhandled packet with no callback: type=%s ch=%u payload=%zu state=%s role=%s peer=0x%llX",
                        PacketTypeName(type), channelID, payloadLen,
                        SessionStateName(s_state), SessionRoleName(s_role),
                        (unsigned long long)peerToken);
                    Rollback::NetplayLog_Flush();
                }
            }
            break;
    }
}

// ============================================================================
// Timeout checks
// ============================================================================

static void CheckTimeouts() {
    DWORD now = GetTickCount();
    DWORD elapsed = now - s_stateEnteredAt;

    switch (s_state) {
        case SessionState::Connecting:
            // Host listens indefinitely — only joiner has a connect timeout
            if (s_role == SessionRole::Join && elapsed > s_config.connect_timeout_ms) {
                if (TryRelayFallback("connect-timeout")) {
                    Rollback::NetplayLog_Write("SESSION", -1,
                        "Join timeout redirected to relay fallback");
                    break;
                }
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Connect timeout after %lu ms", (unsigned long)elapsed);
                SetError("Connection timed out");
            }
            break;
        case SessionState::Handshaking: {
            // §4.2: 10 s per step, 30 s total → back to menu with reason.
            // No session existed yet, so this is not a kill path (C-4).
            const DWORD stepElapsed =
                (s_hsStepEnteredAt != 0) ? (now - s_hsStepEnteredAt) : elapsed;
            if (elapsed > kHandshakeTotalTimeoutMs) {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Handshake total timeout after %lu ms (step=%s)",
                    (unsigned long)elapsed, HandshakeStepName(s_hsStep));
                SetError("Handshake timed out");
            } else if (stepElapsed > kHandshakeStepTimeoutMs) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Handshake timed out (step %s)",
                         HandshakeStepName(s_hsStep));
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Handshake step timeout after %lu ms (step=%s)",
                    (unsigned long)stepElapsed, HandshakeStepName(s_hsStep));
                SetError(msg);
            }
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// Update peer stats
// ============================================================================

static void UpdateStats() {
    Transport2Stats netStats{};
    Transport2_GetStats(&netStats);

    if (netStats.peer_connected) {
        s_stats.rtt_ms = netStats.rtt_ms;
        s_stats.rtt_variance_ms = netStats.rtt_variance_ms;
        s_stats.packets_sent = netStats.packets_sent;
        s_stats.packets_lost = netStats.packets_lost;
        RememberLiveStats();
    } else if (s_lastLiveStatsValid && s_state != SessionState::Idle) {
        s_stats = s_lastLiveStats;
    } else {
        s_stats.rtt_ms = netStats.rtt_ms;
        s_stats.rtt_variance_ms = netStats.rtt_variance_ms;
        s_stats.packets_sent = netStats.packets_sent;
        s_stats.packets_lost = netStats.packets_lost;
    }

    const DWORD now = GetTickCount();
    const DWORD workerAgeMs =
        (netStats.last_service_tick_ms > 0 && now >= netStats.last_service_tick_ms)
            ? (now - netStats.last_service_tick_ms)
            : 0;
    const DWORD stateAgeMs =
        (s_stateEnteredAt > 0 && now >= s_stateEnteredAt)
            ? (now - s_stateEnteredAt)
            : 0;

    if ((s_state == SessionState::Connected ||
         s_state == SessionState::Ready ||
         s_state == SessionState::Handshaking) &&
        s_peerToken != 0 &&
        !netStats.peer_connected &&
        // 5s, not 250ms: at 250ms this raced the worker's own event delivery
        // (peer cleared on the worker thread before the disconnect event was
        // drained on the game thread) and killed live sessions. A real
        // detach also produces a drained disconnect event long before 5s.
        stateAgeMs >= 5000 &&
        netStats.worker_running &&
        s_activeSessionToken != 0) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Transport peer detached without a drained disconnect event: state=%s role=%s token=%u peer=0x%llX state_age=%lums worker_age=%lums last_rtt=%.1f sent=%u recv=%u lost=%u",
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken,
            (unsigned long long)s_peerToken,
            (unsigned long)stateAgeMs,
            (unsigned long)workerAgeMs,
            s_stats.rtt_ms,
            s_stats.packets_sent,
            s_stats.packets_received,
            s_stats.packets_lost);
        SetError("Network transport detached from peer");
    }

    if ((netStats.inbound_queue_depth >= 128 || netStats.outbound_queue_depth >= 128) &&
        (now - s_lastQueueSpikeLogAt) >= 250) {
        s_lastQueueSpikeLogAt = now;
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Queue depth spike: inbound=%u outbound=%u state=%s role=%s token=%u",
            netStats.inbound_queue_depth,
            netStats.outbound_queue_depth,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
    }

    if (netStats.inbound_drop_count != s_lastInboundDropCount) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Inbound queue drops: old=%u new=%u state=%s token=%u",
            s_lastInboundDropCount,
            netStats.inbound_drop_count,
            SessionStateName(s_state),
            s_activeSessionToken);
        s_lastInboundDropCount = netStats.inbound_drop_count;
    }

    if (netStats.outbound_drop_count != s_lastOutboundDropCount) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Outbound queue drops: old=%u new=%u state=%s token=%u",
            s_lastOutboundDropCount,
            netStats.outbound_drop_count,
            SessionStateName(s_state),
            s_activeSessionToken);
        s_lastOutboundDropCount = netStats.outbound_drop_count;
    }

    if (s_state == SessionState::Ready && netStats.peer_connected &&
        netStats.last_inbound_packet_tick_ms > 0) {
        const DWORD inboundSilenceMs =
            (now >= netStats.last_inbound_packet_tick_ms)
                ? (now - netStats.last_inbound_packet_tick_ms)
                : 0;
        const DWORD outboundAgeMs =
            (netStats.last_outbound_packet_tick_ms > 0 &&
             now >= netStats.last_outbound_packet_tick_ms)
                ? (now - netStats.last_outbound_packet_tick_ms)
                : 0xFFFFFFFFu;

        if (inboundSilenceMs >= 500 && outboundAgeMs <= 500 &&
            (s_lastInboundSilenceLogAt == 0 || (now - s_lastInboundSilenceLogAt) >= 250)) {
            s_lastInboundSilenceLogAt = now;
            Rollback::NetplayLog_Write("NTHREAD", -1,
                "Inbound silence while outbound active: silence=%lums protocol_silence=%lums outbound_age=%lums worker_age=%lums "
                "inbound_depth=%u outbound_depth=%u state=%s role=%s token=%u",
                (unsigned long)inboundSilenceMs,
                (unsigned long)netStats.protocol_silence_ms,
                (unsigned long)outboundAgeMs,
                (unsigned long)workerAgeMs,
                netStats.inbound_queue_depth,
                netStats.outbound_queue_depth,
                SessionStateName(s_state),
                SessionRoleName(s_role),
                s_activeSessionToken);
        }
    } else {
        s_lastInboundSilenceLogAt = 0;
    }
}

static void DrainTransportEvents() {
    Transport2Event ev{};
    int drained = 0;
    int staleDropped = 0;

    while (Transport2_TryPopEvent(&ev)) {
        drained++;

        if ((ev.session_token == 0 && s_activeSessionToken != 0) ||
            (ev.session_token != 0 && ev.session_token != s_activeSessionToken)) {
            staleDropped++;
            continue;
        }

        switch (ev.type) {
            case Transport2EventType::Connected:
                OnTransportConnected(ev.peer_token);
                break;

            case Transport2EventType::Disconnected:
                OnTransportDisconnect(ev.peer_token, ev.disconnect_data, ev.transport_tick_ms);
                break;

            case Transport2EventType::PacketReceived:
                OnPacketReceived(ev.peer_token,
                                 ev.channel_id,
                                 ev.packet_data,
                                 ev.packet_len,
                                 ev.transport_tick_ms);
                break;

            case Transport2EventType::WorkerError:
                Rollback::NetplayLog_Write("NTHREAD", -1,
                    "Worker error event: token=%u msg=%s state=%s role=%s",
                    ev.session_token,
                    ev.error_text,
                    SessionStateName(s_state),
                    SessionRoleName(s_role));
                if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
                    SetError(ev.error_text[0] ? ev.error_text : "Network worker error");
                }
                break;
        }
    }

    if (staleDropped > 0) {
        Rollback::NetplayLog_Write("NTHREAD", -1,
            "Dropped %d stale network events (active_token=%u)",
            staleDropped,
            s_activeSessionToken);
    }

    if (drained > 0) {
        Rollback::NetplayLog_Verbose("NTHREAD", -1,
            "Drained network events on game thread: count=%d state=%s role=%s token=%u",
            drained,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            s_activeSessionToken);
    }
}

// The local fail-closed PacingClockDead terminal (M2 obligation): the
// scheduler latches the fault; session2 converts it into exactly one typed
// terminal through the funnel, then hands the UI-level teardown to the menu
// controller's disconnect funnel.
static void CheckPacingClockTerminal() {
    if (s_pacingClockTerminalFired) {
        return;
    }
    if (s_state != SessionState::Connected && s_state != SessionState::Ready) {
        return;
    }
    if (!FrameScheduler_IsPacingClockDead()) {
        return;
    }

    s_pacingClockTerminalFired = true;
    const char* reason = "Game clock stopped (PacingClockDead)";
    Rollback::NetplayLog_Write("SESSION", -1,
        "PacingClockDead latch observed while session active -> terminal");
    Rollback::NetplayLog_Flush();
    Session2_Terminate(Session2TerminalReason::PacingClockDead, reason);
    // Kill-path allowlisted (INV-20 local fail-closed terminal): routes the
    // UI/wiring teardown through the single menu disconnect funnel.
    NetMenu::HandleDisconnection(reason);
}

// ============================================================================
// session2-specific surface
// ============================================================================

const char* Session2TerminalReasonName(Session2TerminalReason reason) {
    switch (reason) {
        case Session2TerminalReason::UserCancel:        return "UserCancel";
        case Session2TerminalReason::GameExit:          return "GameExit";
        case Session2TerminalReason::SupervisorDead:    return "SupervisorDead";
        case Session2TerminalReason::ProgressDeadline:  return "ProgressDeadline";
        case Session2TerminalReason::PacingClockDead:   return "PacingClockDead";
        case Session2TerminalReason::HandshakeRefused:  return "HandshakeRefused";
        case Session2TerminalReason::TransportFailed:   return "TransportFailed";
        case Session2TerminalReason::ProtocolViolation: return "ProtocolViolation";
        case Session2TerminalReason::ConfirmedDesync:   return "ConfirmedDesync";
    }
    return "?";
}

uint64_t Session2_GetSessionId() {
    return s_sessionId;
}

void Session2_Terminate(Session2TerminalReason reason, const char* detail) {
    if (s_state == SessionState::Idle) {
        return;
    }

    const char* text = (detail && detail[0]) ? detail
                                             : Session2TerminalReasonName(reason);
    Rollback::NetplayLog_Write("SESSION", -1,
        "Session2_Terminate: reason=%s detail=%s state=%s role=%s token=%u",
        Session2TerminalReasonName(reason),
        text,
        SessionStateName(s_state),
        SessionRoleName(s_role),
        s_activeSessionToken);
    Rollback::NetplayLog_Flush();

    const uint32_t token = s_activeSessionToken;
    const bool peerReachable =
        token != 0 &&
        (s_state == SessionState::Connected ||
         s_state == SessionState::Ready ||
         s_state == SessionState::Handshaking);

    // Self-describing goodbye when a peer might still hear it (INV-20).
    uint32_t disconnectData = static_cast<uint32_t>(DisconnectReason::Error);
    switch (reason) {
        case Session2TerminalReason::UserCancel:
        case Session2TerminalReason::GameExit:
            disconnectData = static_cast<uint32_t>(DisconnectReason::UserCancel);
            break;
        case Session2TerminalReason::HandshakeRefused:
            disconnectData = static_cast<uint32_t>(DisconnectReason::VersionMismatch);
            break;
        default:
            break;
    }

    if (peerReachable) {
        DisconnectPayload dp{};
        dp.reason_code = static_cast<uint16_t>(disconnectData);
        strncpy(dp.message, text, sizeof(dp.message) - 1);
        dp.message[sizeof(dp.message) - 1] = '\0';
        QueueTypedPacket(CHANNEL_CONTROL, PacketType::Disconnect,
                         &dp, sizeof(dp), true, "terminate");
        Transport2_RequestDisconnect(token, disconnectData, false);
    }

    if (reason == Session2TerminalReason::GameExit) {
        // WM_CLOSE fast-exit: give the worker a bounded moment to deliver the
        // goodbye before the process dies; no state teardown (process exits).
        if (peerReachable) {
            Sleep(150);
        }
        return;
    }

    if (token != 0) {
        Transport2_RequestDestroyHost(token);
        Transport2_ClearQueues(token);
    } else {
        Transport2_RequestDestroyHost(0);
        Transport2_ClearQueues(0);
    }
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session detached from network thread: token=%u", token);

    s_activeSessionToken = 0;
    Nat_StopServices();

    if (reason == Session2TerminalReason::UserCancel) {
        GameSettingsSync_RestoreLocalSession("session cancel");
        ResetState();
    } else {
        // Fault terminal: surface the reason via the Failed state.
        SetError(text);
    }
}

// ============================================================================
// Public API (Session_* facade — session_manager.h, preserved verbatim)
// ============================================================================

void Session_Init() {
    ResetState();
    ClearStatsForNewSession();
    s_activeSessionToken = 0;
    // packet_router is the single dispatch owner (§2.3), registered once here
    // and never handed off. Session_SetPacketCallback(nullptr) restores it.
    s_packetCallback = PacketRouter_OnPacket;
    if (!Transport2_Init()) {
        LOG_ERROR("[Session] Failed to initialize network service thread");
        Rollback::NetplayLog_Write("NTHREAD", -1, "ERROR: network service thread init failed");
    }
    LOG_INFO("[Session] session2 initialized");
}

void Session_Shutdown() {
    if (s_state != SessionState::Idle) {
        Session_Cancel();
    }
    Transport2_Shutdown();
    Nat_StopServices();
    s_activeSessionToken = 0;
    ResetState();
    ClearStatsForNewSession();
    LOG_INFO("[Session] session2 shut down");
}

bool Session_StartHost(const SessionConfig* config) {
    if (!config) return false;
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start host: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    ClearStatsForNewSession();
    memcpy(&s_config, config, sizeof(s_config));
    if (!ComputeLocalBuildHash(&s_config.build_hash)) {
        SetError("Failed to compute exact local build fingerprint");
        return false;
    }
    s_role = SessionRole::Host;
    s_pacingClockTerminalFired = false;
    ResetHandshakeState();
    TickHooks_SetFrameLimiter60FpsSessionOverride(
        TickHooks_GetFrameLimiter60FpsPreferenceEnabled(),
        "start host timing lock");
    GameSettingsSync_BeginNetplaySession("start host");

    char plannedPunchRelayHost[96] = {};
    uint16_t plannedPunchRelayPort = 0;
    GetPunchRelayEndpoint(config, plannedPunchRelayHost, sizeof(plannedPunchRelayHost), &plannedPunchRelayPort);

    Rollback::NetplayLog_Write("SESSION", -1,
        "StartHost: listen_port=%u nick=%s hash=0x%08X connect_timeout=%u handshake_timeout=%u "
        "upnp=%d pcp=%d stun=%d hole_punch=%d turn=%d ipv6=%d pref_direct=%d "
        "backend_upnp=%d backend_pcp=%d backend_stun=%d backend_hole=%d backend_turn=%d punch_relay=%s:%u "
        "frame_timing=%s",
        s_config.listen_port,
        s_config.nickname,
        s_config.build_hash,
        s_config.connect_timeout_ms,
        s_config.handshake_timeout_ms,
        s_config.nat.enable_upnp ? 1 : 0,
        s_config.nat.enable_pcp_fallback ? 1 : 0,
        s_config.nat.enable_stun ? 1 : 0,
        s_config.nat.enable_hole_punch ? 1 : 0,
        s_config.nat.enable_turn ? 1 : 0,
        s_config.nat.allow_ipv6_endpoint ? 1 : 0,
        s_config.nat.prefer_portforwarded_direct ? 1 : 0,
        Nat_IsUpnpBackendAvailable() ? 1 : 0,
        Nat_IsPcpBackendAvailable() ? 1 : 0,
        Nat_IsStunBackendAvailable() ? 1 : 0,
        Nat_IsHolePunchBackendAvailable() ? 1 : 0,
        Nat_IsTurnBackendAvailable() ? 1 : 0,
        plannedPunchRelayHost,
        (unsigned)plannedPunchRelayPort,
        FrameTimingModeName(LocalFrameTimingMode()));

    NatRuntimeConfig natCfg{};
    NatRuntimeConfig_SetDefaults(&natCfg);
    natCfg.enable_upnp = config->nat.enable_upnp;
    natCfg.enable_stun = config->nat.enable_stun;
    natCfg.enable_hole_punch = config->nat.enable_hole_punch;
    natCfg.enable_turn = config->nat.enable_turn;
    natCfg.enable_pcp_fallback = config->nat.enable_pcp_fallback;
    natCfg.allow_ipv6_endpoint = config->nat.allow_ipv6_endpoint;
    natCfg.prefer_portforwarded_direct = config->nat.prefer_portforwarded_direct;
    strncpy_s(natCfg.stun_host, sizeof(natCfg.stun_host), config->nat.stun_host, _TRUNCATE);
    natCfg.stun_port = config->nat.stun_port;
    strncpy_s(natCfg.turn_host, sizeof(natCfg.turn_host), config->nat.turn_host, _TRUNCATE);
    natCfg.turn_port = config->nat.turn_port;
    strncpy_s(natCfg.turn_username, sizeof(natCfg.turn_username), config->nat.turn_username, _TRUNCATE);
    strncpy_s(natCfg.turn_password, sizeof(natCfg.turn_password), config->nat.turn_password, _TRUNCATE);
    natCfg.gather_timeout_ms = config->nat.gather_timeout_ms;
    natCfg.connect_timeout_ms = config->nat.connect_timeout_ms;
    natCfg.mapping_timeout_ms = config->nat.mapping_timeout_ms;
    natCfg.traversal_log_verbosity = config->nat.traversal_log_verbosity;
    Nat_ApplyRuntimeConfig(&natCfg);

    if (!Transport2_Init()) {
        SetError("Failed to initialize network worker");
        return false;
    }

    NextSessionToken();
    Transport2_ClearQueues(0);
    Nat_ClearRemoteHint();
    if (!Transport2_StartHost(s_activeSessionToken,
                              config->listen_port,
                              config->nat.enable_hole_punch,
                              plannedPunchRelayHost,
                              plannedPunchRelayPort)) {
        SetError("Failed to start network host thread command");
        return false;
    }

    Nat_StartServices(config->listen_port);

    SetState(SessionState::Connecting);
    LOG_INFO("[Session] Hosting on port %u, waiting for peer... (token=%u)",
             config->listen_port, s_activeSessionToken);
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session attached to network thread: role=Host token=%u",
        s_activeSessionToken);
    return true;
}

bool Session_StartJoin(const SessionConfig* config) {
    if (!config) return false;
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start join: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    ClearStatsForNewSession();
    memcpy(&s_config, config, sizeof(s_config));
    if (!ComputeLocalBuildHash(&s_config.build_hash)) {
        SetError("Failed to compute exact local build fingerprint");
        return false;
    }
    s_role = SessionRole::Join;
    s_pacingClockTerminalFired = false;
    ResetHandshakeState();
    TickHooks_SetFrameLimiter60FpsSessionOverride(
        TickHooks_GetFrameLimiter60FpsPreferenceEnabled(),
        "start join pending host timing");
    GameSettingsSync_BeginNetplaySession("start join");

    const bool relayConfigured = HasTrafficRelayConfigured(&s_config);

    char plannedPunchRelayHost[96] = {};
    uint16_t plannedPunchRelayPort = 0;
    GetPunchRelayEndpoint(config, plannedPunchRelayHost, sizeof(plannedPunchRelayHost), &plannedPunchRelayPort);

    Rollback::NetplayLog_Write("SESSION", -1,
        "StartJoin: target=%s:%u relay=%s:%u mode=%s nick=%s hash=0x%08X "
        "connect_timeout=%u handshake_timeout=%u upnp=%d pcp=%d stun=%d hole_punch=%d turn=%d "
        "ipv6=%d pref_direct=%d backend_upnp=%d backend_pcp=%d backend_stun=%d backend_hole=%d backend_turn=%d "
        "punch_relay=%s:%u traffic_relay=%d frame_timing=%s",
        s_config.target_host,
        s_config.target_port,
        s_config.nat.relay_host,
        s_config.nat.relay_port,
        ConnectPreferenceName(s_config.connect_preference),
        s_config.nickname,
        s_config.build_hash,
        s_config.connect_timeout_ms,
        s_config.handshake_timeout_ms,
        s_config.nat.enable_upnp ? 1 : 0,
        s_config.nat.enable_pcp_fallback ? 1 : 0,
        s_config.nat.enable_stun ? 1 : 0,
        s_config.nat.enable_hole_punch ? 1 : 0,
        s_config.nat.enable_turn ? 1 : 0,
        s_config.nat.allow_ipv6_endpoint ? 1 : 0,
        s_config.nat.prefer_portforwarded_direct ? 1 : 0,
        Nat_IsUpnpBackendAvailable() ? 1 : 0,
        Nat_IsPcpBackendAvailable() ? 1 : 0,
        Nat_IsStunBackendAvailable() ? 1 : 0,
        Nat_IsHolePunchBackendAvailable() ? 1 : 0,
        Nat_IsTurnBackendAvailable() ? 1 : 0,
        plannedPunchRelayHost,
        (unsigned)plannedPunchRelayPort,
        relayConfigured ? 1 : 0,
        FrameTimingModeName(LocalFrameTimingMode()));

    NatRuntimeConfig natCfg{};
    NatRuntimeConfig_SetDefaults(&natCfg);
    natCfg.enable_upnp = config->nat.enable_upnp;
    natCfg.enable_stun = config->nat.enable_stun;
    natCfg.enable_hole_punch = config->nat.enable_hole_punch;
    natCfg.enable_turn = config->nat.enable_turn;
    natCfg.enable_pcp_fallback = config->nat.enable_pcp_fallback;
    natCfg.allow_ipv6_endpoint = config->nat.allow_ipv6_endpoint;
    natCfg.prefer_portforwarded_direct = config->nat.prefer_portforwarded_direct;
    strncpy_s(natCfg.stun_host, sizeof(natCfg.stun_host), config->nat.stun_host, _TRUNCATE);
    natCfg.stun_port = config->nat.stun_port;
    strncpy_s(natCfg.turn_host, sizeof(natCfg.turn_host), config->nat.turn_host, _TRUNCATE);
    natCfg.turn_port = config->nat.turn_port;
    strncpy_s(natCfg.turn_username, sizeof(natCfg.turn_username), config->nat.turn_username, _TRUNCATE);
    strncpy_s(natCfg.turn_password, sizeof(natCfg.turn_password), config->nat.turn_password, _TRUNCATE);
    natCfg.gather_timeout_ms = config->nat.gather_timeout_ms;
    natCfg.connect_timeout_ms = config->nat.connect_timeout_ms;
    natCfg.mapping_timeout_ms = config->nat.mapping_timeout_ms;
    natCfg.traversal_log_verbosity = config->nat.traversal_log_verbosity;
    Nat_ApplyRuntimeConfig(&natCfg);

    if (!Transport2_Init()) {
        SetError("Failed to initialize network worker");
        return false;
    }

    const char* initialHost = config->target_host;
    uint16_t initialPort = config->target_port;
    bool initialViaRelay = false;
    const bool directLooksIPv6 = (strchr(config->target_host, ':') != nullptr);

    switch (config->connect_preference) {
        case ConnectPreference::RelayOnly:
            if (!relayConfigured) {
                SetError("Traffic relay mode is not available; use Automatic with UDP hole punch");
                return false;
            }
            initialHost = config->nat.relay_host;
            initialPort = config->nat.relay_port;
            initialViaRelay = true;
            break;

        case ConnectPreference::DirectOnly:
            if (!initialHost[0] || initialPort == 0) {
                SetError("Direct mode selected but target endpoint is invalid");
                return false;
            }
            if (directLooksIPv6) {
                SetError("Direct IPv6 endpoints are unsupported by the current ENet IPv4 transport");
                return false;
            }
            break;

        case ConnectPreference::AutoDirectThenRelay:
        default:
            if (!initialHost[0] || initialPort == 0) {
                if (relayConfigured) {
                    initialHost = config->nat.relay_host;
                    initialPort = config->nat.relay_port;
                    initialViaRelay = true;
                } else {
                    SetError("No valid direct endpoint configured");
                    return false;
                }
            } else if (directLooksIPv6 && relayConfigured) {
                initialHost = config->nat.relay_host;
                initialPort = config->nat.relay_port;
                initialViaRelay = true;
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Auto join selected relay first because target endpoint is IPv6 literal");
            }
            break;
    }

    if (strchr(initialHost, ':')) {
        if (initialViaRelay) {
            SetError("Traffic relay endpoint is unsupported by the current ENet transport");
            return false;
        }
        if (relayConfigured && config->connect_preference == ConnectPreference::AutoDirectThenRelay) {
            initialHost = config->nat.relay_host;
            initialPort = config->nat.relay_port;
            initialViaRelay = true;
            s_joinFallbackAttempted = true;
            Rollback::NetplayLog_Write("SESSION", -1,
                "Switching to relay because direct endpoint is IPv6 literal");
        } else {
            SetError("Direct IPv6 endpoint unsupported by current ENet transport");
            return false;
        }
    }

    NextSessionToken();
    Transport2_ClearQueues(0);
    s_joinFallbackAttempted = initialViaRelay;
    if (!StartJoinAttempt(initialHost, initialPort, initialViaRelay, "initial")) {
        SetError("Failed to start network join thread command");
        return false;
    }

    Nat_StartServices(config->listen_port);

    SetState(SessionState::Connecting);
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session attached to network thread: role=Join token=%u",
        s_activeSessionToken);
    return true;
}

void Session_Cancel() {
    if (s_state == SessionState::Idle) return;

    LOG_INFO("[Session] Canceling session (was %s)", SessionStateName(s_state));
    Rollback::NetplayLog_Write("SESSION", -1,
        "Cancel requested from state=%s", SessionStateName(s_state));
    Session2_Terminate(Session2TerminalReason::UserCancel, "Session canceled");
}

uint32_t Session_GetMsSinceLastInbound() {
    Transport2Stats netStats{};
    Transport2_GetStats(&netStats);

    // Protocol-level silence (INV-14): ENet commands (acks of our own pings
    // included) and authenticated autopunch keepalives all count, so an
    // idle-but-healthy link reads ~0 even with zero app traffic. (Field bug
    // 2026-08-17: app-level-only silence killed a healthy session parked on
    // the config screen — one side heartbeated, the other only received.)
    if (netStats.protocol_silence_ms != 0xFFFFFFFFu) {
        return netStats.protocol_silence_ms;
    }

    if (netStats.last_inbound_packet_tick_ms == 0) {
        return 0xFFFFFFFFu;  // never received anything
    }
    const DWORD now = GetTickCount();
    return (now >= netStats.last_inbound_packet_tick_ms)
               ? (uint32_t)(now - netStats.last_inbound_packet_tick_ms)
               : 0;
}

void Session_NotifyGameExit() {
    // Called from the WM_CLOSE fast-exit path. Without this the peer gets no
    // disconnect at all and only finds out via silence timeouts, which is
    // indistinguishable from a crash or link death on their side.
    if (s_state == SessionState::Idle) return;
    LOG_INFO("[Session] Sending goodbye on game exit (state=%s)", SessionStateName(s_state));
    Session2_Terminate(Session2TerminalReason::GameExit, "Peer closed the game");
}

void Session_SignalReady() {
    if (s_state != SessionState::Connected) {
        LOG_WARN("[Session] Cannot signal ready: not in Connected state");
        return;
    }

    s_localReady = true;
    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::Ready,
                          nullptr, 0, true, "ready")) {
        LOG_WARN("[Session] Failed to send Ready packet");
        Rollback::NetplayLog_Write("SESSION", -1,
            "ERROR: failed to send Ready packet");
        return;
    }
    LOG_INFO("[Session] Signaled Ready (local)");
    Rollback::NetplayLog_Write("SESSION", -1,
        "Local Ready sent: remote_ready=%d", s_remoteReady ? 1 : 0);

    if (s_localReady && s_remoteReady) {
        SetState(SessionState::Ready);
    }
}

void Session_Update() {
    const DWORD now = GetTickCount();
    if (s_lastSessionUpdateTick != 0) {
        const DWORD gapMs = now - s_lastSessionUpdateTick;
        if (gapMs >= 100) {
            Transport2Stats netStats{};
            Transport2_GetStats(&netStats);
            const DWORD workerAgeMs =
                (netStats.last_service_tick_ms > 0 && now >= netStats.last_service_tick_ms)
                    ? (now - netStats.last_service_tick_ms)
                    : 0;

            Rollback::NetplayLog_Write("NTHREAD", -1,
                "Game-thread Session_Update gap=%lums worker_running=%d worker_age=%lums "
                "inbound_depth=%u outbound_depth=%u state=%s role=%s token=%u",
                (unsigned long)gapMs,
                netStats.worker_running ? 1 : 0,
                (unsigned long)workerAgeMs,
                netStats.inbound_queue_depth,
                netStats.outbound_queue_depth,
                SessionStateName(s_state),
                SessionRoleName(s_role),
                s_activeSessionToken);
        }
    }
    s_lastSessionUpdateTick = now;

    // Game thread owns packet interpretation and callback dispatch; the
    // worker only enqueues transport events. Drain-before-judge (§2.2): this
    // runs before the supervisor evaluates in the same frame (ModOnFrame
    // ordering unchanged).
    DrainTransportEvents();

    if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
        CheckTimeouts();
    }

    PumpHandshakeResends();
    CheckPacingClockTerminal();

    if (s_state == SessionState::Connected || s_state == SessionState::Ready) {
        SendPeerIdentity();
    }
    if (s_state == SessionState::Connected) {
        SendNatInfo();
    }
    if (s_state == SessionState::Handshaking ||
        s_state == SessionState::Connected ||
        s_state == SessionState::Ready) {
        FlushNatTraversalOutboundSignals();
    }

    UpdateStats();
    UpdateStatusText();
}

bool Session_SendPacket(uint8_t channel, PacketType type,
                        const void* payload, size_t payloadLen, bool reliable) {
    if (s_activeSessionToken == 0 ||
        s_peerToken == 0 ||
        (s_state != SessionState::Connected && s_state != SessionState::Ready)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "DROP send: ch=%u type=%s payload=%zu reliable=%d state=%s role=%s peer=0x%llX token=%u",
            channel,
            PacketTypeName(type),
            payloadLen,
            reliable ? 1 : 0,
            SessionStateName(s_state),
            SessionRoleName(s_role),
            (unsigned long long)s_peerToken,
            s_activeSessionToken);
        return false;
    }

    return QueueTypedPacket(channel, type, payload, payloadLen, reliable, "session-send");
}

void Session_SetPacketCallback(PacketCallback cb) {
    // packet_router is the permanent default sink (§2.3). A non-null callback
    // overrides it (test harnesses); null restores the router.
    PacketCallback effective = cb ? cb : PacketRouter_OnPacket;
    Rollback::NetplayLog_Write("SESSION", -1,
        "Packet callback change: old=0x%llX new=0x%llX (router=0x%llX) state=%s role=%s",
        (unsigned long long)(uintptr_t)s_packetCallback,
        (unsigned long long)(uintptr_t)effective,
        (unsigned long long)(uintptr_t)&PacketRouter_OnPacket,
        SessionStateName(s_state),
        SessionRoleName(s_role));
    Rollback::NetplayLog_Flush();
    s_packetCallback = effective;
    FlushDeferredControlPackets();
}

void Session_GetSnapshot(SessionSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = (s_state != SessionState::Idle);
    out->state  = s_state;
    out->role   = s_role;
    out->local_listen_port = s_config.listen_port;
    memcpy(out->local_nickname, s_config.nickname, sizeof(out->local_nickname));
    out->remote_peer = s_remotePeer;
    out->stats  = out->active ? GetBestStatsSnapshot() : s_stats;
    out->local_ready  = s_localReady;
    out->remote_ready = s_remoteReady;
    memcpy(out->status_text, s_statusText, sizeof(out->status_text));
    memcpy(out->error_text, s_errorText, sizeof(out->error_text));
}

SessionState Session_GetState() {
    return s_state;
}

SessionRole Session_GetRole() {
    return s_role;
}

bool Session_IsConnected() {
    return s_state == SessionState::Connected ||
           s_state == SessionState::Ready;
}

const PeerInfo* Session_GetRemotePeer() {
    return s_remotePeer.valid ? &s_remotePeer : nullptr;
}

void Session_GetStats(ConnectionStats* out) {
    if (out) {
        const ConnectionStats stats = GetBestStatsSnapshot();
        memcpy(out, &stats, sizeof(*out));
    }
}

} // namespace Net
