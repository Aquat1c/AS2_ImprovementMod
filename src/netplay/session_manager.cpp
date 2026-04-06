/**
 * Alice Senki 2 - Session Manager Implementation (ENet transport)
 *
 * Replaces the hand-rolled UDP transport (UdpSocket + AckTracker + ReliableChannel)
 * with ENet for proven reliable/unreliable delivery, built-in RTT, and automatic
 * connection management.
 *
 * Channel layout:
 *   Channel 0 — Reliable (control, bootstrap, charsel config, digest, adaptive delay)
 *   Channel 1 — Unreliable (GekkoData, CharSelInput)
 */

// Include ENet first to ensure winsock2.h comes before windows.h
#include <enet/enet.h>

#include "session_manager.h"
#include "rollback_session.h"
#include "packet_codec.h"
#include "state_digest.h"
#include "netplay_hooks.h"
#include "charsel_sync.h"
#include "match_bootstrap.h"
#include "adaptive_delay.h"
#include "desync_diagnostics.h"
#include "spectator_manager.h"
#include "input_sync_hooks.h"
#include "upnp_manager.h"
#include "log_window.h"
#include "as2_rollback.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

namespace SessionManager {

// ============================================================================
// Constants
// ============================================================================

static constexpr int kConnectTimeoutFrames   = 7200; // 120 seconds
static constexpr int kHandshakeTimeoutFrames = 300;  // 5 seconds
static constexpr int kDigestIntervalFrames   = 600;  // 10 seconds
static constexpr int kMaxGekkoBuffer         = 64;
static constexpr int kENetChannels           = 2;
static constexpr int kENetMaxPeers           = 2;    // opponent + potential spectator

// Reliable ready retransmit (Ready packet for CharSel start)
static constexpr uint32_t kReadyRetransmitFrames = 60; // ~1 second

// ENet channel assignments
static constexpr uint8_t CH_RELIABLE   = 0;
static constexpr uint8_t CH_UNRELIABLE = 1;

// ============================================================================
// Application Handshake Payload (exchanged after ENet connection)
// ============================================================================

#pragma pack(push, 1)
struct AppHandshake {
    uint32_t build_hash;
    uint64_t nonce;
    uint16_t nickname_len;
    char     nickname[24];
};
#pragma pack(pop)

// ============================================================================
// Internal state
// ============================================================================

namespace {

static Mode     s_mode = Mode::None;
static State    s_state = State::Idle;
static uint32_t s_revision = 0;
static char     s_peerNickname[NetplayConfig::kNicknameCap] = "";
static char     s_endpoint[48] = "";
static char     s_status[128] = "Session idle.";
static char     s_lastError[128] = "";

// ENet
static bool       s_enetInitialized = false;
static ENetHost*  s_enetHost = nullptr;
static ENetPeer*  s_peer = nullptr;

// Config snapshot
static NetplayConfig::Config s_config = {};

// Connection identity
static uint64_t s_sessionId     = 0;
static uint32_t s_connectionId  = 0;
static uint64_t s_localNonce    = 0;
static uint64_t s_remoteNonce   = 0;

// Timing
static int      s_stateTimer    = 0;
static int      s_digestTimer   = 0;

// RTT (from ENet, smoothed locally for delay decisions)
static float    s_rttMs = 0.0f;
static int      s_rttSamples = 0;
static constexpr int kDelayRttWindow = 8;
static float    s_delayRttSamples[kDelayRttWindow] = {};
static int      s_delayRttSampleCount = 0;
static int      s_delayRttSampleIndex = 0;
static float    s_delayRttMs = 0.0f;

// Stats
static uint32_t s_packetsSent     = 0;
static uint32_t s_packetsReceived = 0;

// Gekko buffer
static BufferedPacket s_gekkoBuffer[kMaxGekkoBuffer];
static int      s_gekkoBufferHead  = 0;
static int      s_gekkoBufferTail  = 0;
static int      s_gekkoBufferCount = 0;
static uint32_t s_gekkoPacketsBuffered = 0;
static uint32_t s_gekkoPacketsDrained  = 0;

// CharSel start signal
static bool     s_pendingCharSelStart = false;
static uint32_t s_charSelReadyFlags   = 0;

// Host-side Ready ack tracking
static bool     s_readyAckedByPeer      = false;
static uint32_t s_readyRetransmitTimer  = 0;

// Handshake state: tracks whether we've sent/received the AppHandshake
static bool     s_sentHandshake     = false;
static bool     s_receivedHandshake = false;

// ============================================================================
// Helpers
// ============================================================================

static void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy_s(dst, cap, src, _TRUNCATE);
}

static uint64_t ComputeSharedSessionId(uint64_t a, uint64_t b) {
    uint64_t lo = (a < b) ? a : b;
    uint64_t hi = (a < b) ? b : a;
    uint64_t mixed = lo ^ (hi + 0x9E3779B97F4A7C15ull + (lo << 6) + (lo >> 2));
    mixed ^= (mixed >> 33);
    mixed *= 0xff51afd7ed558ccduLL;
    mixed ^= (mixed >> 33);
    return mixed ? mixed : 1ull;
}

static uint32_t ComputeSharedConnectionId(uint64_t sessionId, uint64_t a, uint64_t b) {
    uint64_t lo = (a < b) ? a : b;
    uint64_t hi = (a < b) ? b : a;
    uint64_t mixed = sessionId ^ (lo + 0x517cc1b727220a95ull) ^ (hi << 1) ^ (lo >> 1);
    mixed ^= (mixed >> 32);
    uint32_t out = (uint32_t)(mixed & 0xFFFFFFFFu);
    return out ? out : 1u;
}

static void FinalizeSessionIdentity() {
    if (s_localNonce == 0 || s_remoteNonce == 0) return;
    uint64_t sid = ComputeSharedSessionId(s_localNonce, s_remoteNonce);
    uint32_t cid = ComputeSharedConnectionId(sid, s_localNonce, s_remoteNonce);
    if (s_sessionId == sid && s_connectionId == cid) return;
    s_sessionId = sid;
    s_connectionId = cid;
    LOG_NET_INFO("[Session] Shared identity established (session=0x%llX connection=0x%08X)",
                 s_sessionId, s_connectionId);
}

static uint64_t GenerateNonce() {
    static uint32_t counter = 0;
    uint64_t t = PacketCodec::GetTimestampMs();
    return (t << 16) ^ (++counter);
}

static void UpdateDelayRttEstimate(float sampleMs) {
    if (sampleMs <= 0.0f) return;
    s_delayRttSamples[s_delayRttSampleIndex] = sampleMs;
    s_delayRttSampleIndex = (s_delayRttSampleIndex + 1) % kDelayRttWindow;
    if (s_delayRttSampleCount < kDelayRttWindow)
        s_delayRttSampleCount++;

    float sorted[kDelayRttWindow] = {};
    for (int i = 0; i < s_delayRttSampleCount; ++i)
        sorted[i] = s_delayRttSamples[i];
    for (int i = 0; i < s_delayRttSampleCount - 1; ++i) {
        int best = i;
        for (int j = i + 1; j < s_delayRttSampleCount; ++j)
            if (sorted[j] < sorted[best]) best = j;
        if (best != i) { float tmp = sorted[i]; sorted[i] = sorted[best]; sorted[best] = tmp; }
    }
    int idx = s_delayRttSampleCount / 4;
    if (idx >= s_delayRttSampleCount) idx = s_delayRttSampleCount - 1;
    s_delayRttMs = sorted[idx];
}

static float GetDelaySelectionRttMs() {
    return (s_delayRttMs > 0.0f) ? s_delayRttMs : s_rttMs;
}

static int ClampCharSelDelay(int delay) {
    if (delay < 2) delay = 2;
    if (delay > 10) delay = 10;
    return delay;
}

static uint32_t EncodeCharSelReadyFlags(int delay) {
    return (uint32_t)(ClampCharSelDelay(delay) & 0xFF);
}

static int DecodeCharSelReadyDelay(uint32_t readyFlags) {
    int d = (int)(readyFlags & 0xFFu);
    return (d <= 0) ? 0 : ClampCharSelDelay(d);
}

static int ChooseHostCharSelDelay() {
    int delay = CharSelSync::SuggestInputDelay(s_rttMs);
    if (s_config.preferred_delay_frames > delay)
        delay = s_config.preferred_delay_frames;
    return ClampCharSelDelay(delay);
}

static void SetState(State next, const char* fmt, ...) {
    char buffer[128];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    if (s_state != next || strcmp(s_status, buffer) != 0) {
        LOG_NET_INFO("[Session] %s -> %s | %s",
                    GetStateName(s_state), GetStateName(next), buffer);
        s_state = next;
        CopyText(s_status, sizeof(s_status), buffer);
        s_stateTimer = 0;
        ++s_revision;
    }
}

static void SetError(const char* fmt, ...) {
    char buffer[128];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    CopyText(s_lastError, sizeof(s_lastError), buffer);
    SetState(State::Error, "%s", buffer);
}

// ============================================================================
// ENet Transport
// ============================================================================

static void DestroyHost() {
    if (s_peer) {
        enet_peer_reset(s_peer);
        s_peer = nullptr;
    }
    if (s_enetHost) {
        enet_host_destroy(s_enetHost);
        s_enetHost = nullptr;
    }
}

static void FormatENetAddr(const ENetAddress* addr, char* buf, size_t cap) {
    if (!addr || !buf || cap < 1) return;
    uint32_t h = addr->host;
    _snprintf_s(buf, cap, _TRUNCATE, "%u.%u.%u.%u:%u",
                h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF, (h >> 24) & 0xFF,
                addr->port);
}

// ============================================================================
// Packet Sending
// ============================================================================

// Internal: send raw bytes to the connected peer
static bool SendRaw(const void* data, size_t size, bool reliable) {
    if (!s_peer) return false;
    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    uint8_t channel = reliable ? CH_RELIABLE : CH_UNRELIABLE;
    ENetPacket* pkt = enet_packet_create(data, size, flags);
    if (!pkt) return false;
    if (enet_peer_send(s_peer, channel, pkt) < 0) {
        enet_packet_destroy(pkt);
        return false;
    }
    s_packetsSent++;
    return true;
}

// Internal: send a typed packet with payload
static bool SendTypedPacket(PacketCodec::PacketType type, const void* payload, size_t payloadSize, bool reliable) {
    uint8_t buf[PacketCodec::MAX_PACKET_SIZE];
    if (1 + payloadSize > sizeof(buf)) return false;
    buf[0] = (uint8_t)type;
    if (payload && payloadSize > 0)
        memcpy(buf + 1, payload, payloadSize);
    return SendRaw(buf, 1 + payloadSize, reliable);
}

static void SendAppHandshake() {
    AppHandshake hs = {};
    hs.build_hash = NetplayHooks::GetBuildSignature();
    hs.nonce = s_localNonce;
    hs.nickname_len = (uint16_t)strnlen_s(s_config.nickname, sizeof(s_config.nickname));
    memset(hs.nickname, 0, sizeof(hs.nickname));
    strncpy_s(hs.nickname, sizeof(hs.nickname), s_config.nickname, _TRUNCATE);
    SendTypedPacket(PacketCodec::PacketType::Hello, &hs, sizeof(hs), true);
    s_sentHandshake = true;
}

static void SendStateDigest() {
    PacketCodec::StateDigestPayload digest;
    StateDigest::Capture(&digest);
    StateDigest::RecordLocal(&digest);
    SendTypedPacket(PacketCodec::PacketType::StateDigest, &digest, sizeof(digest), true);
}

// ============================================================================
// Receive Handlers
// ============================================================================

static void HandleAppHandshake(const AppHandshake* hs) {
    uint32_t localBuild = NetplayHooks::GetBuildSignature();
    if (hs->build_hash != localBuild) {
        LOG_NET_WARN("[Session] Build mismatch! local=0x%08X remote=0x%08X",
                    localBuild, hs->build_hash);
        SetError("Version mismatch (local=0x%08X remote=0x%08X). Same mod version required.",
                 localBuild, hs->build_hash);
        if (s_peer) enet_peer_disconnect(s_peer, 1);
        return;
    }

    s_remoteNonce = hs->nonce;
    FinalizeSessionIdentity();

    memset(s_peerNickname, 0, sizeof(s_peerNickname));
    int nickLen = hs->nickname_len;
    if (nickLen > 0) {
        if (nickLen >= (int)sizeof(s_peerNickname)) nickLen = (int)sizeof(s_peerNickname) - 1;
        memcpy(s_peerNickname, hs->nickname, nickLen);
        s_peerNickname[nickLen] = '\0';
    }

    s_receivedHandshake = true;
    LOG_NET_INFO("[Session] AppHandshake from '%s' (build=0x%08X nonce=0x%llX)",
                s_peerNickname, hs->build_hash, hs->nonce);

    // If we've also sent ours, handshake is complete
    if (s_sentHandshake) {
        SetState(State::Connected, "Connected to %s.",
                 s_peerNickname[0] ? s_peerNickname : "peer");
    }
}

static void HandleStateDigest(const PacketCodec::StateDigestPayload* remote) {
    StateDigest::CompareRemote(remote);
}

static void HandleReady(const PacketCodec::ReadyPayload* ready) {
    if (s_state != State::Connected && s_state != State::CharSel) {
        LOG_NET_WARN("[Session] Ignoring Ready in state %s", GetStateName(s_state));
        return;
    }
    const int charSelDelay = DecodeCharSelReadyDelay(ready->ready_flags);
    LOG_NET_INFO("[Session] Received Ready (flags=0x%08X charselDelay=%d)",
                 ready->ready_flags, charSelDelay);

    if (s_mode == Mode::Join) {
        s_charSelReadyFlags = ready->ready_flags;
        s_pendingCharSelStart = true;
        PacketCodec::ReadyPayload ack = {};
        ack.ready_flags = ready->ready_flags;
        SendTypedPacket(PacketCodec::PacketType::Ready, &ack, sizeof(ack), true);
        LOG_NET_INFO("[Session] Sent Ready ack to host (charselDelay=%d)", charSelDelay);
    } else {
        s_readyAckedByPeer = true;
        LOG_NET_INFO("[Session] Host received Ready ack (charselDelay=%d)", charSelDelay);
    }
}

// ============================================================================
// Packet Dispatch
// ============================================================================

static void ProcessReceivedPacket(const uint8_t* data, size_t len) {
    if (len < 1) return;

    PacketCodec::PacketType type = (PacketCodec::PacketType)data[0];
    const void* payload = (len > 1) ? (data + 1) : nullptr;
    int payloadLen = (int)(len - 1);

    s_packetsReceived++;

    switch (type) {
    case PacketCodec::PacketType::Hello:
        // Reused as AppHandshake in ENet flow
        if (payloadLen >= (int)sizeof(AppHandshake)) {
            HandleAppHandshake((const AppHandshake*)payload);
        }
        break;

    case PacketCodec::PacketType::StateDigest:
        if (payloadLen >= (int)sizeof(PacketCodec::StateDigestPayload)) {
            HandleStateDigest((const PacketCodec::StateDigestPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::Ready:
        if (payloadLen >= (int)sizeof(PacketCodec::ReadyPayload)) {
            HandleReady((const PacketCodec::ReadyPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::GekkoData:
        BufferGekkoPacket(payload, payloadLen);
        break;

    case PacketCodec::PacketType::CharSelInput:
        if (payloadLen >= (int)sizeof(PacketCodec::CharSelInputPayload)) {
            CharSelSync::OnReceiveInput((const PacketCodec::CharSelInputPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::MatchConfig:
        if (payloadLen >= (int)sizeof(PacketCodec::MatchConfigPayload)) {
            CharSelSync::OnReceiveMatchConfig((const PacketCodec::MatchConfigPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::MatchConfigAck:
        if (payloadLen >= (int)sizeof(PacketCodec::MatchConfigAckPayload)) {
            CharSelSync::OnReceiveMatchConfigAck((const PacketCodec::MatchConfigAckPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::LoadBarrierReady:
        if (payloadLen >= (int)sizeof(PacketCodec::LoadBarrierReadyPayload)) {
            MatchBootstrap::OnReceiveLoadBarrierReady((const PacketCodec::LoadBarrierReadyPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::BaselineReady:
        if (payloadLen >= (int)sizeof(PacketCodec::BaselineReadyPayload)) {
            MatchBootstrap::OnReceiveBaselineReady((const PacketCodec::BaselineReadyPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::StartGameplay:
        if (payloadLen >= (int)sizeof(PacketCodec::StartGameplayPayload)) {
            MatchBootstrap::OnReceiveStartGameplay((const PacketCodec::StartGameplayPayload*)payload,
                                                  PacketCodec::GetTimestampMs());
        }
        break;

    case PacketCodec::PacketType::StartGameplayAck:
        if (payloadLen >= (int)sizeof(PacketCodec::StartGameplayAckPayload)) {
            MatchBootstrap::OnReceiveStartGameplayAck((const PacketCodec::StartGameplayAckPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::Rematch:
        if (payloadLen >= (int)sizeof(PacketCodec::RematchPayload)) {
            const auto* rp = (const PacketCodec::RematchPayload*)payload;
            LOG_NET_INFO("[Session] Received Rematch (seq=%u decision=%u)", rp->rematch_seq, rp->decision);
        }
        break;

    case PacketCodec::PacketType::ErrorNotice:
        if (payloadLen >= (int)sizeof(PacketCodec::ErrorNoticePayload)) {
            const auto* ep = (const PacketCodec::ErrorNoticePayload*)payload;
            LOG_NET_ERROR("[Session] Received ErrorNotice (code=%u related_type=%u)",
                      ep->error_code, ep->related_packet_type);
        }
        break;

    case PacketCodec::PacketType::DelayChangeRequest:
        if (payloadLen >= (int)sizeof(PacketCodec::DelayChangeRequestPayload)) {
            AdaptiveDelay::OnReceiveDelayChangeRequest((const PacketCodec::DelayChangeRequestPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::DelayChangeAck:
        if (payloadLen >= (int)sizeof(PacketCodec::DelayChangeAckPayload)) {
            AdaptiveDelay::OnReceiveDelayChangeAck((const PacketCodec::DelayChangeAckPayload*)payload);
        }
        break;

    case PacketCodec::PacketType::SpectatorHello:
        // TODO: Spectator via ENet peer management
        LOG_NET_DEBUG("[Session] SpectatorHello received (ENet spectator support TBD)");
        break;

    default:
        LOG_NET_DEBUG("[Session] Unknown packet type %u", data[0]);
        break;
    }
}

// ============================================================================
// ENet Event Poll
// ============================================================================

static void PollENet() {
    if (!s_enetHost) return;

    ENetEvent event;
    // Non-blocking poll — drain all pending events
    while (enet_host_service(s_enetHost, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            char addrBuf[48];
            FormatENetAddr(&event.peer->address, addrBuf, sizeof(addrBuf));

            // Duplicate peer guard (hole-punching can trigger two CONNECT events)
            if (s_peer && s_peer != event.peer) {
                LOG_NET_INFO("[Session] Duplicate peer from %s — already connected, resetting extra", addrBuf);
                enet_peer_reset(event.peer);
                break;
            }

            s_peer = event.peer;
            CopyText(s_endpoint, sizeof(s_endpoint), addrBuf);
            LOG_NET_INFO("[Session] ENet peer connected from %s", addrBuf);

            // Configure peer timeout (limit, min, max in ms)
            enet_peer_timeout(s_peer, 32, 2000, 30000);

            // Send our application handshake
            SendAppHandshake();

            SetState(State::Handshake, "Peer connected, exchanging handshake...");
            break;
        }

        case ENET_EVENT_TYPE_RECEIVE: {
            if (event.packet && event.packet->data && event.packet->dataLength > 0) {
                ProcessReceivedPacket(event.packet->data, event.packet->dataLength);
            }
            enet_packet_destroy(event.packet);
            break;
        }

        case ENET_EVENT_TYPE_DISCONNECT: {
            LOG_NET_INFO("[Session] ENet peer disconnected (data=%u)", event.data);

            // If the hole-punch probe peer disconnects before handshake completes,
            // the host should keep waiting for incoming connections rather than error.
            if (s_mode == Mode::Host && !s_receivedHandshake &&
                s_state == State::Connecting) {
                LOG_NET_INFO("[Session] Hole-punch probe peer disconnected — still waiting for incoming");
                s_peer = nullptr;
                break;
            }

            s_peer = nullptr;
            if (s_state != State::Idle && s_state != State::Error) {
                SetError("Peer disconnected.");
            }
            break;
        }

        case ENET_EVENT_TYPE_NONE:
            break;
        }
    }

    // Update RTT from ENet's built-in measurement
    if (s_peer && s_peer->roundTripTime > 0) {
        float sample = (float)s_peer->roundTripTime;
        if (s_rttSamples == 0) {
            s_rttMs = sample;
        } else {
            s_rttMs = s_rttMs * 0.8f + sample * 0.2f;
        }
        s_rttSamples++;
        UpdateDelayRttEstimate(sample);
        AdaptiveDelay::UpdateRttFloor(sample);
    }
}

// ============================================================================
// State Update Functions
// ============================================================================

static void UpdateConnecting() {
    s_stateTimer++;

    // For joiner: timeout after 120 seconds. Host waits indefinitely.
    if (s_mode != Mode::Host && s_stateTimer >= kConnectTimeoutFrames) {
        SetError("Connection timed out.");
        return;
    }
}

static void UpdateHandshake() {
    s_stateTimer++;
    if (s_stateTimer >= kHandshakeTimeoutFrames) {
        SetError("Handshake timed out.");
        return;
    }

    // Check if handshake completed (both sides exchanged AppHandshake)
    if (s_sentHandshake && s_receivedHandshake) {
        SetState(State::Connected, "Session ready with %s.",
                 s_peerNickname[0] ? s_peerNickname : "peer");
    }
}

static void UpdateConnected() {
    s_stateTimer++;
    s_digestTimer++;

    // Periodic state digest during interactive gameplay
    if (NetplayHooks::IsInPlayableGameplay() && s_digestTimer >= kDigestIntervalFrames) {
        s_digestTimer = 0;
        SendStateDigest();
    }
}

static void UpdateCharSel() {
    s_stateTimer++;

    // Host: retransmit Ready if joiner hasn't acked
    if (s_mode == Mode::Host && !s_readyAckedByPeer) {
        s_readyRetransmitTimer++;
        if (s_readyRetransmitTimer >= kReadyRetransmitFrames) {
            s_readyRetransmitTimer = 0;
            PacketCodec::ReadyPayload ready = {};
            ready.ready_flags = s_charSelReadyFlags;
            SendTypedPacket(PacketCodec::PacketType::Ready, &ready, sizeof(ready), true);
            LOG_NET_INFO("[Session] Host retransmitting Ready (charselDelay=%d)",
                         DecodeCharSelReadyDelay(s_charSelReadyFlags));
        }
    }

    CharSelSync::SetRttMs(s_rttMs);
    CharSelSync::FrameUpdate();

    // If CharSel locked, start match bootstrap
    if (CharSelSync::GetPhase() == CharSelSync::Phase::Locked && !MatchBootstrap::IsActive()) {
        LOG_NET_INFO("[Session] CharSel locked — starting match bootstrap (avgRtt=%.1fms stableRtt=%.1fms)",
                     s_rttMs, GetDelaySelectionRttMs());
        CharSelSync::LockedMatchConfig config;
        if (CharSelSync::GetLockedConfig(&config)) {
            MatchBootstrap::Begin(&config,
                                  s_mode == Mode::Host,
                                  s_sessionId,
                                  s_connectionId,
                                  GetDelaySelectionRttMs());
        }
    }

    if (MatchBootstrap::IsActive()) {
        MatchBootstrap::SetObservedRttMs(GetDelaySelectionRttMs());
        MatchBootstrap::FrameUpdate();
        if (MatchBootstrap::GetPhase() == MatchBootstrap::Phase::Error) {
            SetError("Match bootstrap failed.");
        }
    }
}

static void UpdateGameplay() {
    s_stateTimer++;

    // Check rollback session state
    if (RollbackSession::IsActive() && RollbackSession::GetState() == RollbackSession::State::Error) {
        SetError("Rollback session error.");
        RollbackSession::Destroy();
        return;
    }

    // TODO: Drive spectator via ENet peer management
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void Reset() {
    if (s_state != State::Idle || s_lastError[0] != '\0') {
        LOG_NET_INFO("[Session] Resetting session state.");
    }

    if (s_peer) {
        enet_peer_disconnect_now(s_peer, 0);
        s_peer = nullptr;
    }

    UpnpManager::RemovePortMapping();
    DestroyHost();

    s_mode = Mode::None;
    s_state = State::Idle;
    s_revision = 0;
    s_peerNickname[0] = '\0';
    s_endpoint[0] = '\0';
    s_lastError[0] = '\0';
    CopyText(s_status, sizeof(s_status), "Session idle.");

    s_sessionId = 0;
    s_connectionId = 0;
    s_localNonce = 0;
    s_remoteNonce = 0;

    s_stateTimer = 0;
    s_digestTimer = 0;

    s_rttMs = 0.0f;
    s_rttSamples = 0;
    memset(s_delayRttSamples, 0, sizeof(s_delayRttSamples));
    s_delayRttSampleCount = 0;
    s_delayRttSampleIndex = 0;
    s_delayRttMs = 0.0f;

    s_packetsSent = 0;
    s_packetsReceived = 0;

    s_gekkoBufferHead = 0;
    s_gekkoBufferTail = 0;
    s_gekkoBufferCount = 0;
    s_gekkoPacketsBuffered = 0;
    s_gekkoPacketsDrained = 0;

    s_pendingCharSelStart = false;
    s_charSelReadyFlags = 0;
    s_readyAckedByPeer = false;
    s_readyRetransmitTimer = 0;

    s_sentHandshake = false;
    s_receivedHandshake = false;

    if (RollbackSession::IsActive()) {
        RollbackSession::Destroy();
    }
    if (CharSelSync::IsActive()) {
        CharSelSync::End();
    }

    AS2_ClearTickBaselineSync();
    StateDigest::ResetDesyncCount();
}

static bool StartInternal(const NetplayConfig::Config* config, Mode mode) {
    if (!config) return false;
    s_lastError[0] = '\0';

    AS2_ResetMatchWins();

    if (!config->nickname[0]) {
        CopyText(s_lastError, sizeof(s_lastError), "Set a local nickname before connecting.");
        SetState(State::Error, "%s", s_lastError);
        return false;
    }

    if (mode == Mode::Join) {
        bool zeroIp = config->target_ip[0] == 0 && config->target_ip[1] == 0 &&
                      config->target_ip[2] == 0 && config->target_ip[3] == 0;
        if (zeroIp) {
            CopyText(s_lastError, sizeof(s_lastError), "Set a target IP before connecting.");
            SetState(State::Error, "%s", s_lastError);
            return false;
        }
    }

    // Initialize ENet (idempotent)
    if (!s_enetInitialized) {
        if (enet_initialize() != 0) {
            CopyText(s_lastError, sizeof(s_lastError), "Failed to initialize ENet.");
            SetState(State::Error, "%s", s_lastError);
            return false;
        }
        s_enetInitialized = true;
    }

    // Create ENet host bound to listen port
    ENetAddress bindAddr;
    bindAddr.host = ENET_HOST_ANY;
    bindAddr.port = config->listen_port;

    s_enetHost = enet_host_create(&bindAddr, kENetMaxPeers, kENetChannels, 0, 0);
    if (!s_enetHost) {
        _snprintf_s(s_lastError, sizeof(s_lastError), _TRUNCATE,
                     "Failed to bind UDP port %u.", config->listen_port);
        SetState(State::Error, "%s", s_lastError);
        return false;
    }

    // Enable CRC32 checksum for packet integrity
    s_enetHost->checksum = enet_crc32;

    memcpy(&s_config, config, sizeof(s_config));
    s_mode = mode;
    s_localNonce = GenerateNonce();
    s_sessionId = s_localNonce;
    s_connectionId = (uint32_t)(s_localNonce & 0xFFFFFFFF);
    s_sentHandshake = false;
    s_receivedHandshake = false;

    uint16_t boundPort = config->listen_port;

    if (mode == Mode::Host) {
        _snprintf_s(s_endpoint, sizeof(s_endpoint), _TRUNCATE, "0.0.0.0:%u", boundPort);
        SetState(State::Connecting, "Hosting on port %u. Waiting for peer...", boundPort);
        LOG_NET_INFO("[Session] Host started on port %u (ENet)", boundPort);

        UpnpManager::AddPortMapping(boundPort);

        // Hole-punch: if host has a target address configured, also connect outbound.
        // This sends ENet SYN packets from our bound port to the joiner, creating a
        // NAT mapping so the joiner's return packets can reach us even without UPnP.
        bool hasTarget = config->target_ip[0] || config->target_ip[1] ||
                         config->target_ip[2] || config->target_ip[3];
        if (hasTarget && config->target_port > 0) {
            ENetAddress punchAddr;
            punchAddr.host = (enet_uint32)config->target_ip[0]
                           | ((enet_uint32)config->target_ip[1] << 8)
                           | ((enet_uint32)config->target_ip[2] << 16)
                           | ((enet_uint32)config->target_ip[3] << 24);
            punchAddr.port = (uint16_t)config->target_port;

            ENetPeer* punchPeer = enet_host_connect(s_enetHost, &punchAddr, kENetChannels, 0);
            if (punchPeer) {
                s_peer = punchPeer;
                LOG_NET_INFO("[Session] Host hole-punch: connecting to %u.%u.%u.%u:%u",
                             config->target_ip[0], config->target_ip[1],
                             config->target_ip[2], config->target_ip[3],
                             config->target_port);
            } else {
                LOG_NET_WARN("[Session] Host hole-punch: enet_host_connect failed (non-fatal)");
            }
        }
    } else {
        // Connect to the target host
        ENetAddress peerAddr;
        peerAddr.host = (enet_uint32)config->target_ip[0]
                      | ((enet_uint32)config->target_ip[1] << 8)
                      | ((enet_uint32)config->target_ip[2] << 16)
                      | ((enet_uint32)config->target_ip[3] << 24);
        peerAddr.port = (uint16_t)config->target_port;

        s_peer = enet_host_connect(s_enetHost, &peerAddr, kENetChannels, 0);
        if (!s_peer) {
            DestroyHost();
            CopyText(s_lastError, sizeof(s_lastError), "Failed to initiate ENet connection.");
            SetState(State::Error, "%s", s_lastError);
            return false;
        }

        NetplayConfig::FormatEndpoint(config, s_endpoint, sizeof(s_endpoint));
        SetState(State::Connecting, "Connecting to %s...", s_endpoint);
        LOG_NET_INFO("[Session] Joining %s from port %u (ENet)", s_endpoint, boundPort);
    }

    return true;
}

bool StartHost(const NetplayConfig::Config* config) {
    return StartInternal(config, Mode::Host);
}

bool StartJoin(const NetplayConfig::Config* config) {
    return StartInternal(config, Mode::Join);
}

void Cancel() {
    if (s_state == State::Connecting || s_state == State::Handshake) {
        LOG_NET_INFO("[Session] Connection cancelled by user.");
        if (s_peer) {
            enet_peer_disconnect_now(s_peer, 1);
            s_peer = nullptr;
        }
        UpnpManager::RemovePortMapping();
        DestroyHost();
        s_mode = Mode::None;
        SetState(State::Idle, "Connection cancelled.");
    }
}

void Disconnect(const char* reason) {
    if (s_peer) {
        enet_peer_disconnect(s_peer, 2);
        // Flush the disconnect notification
        enet_host_flush(s_enetHost);
        s_peer = nullptr;
    }
    UpnpManager::RemovePortMapping();
    DestroyHost();
    CopyText(s_lastError, sizeof(s_lastError),
             reason && reason[0] ? reason : "Session disconnected.");
    SetState(State::Error, "%s", s_lastError);
}

void EnterCharSel() {
    if (s_state == State::Connected) {
        CharSelSync::RestoreCursorsFromLastConfig();

        SetState(State::CharSel, "CharSel handoff ready for %s.",
                 s_peerNickname[0] ? s_peerNickname : "peer");

        s_readyAckedByPeer = false;
        s_readyRetransmitTimer = 0;

        const int charSelDelay = s_mode == Mode::Host
            ? ChooseHostCharSelDelay()
            : DecodeCharSelReadyDelay(s_charSelReadyFlags);

        if (s_mode == Mode::Host) {
            s_charSelReadyFlags = EncodeCharSelReadyFlags(charSelDelay);
        }

        CharSelSync::Begin(
            s_mode == Mode::Host,
            charSelDelay,
            s_sessionId,
            s_connectionId);

        LOG_NET_INFO("[Session] EnterCharSel (isHost=%d charSelDelay=%d rtt=%.1fms)",
                     s_mode == Mode::Host ? 1 : 0,
                     charSelDelay,
                     s_rttMs);
    }
}

void EnterGameplay() {
    if (s_state == State::CharSel || s_state == State::Connected) {
        if (CharSelSync::IsActive()) {
            CharSelSync::End();
        }
        SetState(State::Gameplay, "Entering gameplay with %s.",
                 s_peerNickname[0] ? s_peerNickname : "peer");
    }
}

void ReturnToSession() {
    if (s_state == State::CharSel || s_state == State::Gameplay) {
        if (RollbackSession::IsActive()) {
            RollbackSession::Destroy();
        }
        if (CharSelSync::IsActive()) {
            CharSelSync::End();
        }
        if (MatchBootstrap::IsActive()) {
            MatchBootstrap::Abort("Returning to session");
        }
        AS2_ClearTickBaselineSync();
        SetState(State::Connected, "Session resumed with %s.",
                 s_peerNickname[0] ? s_peerNickname : "peer");
    }
}

void FrameUpdate() {
    PollENet();

    switch (s_state) {
    case State::Connecting:
        UpdateConnecting();
        break;
    case State::Handshake:
        UpdateHandshake();
        break;
    case State::Connected:
        UpdateConnected();
        break;
    case State::CharSel:
        UpdateCharSel();
        break;
    case State::Gameplay:
        UpdateGameplay();
        break;
    default:
        break;
    }
}

const char* GetModeName(Mode mode) {
    switch (mode) {
        case Mode::Host: return "Host";
        case Mode::Join: return "Join";
        default: return "None";
    }
}

bool ConsumePendingCharSelStart() {
    if (s_pendingCharSelStart) {
        s_pendingCharSelStart = false;
        return true;
    }
    return false;
}

const char* GetStateName(State state) {
    switch (state) {
        case State::Idle:       return "Idle";
        case State::Connecting: return "Connecting";
        case State::Handshake:  return "Handshake";
        case State::Connected:  return "Connected";
        case State::CharSel:    return "CharSel";
        case State::Gameplay:   return "Gameplay";
        case State::Error:      return "Error";
        default: return "Unknown";
    }
}

bool GetSnapshot(Snapshot* out) {
    if (!out) return false;
    out->active           = s_state != State::Idle && s_state != State::Error;
    out->has_error        = s_state == State::Error;
    out->mode             = s_mode;
    out->state            = s_state;
    out->revision         = s_revision;
    out->frames_remaining = 0;
    out->rtt_ms           = s_rttMs;
    out->packets_sent     = s_packetsSent;
    out->packets_received = s_packetsReceived;
    out->desync_count     = StateDigest::GetDesyncCount();
    CopyText(out->peer_nickname, sizeof(out->peer_nickname), s_peerNickname);
    CopyText(out->endpoint, sizeof(out->endpoint), s_endpoint);
    CopyText(out->status, sizeof(out->status), s_status);
    CopyText(out->last_error, sizeof(out->last_error), s_lastError);
    return true;
}

void BufferGekkoPacket(const void* payload, int payloadLen) {
    if (!payload || payloadLen <= 0) return;
    if (payloadLen > (int)sizeof(BufferedPacket::data)) return;

    if (s_gekkoBufferCount >= kMaxGekkoBuffer) {
        s_gekkoBufferTail = (s_gekkoBufferTail + 1) % kMaxGekkoBuffer;
        s_gekkoBufferCount--;
    }

    BufferedPacket* pkt = &s_gekkoBuffer[s_gekkoBufferHead];
    memcpy(pkt->data, payload, payloadLen);
    pkt->len = payloadLen;
    s_gekkoBufferHead = (s_gekkoBufferHead + 1) % kMaxGekkoBuffer;
    s_gekkoBufferCount++;
    s_gekkoPacketsBuffered++;

    if (s_gekkoPacketsBuffered <= 8) {
        LOG_NET_DEBUG("[Session] Buffered GekkoData #%u len=%d queued=%d",
                      s_gekkoPacketsBuffered, payloadLen, s_gekkoBufferCount);
    }
}

int DrainGekkoPackets(BufferedPacket* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    int drained = 0;
    while (s_gekkoBufferCount > 0 && drained < maxCount) {
        memcpy(&out[drained], &s_gekkoBuffer[s_gekkoBufferTail], sizeof(BufferedPacket));
        s_gekkoBufferTail = (s_gekkoBufferTail + 1) % kMaxGekkoBuffer;
        s_gekkoBufferCount--;
        drained++;
    }
    if (drained > 0) {
        s_gekkoPacketsDrained += (uint32_t)drained;
        if (s_gekkoPacketsDrained <= 8) {
            LOG_NET_DEBUG("[Session] Drained %d GekkoData packet(s) (total=%u queued=%d)",
                          drained, s_gekkoPacketsDrained, s_gekkoBufferCount);
        }
    }
    return drained;
}

bool SendToPeer(PacketCodec::PacketType type, const void* payload, size_t payloadSize, bool reliable) {
    return SendTypedPacket(type, payload, payloadSize, reliable);
}

bool IsHost() {
    return s_mode == Mode::Host;
}

uint64_t GetSessionId() {
    return s_sessionId;
}

uint32_t GetConnectionId() {
    return s_connectionId;
}

float GetRttMs() {
    return s_rttMs;
}

void DriveSocketPoll() {
    PollENet();
}

} // namespace SessionManager
