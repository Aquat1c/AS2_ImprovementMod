/**
 * Alice Senki 2 - Session Manager (Implementation)
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>

#include "net/session_manager.h"
#include "net/network_thread.h"
#include "net/nat_traversal.h"
#include "patches/memory_utils.h"
#include "log_window.h"
#include "rollback/netplay_log.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
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
static uintptr_t      s_peerToken = 0;
static char           s_statusText[128] = "";
static char           s_errorText[128]  = "";
static PacketCallback s_packetCallback  = nullptr;
static bool           s_localReady      = false;
static bool           s_remoteReady     = false;
static bool           s_localNatInfoSent = false;
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

constexpr int MAX_DEFERRED_CONTROL_PACKETS = 64;

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
        "session manager transition"
    );
    s_state = newState;
    s_stateEnteredAt = GetTickCount();
}

static void SetError(const char* msg) {
    snprintf(s_errorText, sizeof(s_errorText), "%s", msg);
    LOG_ERROR("[Session] Error: %s", msg);
    Rollback::NetplayLog_Write("SESSION", -1, "ERROR: %s", msg);
    Nat_StopServices();
    SetState(SessionState::Failed);
}

static bool IsCompatibilityDisconnectData(uint32_t data) {
    return data == static_cast<uint32_t>(DisconnectReason::VersionMismatch);
}

static void RequestCompatibilityDisconnect(const char* packetName,
                                          const char* remoteNickname,
                                          uint16_t remoteProtocolVersion,
                                          uint32_t remoteBuildHash,
                                          const char* reason) {
    if (s_activeSessionToken == 0) {
        return;
    }

    Rollback::NetplayLog_Write("SESSION", -1,
        "Requesting compatibility disconnect: packet=%s nick=%s remote_ver=%u remote_hash=0x%08X data=%u reason=%s token=%u",
        packetName ? packetName : "?",
        remoteNickname && remoteNickname[0] ? remoteNickname : "(unknown)",
        remoteProtocolVersion,
        remoteBuildHash,
        static_cast<uint32_t>(DisconnectReason::VersionMismatch),
        reason ? reason : "?",
        s_activeSessionToken);
    NetworkThread_RequestDisconnect(
        s_activeSessionToken,
        static_cast<uint32_t>(DisconnectReason::VersionMismatch),
        false);
}

static void ResetState() {
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
    s_stateEnteredAt = 0;
    s_joinFallbackAttempted = false;
    s_joinUsingRelay = false;
    s_activeJoinHost[0] = '\0';
    s_activeJoinPort = 0;
    s_deferredControlCount = 0;
    s_lastInboundDropCount = 0;
    s_lastOutboundDropCount = 0;
    s_lastQueueSpikeLogAt = 0;
    s_lastInboundSilenceLogAt = 0;
    s_lastDrainLagLogAt = 0;
    s_lastSessionUpdateTick = 0;
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

static void CopyHandshakeNickname(const char* source,
                                  size_t sourceLen,
                                  char* destination,
                                  size_t destinationCap) {
    if (!destination || destinationCap == 0) {
        return;
    }

    memset(destination, 0, destinationCap);
    if (!source || sourceLen == 0) {
        return;
    }

    size_t copyLen = sourceLen;
    if (copyLen >= destinationCap) {
        copyLen = destinationCap - 1;
    }
    memcpy(destination, source, copyLen);
}

static bool ProcessHandshakeIdentity(const char* packetName,
                                     uint16_t protocolVersion,
                                     uint32_t buildHash,
                                     const char* nickname,
                                     size_t nicknameLen,
                                     uint16_t listenPort) {
    char remoteNickname[sizeof(s_remotePeer.nickname) + 1] = {};
    CopyHandshakeNickname(nickname, nicknameLen, remoteNickname, sizeof(remoteNickname));

    LOG_INFO("[Session] Received %s (nick=%s, ver=%u, hash=0x%08X, port=%u)",
             packetName, remoteNickname, protocolVersion, buildHash, listenPort);
    Rollback::NetplayLog_Write("SESSION", -1,
        "Received %s: nick=%s ver=%u hash=0x%08X listen_port=%u local_ver=%u local_hash=0x%08X state=%s role=%s",
        packetName,
        remoteNickname,
        protocolVersion,
        buildHash,
        listenPort,
        PROTOCOL_VERSION,
        s_config.build_hash,
        SessionStateName(s_state),
        SessionRoleName(s_role));

    if (protocolVersion != PROTOCOL_VERSION) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Rejecting %s from nick=%s: protocol mismatch local=%u remote=%u local_hash=0x%08X remote_hash=0x%08X",
            packetName,
            remoteNickname,
            PROTOCOL_VERSION,
            protocolVersion,
            s_config.build_hash,
            buildHash);
        RequestCompatibilityDisconnect(
            packetName,
            remoteNickname,
            protocolVersion,
            buildHash,
            "protocol mismatch");
        char msg[128];
        snprintf(msg, sizeof(msg), "Protocol version mismatch: local=%u remote=%u",
                 PROTOCOL_VERSION, protocolVersion);
        SetError(msg);
        return false;
    }

    if (buildHash != s_config.build_hash) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Rejecting %s from nick=%s: build mismatch local=0x%08X remote=0x%08X local_ver=%u remote_ver=%u",
            packetName,
            remoteNickname,
            s_config.build_hash,
            buildHash,
            PROTOCOL_VERSION,
            protocolVersion);
        RequestCompatibilityDisconnect(
            packetName,
            remoteNickname,
            protocolVersion,
            buildHash,
            "build mismatch");
        char msg[128];
        snprintf(msg, sizeof(msg), "Build hash mismatch: local=0x%08X remote=0x%08X",
                 s_config.build_hash, buildHash);
        SetError(msg);
        return false;
    }

    s_remotePeer.valid = true;
    s_remotePeer.protocol_version = protocolVersion;
    s_remotePeer.build_hash = buildHash;
    s_remotePeer.listen_port = listenPort;
    memset(s_remotePeer.nickname, 0, sizeof(s_remotePeer.nickname));
    strncpy_s(s_remotePeer.nickname, sizeof(s_remotePeer.nickname), remoteNickname, _TRUNCATE);

    LOG_INFO("[Session] %s accepted (nick=%s, ver=%u, hash=0x%08X)",
             packetName,
             s_remotePeer.nickname,
             s_remotePeer.protocol_version,
             s_remotePeer.build_hash);
    Rollback::NetplayLog_Write("SESSION", -1,
        "Accepted %s: nick=%s ver=%u hash=0x%08X listen_port=%u",
        packetName,
        s_remotePeer.nickname,
        s_remotePeer.protocol_version,
        s_remotePeer.build_hash,
        s_remotePeer.listen_port);
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
    if (s_deferredControlCount >= MAX_DEFERRED_CONTROL_PACKETS) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Deferred control packet queue full: type=%s ch=%u payload=%zu state=%s role=%s",
            PacketTypeName(type),
            channelID,
            payloadLen,
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
    for (int i = 0; i < queued; i++) {
        const DeferredControlPacket* packet = &s_deferredControlPackets[i];
        Rollback::NetplayLog_Write("SESSION", -1,
            "Dispatching deferred packet to callback: cb=0x%llX type=%s ch=%u payload=%zu state=%s role=%s",
            (unsigned long long)(uintptr_t)s_packetCallback,
            PacketTypeName(packet->type),
            packet->channel_id,
            packet->payload_len,
            SessionStateName(s_state),
            SessionRoleName(s_role));
        Rollback::NetplayLog_Flush();
        s_packetCallback(packet->type, packet->payload, packet->payload_len);
        Rollback::NetplayLog_Write("SESSION", -1,
            "Deferred packet callback returned: cb=0x%llX type=%s ch=%u",
            (unsigned long long)(uintptr_t)s_packetCallback,
            PacketTypeName(packet->type),
            packet->channel_id);
        Rollback::NetplayLog_Flush();
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
            snprintf(s_statusText, sizeof(s_statusText), "Connected to %s (RTT: %.0fms)",
                     s_remotePeer.nickname, s_stats.rtt_ms);
            break;
        case SessionState::Ready:
            snprintf(s_statusText, sizeof(s_statusText), "Ready (both peers)");
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
    s_stats.bytes_sent += sizeof(PacketType) + payloadLen;
    Rollback::NetplayLog_Verbose("SESSION", -1,
        "SEND %s ch=%u type=%s payload=%zu total=%zu reliable=%d state=%s",
        context ? context : "packet",
        channel,
        PacketTypeName(type),
        payloadLen,
        sizeof(PacketType) + payloadLen,
        reliable ? 1 : 0,
        SessionStateName(s_state));
}

static void NotePacketReceived(uint8_t channel, PacketType type, size_t payloadLen) {
    s_stats.packets_received++;
    s_stats.bytes_received += sizeof(PacketType) + payloadLen;
    Rollback::NetplayLog_Verbose("SESSION", -1,
        "RECV ch=%u type=%s payload=%zu total=%zu state=%s",
        channel,
        PacketTypeName(type),
        payloadLen,
        sizeof(PacketType) + payloadLen,
        SessionStateName(s_state));
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

    const bool queued = NetworkThread_SendPacket(
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

    Rollback::NetplayLog_Write("SESSION", -1,
        "Join attempt begin: token=%u host=%s port=%u via=%s reason=%s hole_punch_req=%d hole_punch_use=%d",
        s_activeSessionToken,
        s_activeJoinHost,
        s_activeJoinPort,
        usingRelay ? "relay" : "direct",
        reason ? reason : "?",
        wantsHolePunch ? 1 : 0,
        useHolePunch ? 1 : 0);

    if (wantsHolePunch && !canHolePunch) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Hole-punch requested but no OSS backend is linked; continuing with direct connect + relay fallback");
    }

    return NetworkThread_StartJoin(s_activeSessionToken,
                                   s_config.listen_port,
                                   s_activeJoinHost,
                                   s_activeJoinPort,
                                   useHolePunch);
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
    if (!HasRelayConfigured(&s_config)) {
        return false;
    }

    s_joinFallbackAttempted = true;

    Rollback::NetplayLog_Write("SESSION", -1,
        "Direct join failed -> relay fallback: relay=%s:%u reason=%s",
        s_config.nat.relay_host,
        s_config.nat.relay_port,
        reason ? reason : "?");

    NetworkThread_RequestDestroyHost(s_activeSessionToken);
    NetworkThread_ClearQueues(s_activeSessionToken);

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
    if (HasRelayConfigured(&s_config)) payload.flags |= NAT_INFO_FLAG_RELAY_FALLBACK;
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
// Handshake
// ============================================================================

static void SendHello() {
    HelloPayload hello;
    hello.protocol_version = PROTOCOL_VERSION;
    hello.build_hash = s_config.build_hash;
    hello.listen_port = s_config.listen_port;
    memset(hello.nickname, 0, sizeof(hello.nickname));
    strncpy(hello.nickname, s_config.nickname, sizeof(hello.nickname) - 1);

    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::Hello,
                          &hello, sizeof(hello), true, "hello")) {
        SetError("Failed to send Hello");
        return;
    }
    LOG_INFO("[Session] Sent Hello (nick=%s, ver=%u, hash=0x%08X)",
             hello.nickname, hello.protocol_version, hello.build_hash);
    Rollback::NetplayLog_Write("SESSION", -1,
        "Sent Hello: nick=%s ver=%u hash=0x%08X listen_port=%u",
        hello.nickname, hello.protocol_version, hello.build_hash, hello.listen_port);
}

static void SendHelloAck() {
    HelloAckPayload ack;
    ack.protocol_version = PROTOCOL_VERSION;
    ack.build_hash = s_config.build_hash;
    ack.listen_port = s_config.listen_port;
    memset(ack.nickname, 0, sizeof(ack.nickname));
    strncpy(ack.nickname, s_config.nickname, sizeof(ack.nickname) - 1);

    if (!QueueTypedPacket(CHANNEL_CONTROL, PacketType::HelloAck,
                          &ack, sizeof(ack), true, "hello-ack")) {
        SetError("Failed to send HelloAck");
        return;
    }
    LOG_INFO("[Session] Sent HelloAck");
    Rollback::NetplayLog_Write("SESSION", -1,
        "Sent HelloAck: nick=%s ver=%u hash=0x%08X listen_port=%u",
        ack.nickname, ack.protocol_version, ack.build_hash, ack.listen_port);
}

static bool ProcessHelloPayload(const void* payload, size_t len) {
    if (len < sizeof(HelloPayload)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "Hello payload too small: got=%zu expected=%zu state=%s role=%s",
            len, sizeof(HelloPayload), SessionStateName(s_state), SessionRoleName(s_role));
        RequestCompatibilityDisconnect("Hello", "(unknown)", 0, 0, "hello payload too small");
        SetError("Hello payload too small");
        return false;
    }

    const HelloPayload* hello = static_cast<const HelloPayload*>(payload);
    return ProcessHandshakeIdentity(
        "Hello",
        hello->protocol_version,
        hello->build_hash,
        hello->nickname,
        sizeof(hello->nickname),
        hello->listen_port);
}

static bool ProcessHelloAckPayload(const void* payload, size_t len) {
    if (len < sizeof(HelloAckPayload)) {
        Rollback::NetplayLog_Write("SESSION", -1,
            "HelloAck payload too small: got=%zu expected=%zu state=%s role=%s",
            len, sizeof(HelloAckPayload), SessionStateName(s_state), SessionRoleName(s_role));
        RequestCompatibilityDisconnect("HelloAck", "(unknown)", 0, 0, "hello-ack payload too small");
        SetError("HelloAck payload too small");
        return false;
    }

    const HelloAckPayload* ack = static_cast<const HelloAckPayload*>(payload);
    return ProcessHandshakeIdentity(
        "HelloAck",
        ack->protocol_version,
        ack->build_hash,
        ack->nickname,
        sizeof(ack->nickname),
        ack->listen_port);
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
        // Joiner sends Hello first; Host waits
        if (s_role == SessionRole::Join) {
            SendHello();
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
        case PacketType::Hello:
            if (s_state == SessionState::Handshaking || s_state == SessionState::Connecting) {
                if (s_state == SessionState::Connecting) {
                    // Host received Hello before we noticed the connect event
                    s_peerToken = peerToken;
                    SetState(SessionState::Handshaking);
                }
                if (ProcessHelloPayload(payload, payloadLen)) {
                    SendHelloAck();
                    if (s_role == SessionRole::Host) {
                        SetState(SessionState::Connected);
                        SendNatInfo();
                    }
                }
            }
            break;

        case PacketType::HelloAck:
            if (s_state == SessionState::Handshaking) {
                if (ProcessHelloAckPayload(payload, payloadLen)) {
                    SetState(SessionState::Connected);
                    SendNatInfo();
                }
            }
            break;

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
                NetworkThread_RequestDisconnect(s_activeSessionToken, 0, true);
                NetworkThread_RequestDestroyHost(s_activeSessionToken);
                NetworkThread_ClearQueues(s_activeSessionToken);
            }
            s_peerToken = 0;
            Nat_StopServices();
            ResetState();
            break;
        }

        default:
            // Forward to external callback
            if (s_packetCallback) {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Dispatching packet to callback: cb=0x%llX type=%s ch=%u payload=%zu state=%s role=%s peer=0x%llX",
                    (unsigned long long)(uintptr_t)s_packetCallback,
                    PacketTypeName(type),
                    channelID,
                    payloadLen,
                    SessionStateName(s_state),
                    SessionRoleName(s_role),
                    (unsigned long long)peerToken);
                Rollback::NetplayLog_Flush();
                s_packetCallback(type, payload, payloadLen);
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Callback returned: cb=0x%llX type=%s ch=%u",
                    (unsigned long long)(uintptr_t)s_packetCallback,
                    PacketTypeName(type),
                    channelID);
                Rollback::NetplayLog_Flush();
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
        case SessionState::Handshaking:
            if (elapsed > s_config.handshake_timeout_ms) {
                Rollback::NetplayLog_Write("SESSION", -1,
                    "Handshake timeout after %lu ms", (unsigned long)elapsed);
                SetError("Handshake timed out");
            }
            break;
        default:
            break;
    }
}

// ============================================================================
// Update peer stats
// ============================================================================

static void UpdateStats() {
    NetworkThreadStats netStats{};
    NetworkThread_GetStats(&netStats);

    s_stats.rtt_ms = netStats.rtt_ms;
    s_stats.rtt_variance_ms = netStats.rtt_variance_ms;
    s_stats.packets_sent = netStats.packets_sent;
    s_stats.packets_lost = netStats.packets_lost;

    const DWORD now = GetTickCount();
    const DWORD workerAgeMs =
        (netStats.last_service_tick_ms > 0 && now >= netStats.last_service_tick_ms)
            ? (now - netStats.last_service_tick_ms)
            : 0;

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
                "Inbound silence while outbound active: silence=%lums outbound_age=%lums worker_age=%lums "
                "inbound_depth=%u outbound_depth=%u state=%s role=%s token=%u",
                (unsigned long)inboundSilenceMs,
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

static void DrainNetworkEvents() {
    NetworkThreadEvent ev{};
    int drained = 0;
    int staleDropped = 0;

    while (NetworkThread_TryPopEvent(&ev)) {
        drained++;

        if ((ev.session_token == 0 && s_activeSessionToken != 0) ||
            (ev.session_token != 0 && ev.session_token != s_activeSessionToken)) {
            staleDropped++;
            continue;
        }

        switch (ev.type) {
            case NetworkThreadEventType::Connected:
                OnTransportConnected(ev.peer_token);
                break;

            case NetworkThreadEventType::Disconnected:
                OnTransportDisconnect(ev.peer_token, ev.disconnect_data, ev.transport_tick_ms);
                break;

            case NetworkThreadEventType::PacketReceived:
                OnPacketReceived(ev.peer_token,
                                 ev.channel_id,
                                 ev.packet_data,
                                 ev.packet_len,
                                 ev.transport_tick_ms);
                break;

            case NetworkThreadEventType::WorkerError:
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

// ============================================================================
// Public API
// ============================================================================

void Session_Init() {
    ResetState();
    s_activeSessionToken = 0;
    if (!NetworkThread_Init()) {
        LOG_ERROR("[Session] Failed to initialize network service thread");
        Rollback::NetplayLog_Write("NTHREAD", -1, "ERROR: network service thread init failed");
    }
    LOG_INFO("[Session] Session manager initialized");
}

void Session_Shutdown() {
    if (s_state != SessionState::Idle) {
        Session_Cancel();
    }
    NetworkThread_Shutdown();
    Nat_StopServices();
    s_activeSessionToken = 0;
    ResetState();
    LOG_INFO("[Session] Session manager shut down");
}

bool Session_StartHost(const SessionConfig* config) {
    if (!config) return false;
    if (s_state != SessionState::Idle) {
        LOG_WARN("[Session] Cannot start host: session already active (state=%s)",
                 SessionStateName(s_state));
        return false;
    }

    memcpy(&s_config, config, sizeof(s_config));
    if (!ComputeLocalBuildHash(&s_config.build_hash)) {
        SetError("Failed to compute exact local build fingerprint");
        return false;
    }
    s_role = SessionRole::Host;

    Rollback::NetplayLog_Write("SESSION", -1,
        "StartHost: listen_port=%u nick=%s hash=0x%08X connect_timeout=%u handshake_timeout=%u "
        "upnp=%d pcp=%d stun=%d hole_punch=%d turn=%d ipv6=%d pref_direct=%d "
        "backend_upnp=%d backend_pcp=%d backend_stun=%d backend_hole=%d backend_turn=%d",
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
        Nat_IsTurnBackendAvailable() ? 1 : 0);

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

    if (!NetworkThread_Init()) {
        SetError("Failed to initialize network worker");
        return false;
    }

    NextSessionToken();
    NetworkThread_ClearQueues(0);
    Nat_ClearRemoteHint();
    if (!NetworkThread_StartHost(s_activeSessionToken, config->listen_port)) {
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

    memcpy(&s_config, config, sizeof(s_config));
    if (!ComputeLocalBuildHash(&s_config.build_hash)) {
        SetError("Failed to compute exact local build fingerprint");
        return false;
    }
    s_role = SessionRole::Join;

    const bool relayConfigured = HasRelayConfigured(&s_config);

    Rollback::NetplayLog_Write("SESSION", -1,
        "StartJoin: target=%s:%u relay=%s:%u mode=%s nick=%s hash=0x%08X "
        "connect_timeout=%u handshake_timeout=%u upnp=%d pcp=%d stun=%d hole_punch=%d turn=%d "
        "ipv6=%d pref_direct=%d backend_upnp=%d backend_pcp=%d backend_stun=%d backend_hole=%d backend_turn=%d",
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
        Nat_IsTurnBackendAvailable() ? 1 : 0);

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

    if (!NetworkThread_Init()) {
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
                SetError("Relay mode selected but relay endpoint is missing");
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
                SetError("Direct IPv6 endpoints require relay fallback with current ENet transport");
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
                    SetError("No valid direct endpoint or relay fallback configured");
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
            SetError("Relay endpoint is IPv6 literal; current ENet transport requires IPv4/DNS relay");
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
    NetworkThread_ClearQueues(0);
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

    const uint32_t cancelToken = s_activeSessionToken;

    // Send a disconnect packet if we have a peer
    if (cancelToken != 0 &&
        (s_state == SessionState::Connected ||
         s_state == SessionState::Ready ||
         s_state == SessionState::Handshaking)) {
        DisconnectPayload dp;
        dp.reason_code = static_cast<uint16_t>(DisconnectReason::UserCancel);
        strncpy(dp.message, "Session canceled", sizeof(dp.message) - 1);
        dp.message[sizeof(dp.message) - 1] = '\0';
        QueueTypedPacket(CHANNEL_CONTROL, PacketType::Disconnect, &dp, sizeof(dp), true, "cancel");
        NetworkThread_RequestDisconnect(cancelToken, 0, false);
    }

    if (cancelToken != 0) {
        NetworkThread_RequestDestroyHost(cancelToken);
        NetworkThread_ClearQueues(cancelToken);
    } else {
        NetworkThread_RequestDestroyHost(0);
        NetworkThread_ClearQueues(0);
    }
    Rollback::NetplayLog_Write("NTHREAD", -1,
        "Session detached from network thread: token=%u",
        cancelToken);

    s_activeSessionToken = 0;
    Nat_StopServices();
    ResetState();
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
            NetworkThreadStats netStats{};
            NetworkThread_GetStats(&netStats);
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

    // Game thread owns packet interpretation and callback dispatch. Network
    // thread only enqueues transport events.
    DrainNetworkEvents();

    if (s_state != SessionState::Idle && s_state != SessionState::Failed) {
        CheckTimeouts();
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
    Rollback::NetplayLog_Write("SESSION", -1,
        "Packet callback change: old=0x%llX new=0x%llX state=%s role=%s",
        (unsigned long long)(uintptr_t)s_packetCallback,
        (unsigned long long)(uintptr_t)cb,
        SessionStateName(s_state),
        SessionRoleName(s_role));
    Rollback::NetplayLog_Flush();
    s_packetCallback = cb;
    FlushDeferredControlPackets();
}

void Session_GetSnapshot(SessionSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active = (s_state != SessionState::Idle);
    out->state  = s_state;
    out->role   = s_role;
    memcpy(out->local_nickname, s_config.nickname, sizeof(out->local_nickname));
    out->remote_peer = s_remotePeer;
    out->stats  = s_stats;
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
        memcpy(out, &s_stats, sizeof(*out));
    }
}

} // namespace Net
