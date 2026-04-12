#include <enet/enet.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "net/spectator_client.h"

#include "net/spectator_protocol.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

using namespace Net;

struct BufferedFrame {
    Spectator::FrameRecord record;
    bool valid;
};

struct BufferedPaletteState {
    bool     metadata_valid;
    bool     bank_valid;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    uint32_t payload_crc;
    uint16_t payload_size;
    uint8_t  data[NETPLAY_PALETTE_BANK_SIZE];
};

constexpr int kFastForwardGapFrames = 30;
constexpr int kHardSyncGapFrames = 180;
constexpr int kMaxEventsPerFrame = 64;
constexpr DWORD kStatusIntervalMs = 250;
constexpr DWORD kConnectTimeoutMs = 3000;
constexpr DWORD kHandshakeTimeoutMs = 3000;

static bool s_initialized = false;
static SpectatorClientState s_state = SpectatorClientState::Idle;
static ENetHost* s_clientHost = nullptr;
static ENetPeer* s_peer = nullptr;
static char s_endpoint[96] = "";
static char s_redirectEndpoint[96] = "";
static char s_status[128] = "Spectator client idle.";
static char s_error[128] = "";
static bool s_matchActive = false;
static uint32_t s_matchId = 0;
static int32_t s_bufferBaseRbFrame = -1;
static int32_t s_bufferEndRbFrame = -1;
static std::vector<BufferedFrame> s_buffer;
static int32_t s_serverConfirmedRbFrame = -1;
static int32_t s_serverLiveRbFrame = -1;
static int32_t s_playbackRbFrame = -1;
static bool s_fastForwardEnabled = true;
static bool s_hardSyncEnabled = false;
static bool s_shouldFastForward = false;
static bool s_needsHardSync = false;
static DWORD s_lastStatusSentAt = 0;
static DWORD s_stateEnteredAt = 0;
static bool s_deferredDestroyHost = false;
static char s_deferredDestroyReason[64] = "";
static uint32_t s_paletteEpoch = 0;
static BufferedPaletteState s_palette[2] = {};

static void CopyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void SetError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_error, sizeof(s_error), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void FailConnection(const char* message) {
    if (message && message[0]) {
        SetError("%s", message);
        SetStatus("%s", message);
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] fail reason=%s endpoint=%s",
            message,
            s_endpoint[0] ? s_endpoint : "(unset)");
    }
    s_matchActive = false;
    s_matchId = 0;
    s_state = SpectatorClientState::Failed;
    s_stateEnteredAt = 0;
}

static void ResetBuffer() {
    s_buffer.clear();
    s_bufferBaseRbFrame = -1;
    s_bufferEndRbFrame = -1;
    s_serverConfirmedRbFrame = -1;
    s_serverLiveRbFrame = -1;
    s_playbackRbFrame = -1;
    s_shouldFastForward = false;
    s_needsHardSync = false;
    s_paletteEpoch = 0;
    memset(s_palette, 0, sizeof(s_palette));
}

static void DestroyClientHostNow(const char* reason) {
    const bool hadResources = (s_peer != nullptr) || (s_clientHost != nullptr);
    if (s_peer) {
        enet_peer_disconnect_now(s_peer, 0);
        s_peer = nullptr;
    }
    if (s_clientHost) {
        enet_host_destroy(s_clientHost);
        s_clientHost = nullptr;
    }
    s_deferredDestroyHost = false;
    s_deferredDestroyReason[0] = '\0';
    if (hadResources) {
        LOG_NETPLAY(LOG_INFO,
            "[SCLIENT] cleanup_complete reason=%s endpoint=%s",
            reason && reason[0] ? reason : "unspecified",
            s_endpoint[0] ? s_endpoint : "(unset)");
    }
}

static void RequestDeferredDestroy(const char* reason) {
    if (!s_clientHost) {
        return;
    }

    CopyText(s_deferredDestroyReason,
        sizeof(s_deferredDestroyReason),
        reason && reason[0] ? reason : "deferred");
    if (!s_deferredDestroyHost) {
        LOG_NETPLAY(LOG_INFO,
            "[SCLIENT] defer_teardown reason=%s endpoint=%s",
            s_deferredDestroyReason,
            s_endpoint[0] ? s_endpoint : "(unset)");
    }
    s_deferredDestroyHost = true;
}

static bool ParseEndpointText(const char* text, char* outHost, size_t outHostSize, uint16_t* outPort) {
    if (!text || !text[0] || !outHost || outHostSize == 0 || !outPort) {
        return false;
    }

    outHost[0] = '\0';
    *outPort = 0;

    if (text[0] == '[') {
        const char* close = strchr(text, ']');
        if (!close || close[1] != ':') {
            return false;
        }
        const size_t hostLen = (size_t)(close - (text + 1));
        if (hostLen == 0 || hostLen >= outHostSize) {
            return false;
        }
        memcpy(outHost, text + 1, hostLen);
        outHost[hostLen] = '\0';
        const int port = atoi(close + 2);
        if (port <= 0 || port > 65535) {
            return false;
        }
        *outPort = (uint16_t)port;
        return true;
    }

    const char* colon = strrchr(text, ':');
    if (!colon || colon == text) {
        return false;
    }
    const size_t hostLen = (size_t)(colon - text);
    if (hostLen == 0 || hostLen >= outHostSize) {
        return false;
    }
    memcpy(outHost, text, hostLen);
    outHost[hostLen] = '\0';
    const int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        return false;
    }
    *outPort = (uint16_t)port;
    return true;
}

static bool SendTyped(uint8_t channel,
                      Spectator::PacketType type,
                      const void* payload,
                      size_t payloadLen,
                      bool reliable) {
    if (!s_peer) {
        return false;
    }
    if (sizeof(Spectator::PacketType) + payloadLen > (size_t)Spectator::MAX_PACKET_SIZE) {
        return false;
    }

    uint8_t buffer[Spectator::MAX_PACKET_SIZE] = {};
    memcpy(buffer, &type, sizeof(type));
    if (payload && payloadLen > 0) {
        memcpy(buffer + sizeof(type), payload, payloadLen);
    }

    ENetPacket* packet = enet_packet_create(buffer,
        sizeof(type) + payloadLen,
        reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (!packet) {
        return false;
    }
    if (enet_peer_send(s_peer, channel, packet) < 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

static BufferedFrame* EnsureBufferSlot(int32_t rb_frame) {
    if (rb_frame < 0) {
        return nullptr;
    }
    if (s_buffer.empty()) {
        s_bufferBaseRbFrame = rb_frame;
        s_buffer.resize(1);
        s_bufferEndRbFrame = rb_frame;
        return &s_buffer[0];
    }
    if (rb_frame < s_bufferBaseRbFrame) {
        return nullptr;
    }
    const size_t index = (size_t)(rb_frame - s_bufferBaseRbFrame);
    if (index >= s_buffer.size()) {
        s_buffer.resize(index + 1);
    }
    s_bufferEndRbFrame = (std::max)(s_bufferEndRbFrame, rb_frame);
    return &s_buffer[index];
}

static void AdvanceSyntheticPlayback() {
    if (!s_matchActive || s_bufferEndRbFrame < 0) {
        s_shouldFastForward = false;
        s_needsHardSync = false;
        return;
    }

    if (s_playbackRbFrame < s_bufferBaseRbFrame) {
        s_playbackRbFrame = s_bufferBaseRbFrame;
    }

    const int32_t gap = s_bufferEndRbFrame - s_playbackRbFrame;
    s_shouldFastForward = false;
    s_needsHardSync = false;

    if (s_hardSyncEnabled && gap >= kHardSyncGapFrames) {
        s_playbackRbFrame = s_bufferEndRbFrame - 2;
        s_needsHardSync = true;
    } else {
        int step = 1;
        if (s_fastForwardEnabled && gap >= kFastForwardGapFrames) {
            step = 4;
            s_shouldFastForward = true;
        }
        s_playbackRbFrame = (std::min)(s_bufferEndRbFrame, s_playbackRbFrame + step);
    }
}

static void SendClientStatusIfNeeded() {
    if (!s_peer || s_state != SpectatorClientState::Streaming) {
        return;
    }

    const DWORD now = GetTickCount();
    if ((now - s_lastStatusSentAt) < kStatusIntervalMs) {
        return;
    }

    Spectator::ClientStatusPayload payload{};
    payload.match_id = s_matchId;
    payload.playback_rb_frame = s_playbackRbFrame;
    payload.buffered_frame_count = (s_playbackRbFrame >= 0 && s_bufferEndRbFrame >= s_playbackRbFrame)
        ? (s_bufferEndRbFrame - s_playbackRbFrame)
        : 0;
    if (s_shouldFastForward) {
        payload.flags |= Spectator::CLIENT_STATUS_FLAG_FAST_FORWARD;
    }
    if (s_needsHardSync) {
        payload.flags |= Spectator::CLIENT_STATUS_FLAG_HARD_SYNC;
    }

    if (SendTyped(Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::ClientStatus,
            &payload,
            sizeof(payload),
            true)) {
        s_lastStatusSentAt = now;
    }
}

} // namespace

namespace Net {

void SpectatorClient_Init() {
    if (s_initialized) {
        return;
    }
    s_state = SpectatorClientState::Idle;
    s_endpoint[0] = '\0';
    s_redirectEndpoint[0] = '\0';
    s_status[0] = '\0';
    s_error[0] = '\0';
    s_matchActive = false;
    s_matchId = 0;
    ResetBuffer();
    SetStatus("Spectator client idle.");
    s_stateEnteredAt = 0;
    s_deferredDestroyHost = false;
    s_deferredDestroyReason[0] = '\0';
    s_initialized = true;
}

void SpectatorClient_Shutdown() {
    if (!s_initialized) {
        return;
    }
    SpectatorClient_Disconnect("shutdown");
    s_initialized = false;
}

bool SpectatorClient_StartConnect(const char* endpoint) {
    if (!s_initialized || !endpoint || !endpoint[0]) {
        return false;
    }

    char host[96] = {};
    uint16_t port = 0;
    if (!ParseEndpointText(endpoint, host, sizeof(host), &port)) {
        SetError("Invalid spectator endpoint.");
        SetStatus("Invalid spectator endpoint.");
        s_state = SpectatorClientState::Failed;
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] fail reason=invalid_endpoint endpoint=%s",
            endpoint ? endpoint : "(null)");
        return false;
    }

    DestroyClientHostNow("start_connect_reset");
    ResetBuffer();
    s_matchActive = false;
    s_matchId = 0;
    s_error[0] = '\0';
    s_redirectEndpoint[0] = '\0';
    s_deferredDestroyHost = false;
    s_deferredDestroyReason[0] = '\0';

    s_clientHost = enet_host_create(nullptr, 1, Spectator::NUM_CHANNELS, 0, 0);
    if (!s_clientHost) {
        SetError("Failed to create spectator client host.");
        SetStatus("Failed to create spectator client host.");
        s_state = SpectatorClientState::Failed;
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] fail reason=create_host endpoint=%s",
            endpoint);
        return false;
    }

    ENetAddress address{};
    address.port = port;
    if (enet_address_set_host(&address, host) < 0) {
        DestroyClientHostNow("resolve_failed");
        SetError("Failed to resolve spectator endpoint.");
        SetStatus("Failed to resolve spectator endpoint.");
        s_state = SpectatorClientState::Failed;
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] fail reason=resolve_failed endpoint=%s",
            endpoint);
        return false;
    }

    s_peer = enet_host_connect(s_clientHost, &address, Spectator::NUM_CHANNELS, 0);
    if (!s_peer) {
        DestroyClientHostNow("connect_start_failed");
        SetError("Failed to start spectator connect.");
        SetStatus("Failed to start spectator connect.");
        s_state = SpectatorClientState::Failed;
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] fail reason=connect_start_failed endpoint=%s",
            endpoint);
        return false;
    }

    CopyText(s_endpoint, sizeof(s_endpoint), endpoint);
    SetStatus("Connecting to spectator endpoint %s", endpoint);
    s_state = SpectatorClientState::Connecting;
    s_stateEnteredAt = GetTickCount();
    LOG_NETPLAY(LOG_INFO,
        "[SCLIENT] start_connect endpoint=%s",
        endpoint);
    return true;
}

void SpectatorClient_Disconnect(const char* reason) {
    if (s_peer) {
        Spectator::DisconnectPayload payload{};
        payload.reason_code = 0;
        CopyText(payload.message, sizeof(payload.message), reason ? reason : "disconnect");
        SendTyped(Spectator::CHANNEL_CONTROL,
            Spectator::PacketType::Disconnect,
            &payload,
            sizeof(payload),
            true);
    }

    DestroyClientHostNow(reason ? reason : "disconnect");
    ResetBuffer();
    s_matchActive = false;
    s_matchId = 0;
    s_state = SpectatorClientState::Idle;
    SetStatus("Spectator client idle.");
    s_stateEnteredAt = 0;
}

void SpectatorClient_FrameUpdate() {
    if (!s_initialized || !s_clientHost) {
        return;
    }

    bool stopProcessing = false;
    ENetEvent event{};
    int processedEvents = 0;
    while (s_clientHost && !stopProcessing && processedEvents < kMaxEventsPerFrame &&
           enet_host_service(s_clientHost, &event, 0) > 0) {
        processedEvents++;
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                Spectator::HelloPayload hello{};
                hello.protocol_version = Spectator::PROTOCOL_VERSION;
                hello.flags = Spectator::HELLO_FLAG_ACCEPT_REDIRECT;
                CopyText(hello.nickname, sizeof(hello.nickname), "Spectator");
                SendTyped(Spectator::CHANNEL_CONTROL,
                    Spectator::PacketType::Hello,
                    &hello,
                    sizeof(hello),
                    true);
                s_state = SpectatorClientState::Handshaking;
                s_stateEnteredAt = GetTickCount();
                SetStatus("Spectator handshake started.");
                LOG_NETPLAY(LOG_INFO,
                    "[SCLIENT] connected endpoint=%s -> handshake_start",
                    s_endpoint[0] ? s_endpoint : "(unset)");
                break;
            }

            case ENET_EVENT_TYPE_RECEIVE: {
                if (!event.packet) {
                    break;
                }

                if (!Spectator::ValidatePacketSize(event.packet->data, event.packet->dataLength)) {
                    enet_packet_destroy(event.packet);
                    break;
                }

                const Spectator::PacketType type = Spectator::ReadPacketType(event.packet->data);
                const void* payload = Spectator::GetPayloadPtr(event.packet->data);
                const size_t payloadLen = Spectator::GetPayloadSize(event.packet->dataLength);

                switch (type) {
                    case Spectator::PacketType::HelloAck:
                        if (payloadLen >= sizeof(Spectator::HelloAckPayload)) {
                            const auto* ack = static_cast<const Spectator::HelloAckPayload*>(payload);
                            s_matchId = ack->match_id;
                            s_matchActive = ack->match_state == Spectator::MATCH_STATE_ACTIVE;
                            if (!s_matchActive) {
                                LOG_NETPLAY(LOG_WARNING,
                                    "[SCLIENT] fail reason=no_active_match endpoint=%s",
                                    s_endpoint[0] ? s_endpoint : "(unset)");
                                FailConnection("No active match to spectate.");
                                RequestDeferredDestroy("fail_during_receive");
                                stopProcessing = true;
                            } else {
                                SetStatus("Connected to spectator server.");
                                LOG_NETPLAY(LOG_INFO,
                                    "[SCLIENT] hello_ack endpoint=%s match_id=0x%08X active_match=1",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId);
                            }
                        }
                        break;

                    case Spectator::PacketType::Redirect:
                        if (payloadLen >= sizeof(Spectator::RedirectPayload)) {
                            const auto* redirect = static_cast<const Spectator::RedirectPayload*>(payload);
                            CopyText(s_redirectEndpoint, sizeof(s_redirectEndpoint), redirect->endpoint);
                            SetStatus("Redirected to %s", s_redirectEndpoint);
                            s_state = SpectatorClientState::Redirected;
                            s_stateEnteredAt = 0;
                            LOG_NETPLAY(LOG_INFO,
                                "[SCLIENT] fail reason=redirect endpoint=%s redirect=%s",
                                s_endpoint[0] ? s_endpoint : "(unset)",
                                s_redirectEndpoint[0] ? s_redirectEndpoint : "(unset)");
                            RequestDeferredDestroy("redirect_during_receive");
                            stopProcessing = true;
                        }
                        break;

                    case Spectator::PacketType::MatchState:
                        if (payloadLen >= sizeof(Spectator::MatchStatePayload)) {
                            const auto* match = static_cast<const Spectator::MatchStatePayload*>(payload);
                            s_matchId = match->match_id;
                            s_matchActive = match->match_state == Spectator::MATCH_STATE_ACTIVE;
                            s_serverConfirmedRbFrame = match->confirmed_rb_frame;
                            s_serverLiveRbFrame = match->live_rb_frame;
                            if (s_matchActive) {
                                s_state = SpectatorClientState::Streaming;
                                s_stateEnteredAt = 0;
                                SetStatus("Spectator stream active: live=%d confirmed=%d",
                                    s_serverLiveRbFrame,
                                    s_serverConfirmedRbFrame);
                                LOG_NETPLAY(LOG_INFO,
                                    "[SCLIENT] stream_active endpoint=%s match_id=0x%08X live=%d confirmed=%d",
                                    s_endpoint[0] ? s_endpoint : "(unset)",
                                    s_matchId,
                                    s_serverLiveRbFrame,
                                    s_serverConfirmedRbFrame);
                            }
                        }
                        break;

                    case Spectator::PacketType::FrameBatch:
                        if (payloadLen >= offsetof(Spectator::FrameBatchPayload, records)) {
                            const auto* batch = static_cast<const Spectator::FrameBatchPayload*>(payload);
                            const uint16_t recordCount = (std::min)(batch->record_count,
                                (uint16_t)Spectator::MAX_FRAME_BATCH);
                            s_matchId = batch->match_id;
                            s_serverConfirmedRbFrame = batch->confirmed_rb_frame;
                            s_serverLiveRbFrame = batch->live_rb_frame;
                            for (uint16_t index = 0; index < recordCount; index++) {
                                BufferedFrame* slot = EnsureBufferSlot(batch->records[index].rb_frame);
                                if (!slot) {
                                    continue;
                                }
                                slot->record = batch->records[index];
                                slot->valid = true;
                            }
                            if (s_playbackRbFrame < 0) {
                                s_playbackRbFrame = s_bufferBaseRbFrame;
                            }
                            s_state = SpectatorClientState::Streaming;
                            s_stateEnteredAt = 0;
                            s_matchActive = true;
                        }
                        break;

                    case Spectator::PacketType::PaletteState:
                        if (payloadLen >= sizeof(Spectator::PaletteStatePayload)) {
                            const auto* palette = static_cast<const Spectator::PaletteStatePayload*>(payload);
                            s_matchId = palette->match_id;
                            s_paletteEpoch = palette->palette_epoch;
                            for (int index = 0; index < 2; index++) {
                                s_palette[index].metadata_valid = true;
                                s_palette[index].character_id = palette->player[index].character_id;
                                s_palette[index].base_palette = palette->player[index].base_palette;
                                s_palette[index].flags = palette->player[index].flags;
                                s_palette[index].payload_crc = palette->player[index].payload_crc;
                                s_palette[index].payload_size = palette->player[index].payload_size;
                                if (!palette->player[index].has_custom_data) {
                                    s_palette[index].bank_valid = false;
                                    memset(s_palette[index].data, 0, sizeof(s_palette[index].data));
                                }
                            }
                        }
                        break;

                    case Spectator::PacketType::PaletteData:
                        if (payloadLen >= sizeof(Spectator::PaletteDataPayload)) {
                            const auto* palette = static_cast<const Spectator::PaletteDataPayload*>(payload);
                            if (palette->game_slot < 2 &&
                                palette->payload_size == NETPLAY_PALETTE_BANK_SIZE) {
                                s_matchId = palette->match_id;
                                s_paletteEpoch = palette->palette_epoch;
                                BufferedPaletteState& slot = s_palette[palette->game_slot];
                                slot.metadata_valid = true;
                                slot.bank_valid = true;
                                slot.character_id = palette->character_id;
                                slot.base_palette = palette->base_palette;
                                slot.payload_crc = palette->payload_crc;
                                slot.payload_size = palette->payload_size;
                                memcpy(slot.data, palette->payload, NETPLAY_PALETTE_BANK_SIZE);
                            }
                        }
                        break;

                    case Spectator::PacketType::Heartbeat:
                        if (payloadLen >= sizeof(Spectator::HeartbeatPayload)) {
                            const auto* heartbeat = static_cast<const Spectator::HeartbeatPayload*>(payload);
                            s_matchId = heartbeat->match_id;
                            s_serverConfirmedRbFrame = heartbeat->confirmed_rb_frame;
                            s_serverLiveRbFrame = heartbeat->live_rb_frame;
                            s_matchActive = heartbeat->match_state == Spectator::MATCH_STATE_ACTIVE;
                        }
                        break;

                    case Spectator::PacketType::Disconnect:
                        if (payloadLen >= sizeof(Spectator::DisconnectPayload)) {
                            const auto* disconnect = static_cast<const Spectator::DisconnectPayload*>(payload);
                            FailConnection(disconnect->message);
                        } else {
                            FailConnection("Spectator stream disconnected.");
                        }
                        RequestDeferredDestroy("fail_during_receive");
                        stopProcessing = true;
                        break;

                    case Spectator::PacketType::ClientStatus:
                    case Spectator::PacketType::Hello:
                        break;
                }

                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                s_peer = nullptr;
                if (s_state != SpectatorClientState::Redirected) {
                    if (s_state == SpectatorClientState::Connecting ||
                        s_state == SpectatorClientState::Handshaking) {
                        FailConnection("Spectator endpoint unavailable.");
                    } else {
                        FailConnection(s_error[0] ? s_error : "Spectator connection closed.");
                    }
                    RequestDeferredDestroy("disconnect_event");
                    stopProcessing = true;
                }
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    if (s_deferredDestroyHost && s_clientHost) {
        DestroyClientHostNow(s_deferredDestroyReason[0] ? s_deferredDestroyReason : "deferred");
    }

    if (s_state == SpectatorClientState::Redirected ||
        s_state == SpectatorClientState::Failed) {
        return;
    }

    if (!s_clientHost) {
        return;
    }

    const DWORD now = GetTickCount();
    if (s_state == SpectatorClientState::Connecting &&
        s_stateEnteredAt != 0 &&
        (now - s_stateEnteredAt) >= kConnectTimeoutMs) {
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] connect_timeout endpoint=%s",
            s_endpoint[0] ? s_endpoint : "(unset)");
        FailConnection("Spectator connect timed out.");
        DestroyClientHostNow("connect_timeout");
        return;
    }

    if (s_state == SpectatorClientState::Handshaking &&
        s_stateEnteredAt != 0 &&
        (now - s_stateEnteredAt) >= kHandshakeTimeoutMs) {
        LOG_NETPLAY(LOG_WARNING,
            "[SCLIENT] handshake_timeout endpoint=%s",
            s_endpoint[0] ? s_endpoint : "(unset)");
        FailConnection("Spectator handshake timed out.");
        DestroyClientHostNow("handshake_timeout");
        return;
    }

    AdvanceSyntheticPlayback();
    SendClientStatusIfNeeded();
    enet_host_flush(s_clientHost);
}

void SpectatorClient_SetFastForwardEnabled(bool enabled) {
    s_fastForwardEnabled = enabled;
}

void SpectatorClient_SetHardSyncEnabled(bool enabled) {
    s_hardSyncEnabled = enabled;
}

SpectatorClientState SpectatorClient_GetState() {
    return s_state;
}

void SpectatorClient_GetSnapshot(SpectatorClientSnapshot* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->active = s_state != SpectatorClientState::Idle;
    out->state = s_state;
    CopyText(out->endpoint, sizeof(out->endpoint), s_endpoint);
    CopyText(out->redirect_endpoint, sizeof(out->redirect_endpoint), s_redirectEndpoint);
    out->match_id = s_matchId;
    out->match_active = s_matchActive;
    out->buffered_start_rb_frame = s_bufferBaseRbFrame;
    out->buffered_end_rb_frame = s_bufferEndRbFrame;
    out->server_confirmed_rb_frame = s_serverConfirmedRbFrame;
    out->server_live_rb_frame = s_serverLiveRbFrame;
    out->playback_rb_frame = s_playbackRbFrame;
    out->buffered_frame_count =
        (s_bufferBaseRbFrame >= 0 && s_bufferEndRbFrame >= s_bufferBaseRbFrame)
            ? (uint32_t)(s_bufferEndRbFrame - s_bufferBaseRbFrame + 1)
            : 0;
    out->should_fast_forward = s_shouldFastForward;
    out->needs_hard_sync = s_needsHardSync;
    CopyText(out->status, sizeof(out->status), s_status);
    CopyText(out->error, sizeof(out->error), s_error);
}

} // namespace Net