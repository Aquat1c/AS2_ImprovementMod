/**
 * Alice Senki 2 - Character Select Sync Manager Implementation
 *
 * Deterministic lockstep for CharSel (Mode 6).
 * Game runs as GAMETYPE_VS_HUMAN — mod relays inputs frame-by-frame.
 * Both sides must have each other's input for frame N before advancing.
 * Redundant history in each packet ensures single-packet loss is recoverable.
 */

#include "charsel_sync.h"
#include "session_manager.h"
#include "as2_constants.h"
#include "game_state.h"
#include "input_system.h"
#include "log_window.h"
#include "test_harness.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>

namespace CharSelSync {

// ============================================================================
// Constants
// ============================================================================

static const int kInputRingSize    = 512;   // Circular buffer size (power of 2)
static const int kInputRingMask    = kInputRingSize - 1;
static const int kMinInputDelay    = 2;     // Minimum input delay (frames)
static const int kMaxInputDelay    = 10;    // Maximum input delay (frames)
static const int kDefaultInputDelay = 2;    // Default until RTT is known
static const int kStallLogInterval = 60;    // Log a stall warning every N stall frames

// ============================================================================
// Internal State
// ============================================================================

static bool        s_active = false;
static bool        s_isHost = false;
static uint64_t    s_sessionId    = 0;
static uint32_t    s_connectionId = 0;

static Phase       s_phase = Phase::Enter;

// Lockstep frame management
// s_localFrame: the next frame to CONSUME (i.e., the frame we need both inputs for)
// s_localInputFrame: the next frame to PRODUCE local input for  
// Both start at 0. s_localInputFrame gets ahead by inputDelay frames.
static int32_t     s_localFrame      = 0;   // Next frame to consume
static int32_t     s_localInputFrame = 0;   // Next frame to produce local input for
static int32_t     s_inputDelay      = kDefaultInputDelay;
static float       s_rttMs           = 0.0f;
static bool        s_inputDelayLocked = false; // Lock delay after first frame produced
static bool        s_inputDelayPinned = false;  // Host-selected delay shared by both peers

// Input ring buffers (circular, indexed by frame & kInputRingMask)
static uint16_t    s_localInputs[kInputRingSize];
static bool        s_hasLocalInput[kInputRingSize];
static uint16_t    s_remoteInputs[kInputRingSize];
static bool        s_hasRemoteInput[kInputRingSize];

// Latest remote frame we know about (for ack field in outgoing packets)
static int32_t     s_remoteLatestFrame = -1;
static int32_t     s_peerAckedLocalFrame = -1;

// Statistics
static uint32_t    s_totalStalls = 0;
static uint32_t    s_consecutiveStalls = 0;
static uint32_t    s_packetsSent = 0;
static uint32_t    s_packetsReceived = 0;

// Config lock (MatchConfig exchange)
static bool        s_configLocked = false;
static LockedMatchConfig s_lockedConfig = {};
static bool        s_sentMatchConfig = false;
static bool        s_receivedMatchConfigAck = false;
static PacketCodec::MatchConfigPayload s_pendingConfig = {};

// Persistent last-confirmed config — survives End()/Begin() cycles.
// Used to sync cursor positions when re-entering charsel after a match.
static bool        s_hasLastConfirmedConfig = false;
static LockedMatchConfig s_lastConfirmedConfig = {};

// Game state monitoring for config lock detection
static uint32_t    s_prevP1CharId  = 0xFFFFFFFF;
static uint32_t    s_prevP2CharId  = 0xFFFFFFFF;
static uint8_t     s_prevP1Palette = 0xFF;
static uint8_t     s_prevP2Palette = 0xFF;
static uint8_t     s_prevP1Confirm = 0xFF;
static uint8_t     s_prevP2Confirm = 0xFF;
static uint8_t     s_prevStageId = 0xFF;
static bool        s_bothConfirmedSeen = false;
static uint32_t    s_lastLoggedMode = 0xFFFFFFFF;
static uint32_t    s_lastLoggedSubstate = 0xFFFFFFFF;

// Both-ready flags for MatchConfig trigger
static bool        s_localReady  = false;
static bool        s_remoteReady = false;

// Forward declarations
static void PollGameState();

// ============================================================================
// Helpers
// ============================================================================

static const char* PhaseToString(Phase p) {
    switch (p) {
        case Phase::Enter:  return "Enter";
        case Phase::Active: return "Active";
        case Phase::Locked: return "Locked";
        default:            return "Unknown";
    }
}

static uint8_t LocalSlot() {
    return s_isHost ? 1 : 2;
}

static uint8_t RemoteSlot() {
    return s_isHost ? 2 : 1;
}

static const char* CharSelSubstateToString(uint32_t substate) {
    switch (substate) {
        case CHARSEL_SUB_INIT:        return "Init";
        case CHARSEL_SUB_FADEIN:      return "FadeIn";
        case CHARSEL_SUB_SELECT:      return "Select";
        case CHARSEL_SUB_CANCEL:      return "Cancel";
        case CHARSEL_SUB_CONFIRM:     return "Confirm";
        case CHARSEL_SUB_TRANS5:      return "Trans5";
        case CHARSEL_SUB_TRANS6:      return "Trans6";
        case CHARSEL_SUB_PREVIEW:     return "Preview";
        case CHARSEL_SUB_STAGE_INTRO: return "StageIntro";
        case CHARSEL_SUB_LOADING:     return "Loading";
        case CHARSEL_SUB_PREMATCH:    return "PreMatch";
        case CHARSEL_SUB_STAGE:       return "Stage";
        case CHARSEL_SUB_BACK_MENU:   return "BackMenu";
        case CHARSEL_SUB_BACK_LOBBY:  return "BackLobby";
        case CHARSEL_SUB_TO_MATCH:    return "ToMatch";
        default:                      return "Unknown";
    }
}

static uint16_t CanonicalizeStageAxis(uint16_t input, uint16_t firstMask, uint16_t secondMask) {
    const uint16_t axisMask = (uint16_t)(firstMask | secondMask);
    const uint16_t axis = (uint16_t)(input & axisMask);
    if (axis == axisMask) {
        return firstMask;
    }
    return axis;
}

static uint16_t CanonicalizeSingleStageInput(uint16_t input) {
    uint16_t result = input;

    result &= (uint16_t)(INPUT_UP | INPUT_DOWN | INPUT_LEFT | INPUT_RIGHT |
                         INPUT_A | INPUT_B | INPUT_C | INPUT_D |
                         INPUT_START | INPUT_SELECT);

    result &= (uint16_t)~(INPUT_LEFT | INPUT_RIGHT);
    result |= CanonicalizeStageAxis(input, INPUT_LEFT, INPUT_RIGHT);

    result &= (uint16_t)~(INPUT_UP | INPUT_DOWN);
    result |= CanonicalizeStageAxis(input, INPUT_UP, INPUT_DOWN);

    return result;
}

static uint16_t ResolveSharedStageAxis(uint16_t p1Input, uint16_t p2Input,
                                       uint16_t firstMask, uint16_t secondMask) {
    const uint16_t combinedAxis = (uint16_t)((p1Input | p2Input) & (firstMask | secondMask));

    // Match the game's menu branch order: when both directions are present in
    // the same lockstep frame, prefer the branch that vanilla checks first.
    if ((combinedAxis & firstMask) && (combinedAxis & secondMask)) {
        return firstMask;
    }

    return combinedAxis;
}

uint16_t CanonicalizeSharedStageInput(uint16_t p1Input, uint16_t p2Input) {
    const uint16_t canonicalP1 = CanonicalizeSingleStageInput(p1Input);
    const uint16_t canonicalP2 = CanonicalizeSingleStageInput(p2Input);

    uint16_t result = 0;
    result |= ResolveSharedStageAxis(canonicalP1, canonicalP2, INPUT_LEFT, INPUT_RIGHT);
    result |= ResolveSharedStageAxis(canonicalP1, canonicalP2, INPUT_UP, INPUT_DOWN);

    const uint16_t buttonMask = (uint16_t)(INPUT_A | INPUT_B | INPUT_C | INPUT_D |
                                           INPUT_START | INPUT_SELECT);
    const uint16_t confirmMask = (uint16_t)(INPUT_A | INPUT_C);
    const uint16_t cancelMask = (uint16_t)(INPUT_B | INPUT_D | INPUT_SELECT);
    const uint16_t combinedButtons = (uint16_t)((canonicalP1 | canonicalP2) & buttonMask);

    const bool haveConfirm = (combinedButtons & confirmMask) != 0;
    const bool haveCancel = (combinedButtons & cancelMask) != 0;
    if (haveConfirm && haveCancel) {
        result |= (uint16_t)(combinedButtons & confirmMask);
    } else {
        result |= combinedButtons;
    }

    return result;
}

static int ComputeInputDelay(float rttMs) {
    if (rttMs <= 0.0f) return kDefaultInputDelay;
    // One-way trip in frames: RTT/2 converted to 60fps frames, plus 1 safety margin
    int delay = (int)ceilf(rttMs * 60.0f / 1000.0f / 2.0f) + 1;
    if (delay < kMinInputDelay) delay = kMinInputDelay;
    if (delay > kMaxInputDelay) delay = kMaxInputDelay;
    return delay;
}

int SuggestInputDelay(float rttMs) {
    return ComputeInputDelay(rttMs);
}

static void SendInputPacket(int32_t frame) {

    PacketCodec::CharSelInputPayload payload = {};
    payload.frame     = (uint32_t)frame;
    payload.ack_frame = (uint32_t)(s_remoteLatestFrame >= 0 ? s_remoteLatestFrame : 0);

    // Pack redundant history: inputs[0] = frame, inputs[1] = frame-1, ...
    int count = 0;
    for (int i = 0; i < PacketCodec::kCharSelInputRedundancy; i++) {
        int f = frame - i;
        if (f < 0) break;
        int idx = f & kInputRingMask;
        if (!s_hasLocalInput[idx]) break;
        payload.inputs[i] = s_localInputs[idx];
        count++;
    }
    payload.input_count = (uint16_t)count;

    SessionManager::SendToPeer(PacketCodec::PacketType::CharSelInput,
                               &payload, sizeof(payload), false);
    s_packetsSent++;

    if (s_packetsSent <= 4 || (s_packetsSent % 120) == 0) {
        LOG_NET_DEBUG("[CharSel] SendInputPacket frame=%d ack=%u count=%d (#%u)",
                      frame, payload.ack_frame, count, s_packetsSent);
    }
}

static void TryLockConfig() {
    if (!s_localReady || !s_remoteReady) return;
    if (s_configLocked) return;

    // Host sends MatchConfig (once)
    if (s_isHost && !s_sentMatchConfig) {
        LOG_NET_INFO("[CharSel] Both players ready — host sending MatchConfig...");

        uint32_t p1Char = *reinterpret_cast<uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID);
        uint32_t p2Char = *reinterpret_cast<uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID);
        uint32_t p1Pal  = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P1_PALETTE);
        uint32_t p2Pal  = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P2_PALETTE);
        uint32_t stage  = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_STAGE_ID);

        PacketCodec::MatchConfigPayload config = {};
        config.p1_character = p1Char;
        config.p2_character = p2Char;
        config.p1_palette   = p1Pal;
        config.p2_palette   = p2Pal;
        config.stage_id     = stage;
        config.round_count  = 2;
        config.timer_setting = 99;
        config.gameplay_flags = 0;
        config.session_seed  = PacketCodec::GetTimestampMs();
        config.config_hash   = PacketCodec::HashMatchConfig(&config);

        s_pendingConfig = config;

        SessionManager::SendToPeer(PacketCodec::PacketType::MatchConfig,
            &config, sizeof(config), true);

        s_sentMatchConfig = true;
        LOG_NET_INFO("[CharSel] Host sent MatchConfig (P1=char%u/pal%u P2=char%u/pal%u stage=%u hash=0x%08X)",
                     p1Char, p1Pal, p2Char, p2Pal, stage, config.config_hash);
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void Begin(bool isHost,
        int negotiatedDelay,
        uint64_t sessionId, uint32_t connectionId) {
    s_active = true;
    s_isHost = isHost;
    s_sessionId    = sessionId;
    s_connectionId = connectionId;

    s_phase = Phase::Enter;
    s_localFrame      = 0;
    s_localInputFrame = 0;
    s_inputDelay      = negotiatedDelay > 0 ? negotiatedDelay : kDefaultInputDelay;
    s_rttMs           = 0.0f;
    s_inputDelayLocked = false;
    s_inputDelayPinned = negotiatedDelay > 0;

    if (s_inputDelay < kMinInputDelay) s_inputDelay = kMinInputDelay;
    if (s_inputDelay > kMaxInputDelay) s_inputDelay = kMaxInputDelay;

    memset(s_localInputs, 0, sizeof(s_localInputs));
    memset(s_hasLocalInput, 0, sizeof(s_hasLocalInput));
    memset(s_remoteInputs, 0, sizeof(s_remoteInputs));
    memset(s_hasRemoteInput, 0, sizeof(s_hasRemoteInput));

    s_remoteLatestFrame = -1;
    s_peerAckedLocalFrame = -1;
    s_totalStalls = 0;
    s_consecutiveStalls = 0;
    s_packetsSent = 0;
    s_packetsReceived = 0;

    s_configLocked = false;
    memset(&s_lockedConfig, 0, sizeof(s_lockedConfig));
    s_sentMatchConfig = false;
    s_receivedMatchConfigAck = false;
    memset(&s_pendingConfig, 0, sizeof(s_pendingConfig));

    s_prevP1CharId  = 0xFFFFFFFF;
    s_prevP2CharId  = 0xFFFFFFFF;
    s_prevP1Palette = 0xFF;
    s_prevP2Palette = 0xFF;
    s_prevP1Confirm = 0xFF;
    s_prevP2Confirm = 0xFF;
    s_prevStageId = 0xFF;
    s_bothConfirmedSeen = false;
    s_lastLoggedMode = 0xFFFFFFFF;
    s_lastLoggedSubstate = 0xFFFFFFFF;
    s_localReady  = false;
    s_remoteReady = false;

    LOG_NET_INFO("[CharSel] BEGIN lockstep (isHost=%d localSlot=P%u remoteSlot=P%u session=0x%llX initialDelay=%d pinned=%d)",
                 isHost,
                 LocalSlot(),
                 RemoteSlot(),
                 sessionId,
                 s_inputDelay,
                 s_inputDelayPinned ? 1 : 0);
}

void End() {
    if (!s_active) return;
    LOG_NET_INFO("[CharSel] END (phase=%s, locked=%d, frames=%d, stalls=%u, sent=%u, recv=%u)",
                 PhaseToString(s_phase), s_configLocked,
                 s_localFrame, s_totalStalls, s_packetsSent, s_packetsReceived);
    s_active = false;
}

bool IsActive() { return s_active; }

// ============================================================================
// RTT / Input Delay
// ============================================================================

void SetRttMs(float rttMs) {
    s_rttMs = rttMs;
    if (!s_inputDelayLocked && !s_inputDelayPinned) {
        int newDelay = ComputeInputDelay(rttMs);
        if (newDelay != s_inputDelay) {
            LOG_NET_INFO("[CharSel] Input delay updated: %d -> %d frames (RTT=%.1fms)",
                         s_inputDelay, newDelay, rttMs);
            s_inputDelay = newDelay;
        }
    }
}

// ============================================================================
// Lockstep Input API
// ============================================================================

void BufferAndSendLocalInput(uint16_t rawInput) {
    if (!s_active) return;

    // Lock the input delay after the first input is produced.
    // If the host already selected a delay during the Ready handoff, pin that.
    if (!s_inputDelayLocked) {
        if (!s_inputDelayPinned) {
            s_inputDelay = ComputeInputDelay(s_rttMs);
        }
        s_inputDelayLocked = true;
        if (s_inputDelayPinned) {
            LOG_NET_INFO("[CharSel] Input delay locked at %d frames (host-selected)",
                         s_inputDelay);
        } else {
            LOG_NET_INFO("[CharSel] Input delay locked at %d frames (RTT=%.1fms)",
                         s_inputDelay, s_rttMs);
        }

        // Pre-fill frames 0..(inputDelay-1) with neutral input (0).
        // This ensures the buffer has local input for the first inputDelay
        // frames so lockstep doesn't immediately stall.
        for (int f = 0; f < s_inputDelay; f++) {
            int idx = f & kInputRingMask;
            s_localInputs[idx]  = 0;
            s_hasLocalInput[idx] = true;
        }
        s_localInputFrame = s_inputDelay;

        // Send the pre-filled neutral frames so the peer has them
        for (int f = 0; f < s_inputDelay; f++) {
            SendInputPacket(f);
        }
    }

    // Keep the local send window bounded to exactly the configured delay.
    // If the current lockstep frame stalls, do NOT keep queuing deeper future
    // inputs every visual frame. That causes runaway send-head growth,
    // queued extra inputs, and eventual ring-buffer overrun.
    const int32_t targetFrame = s_localFrame + s_inputDelay;

    // Prevent buffer overrun
    if (targetFrame - s_localFrame >= kInputRingSize) {
        LOG_NET_WARN("[CharSel] Input buffer overrun! target=%d local=%d", targetFrame, s_localFrame);
        return;
    }

    if (s_localInputFrame <= targetFrame) {
        int idx = targetFrame & kInputRingMask;
        s_localInputs[idx]   = rawInput;
        s_hasLocalInput[idx] = true;
        s_localInputFrame = targetFrame + 1;

        // Transition to Active on first real input
        if (s_phase == Phase::Enter) {
            s_phase = Phase::Active;
            LOG_NET_INFO("[CharSel] Phase: Enter -> Active (localSlot=P%u remoteSlot=P%u delay=%d)",
                         LocalSlot(), RemoteSlot(), s_inputDelay);
        }
    }

    // Send packet with redundant history
    SendInputPacket(targetFrame);
}

bool HasInputsForCurrentFrame() {
    if (!s_active) return false;
    int idx = s_localFrame & kInputRingMask;
    return s_hasLocalInput[idx] && s_hasRemoteInput[idx];
}

bool ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2) {
    if (!s_active || !outP1 || !outP2) return false;

    int idx = s_localFrame & kInputRingMask;
    if (!s_hasLocalInput[idx] || !s_hasRemoteInput[idx]) return false;

    uint16_t localInput  = s_localInputs[idx];
    uint16_t remoteInput = s_remoteInputs[idx];

    *outP1 = s_isHost ? localInput  : remoteInput;
    *outP2 = s_isHost ? remoteInput : localInput;

    if (s_localFrame < 12 || (s_localFrame % 60) == 0 || localInput != 0 || remoteInput != 0) {
        LOG_NET_DEBUG("[CharSel] Advance frame=%d local[P%u]=0x%04X remote[P%u]=0x%04X p1=0x%04X p2=0x%04X delay=%d remoteLatest=%d peerAck=%d",
                      s_localFrame,
                      LocalSlot(),
                      localInput,
                      RemoteSlot(),
                      remoteInput,
                      *outP1,
                      *outP2,
                      s_inputDelay,
                      s_remoteLatestFrame,
                      s_peerAckedLocalFrame);
    }

    {
        const uint32_t gameMode = GetGameMode();
        const uint32_t subState = GetSubstate();
        if (gameMode == MODE_CHARSEL &&
            (subState == CHARSEL_SUB_PREVIEW ||
             subState == CHARSEL_SUB_STAGE_INTRO ||
             subState == CHARSEL_SUB_CANCEL)) {
            const uint8_t stageId = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_STAGE_ID);
            const uint16_t sharedInput = (subState == CHARSEL_SUB_PREVIEW)
                ? CanonicalizeSharedStageInput(*outP1, *outP2)
                : 0;
            TestHarness::RecordStageEvent("LockstepConsume",
                                          *reinterpret_cast<uint32_t*>(ADDR_FRAME_COUNTER),
                                          s_localFrame,
                                          gameMode,
                                          subState,
                                          stageId,
                                          localInput,
                                          remoteInput,
                                          *outP1,
                                          *outP2,
                                          sharedInput,
                                          *outP1,
                                          *outP2);
        }
    }

    // Clear consumed slot
    s_hasLocalInput[idx]  = false;
    s_hasRemoteInput[idx] = false;

    if (s_consecutiveStalls > 0) {
        LOG_NET_DEBUG("[CharSel] Stall ended after %u frames (frame=%d)", s_consecutiveStalls, s_localFrame);
        s_consecutiveStalls = 0;
    }

    s_localFrame++;
    return true;
}

// ============================================================================
// Receive
// ============================================================================

void OnReceiveInput(const PacketCodec::CharSelInputPayload* payload) {
    if (!s_active) return;

    s_packetsReceived++;

    int32_t frame = (int32_t)payload->frame;
    int32_t ackFrame = (int32_t)payload->ack_frame;
    int     count = (int)payload->input_count;

    if (count < 1 || count > PacketCodec::kCharSelInputRedundancy) {
        LOG_NET_WARN("[CharSel] Invalid input_count=%d in packet (frame=%d)", count, frame);
        return;
    }

    if (frame >= (s_localFrame + kInputRingSize)) {
        LOG_NET_WARN("[CharSel] Ignoring implausible remote frame=%d local=%d ack=%u count=%d",
                     frame, s_localFrame, payload->ack_frame, count);
        return;
    }

    if (ackFrame > s_peerAckedLocalFrame) {
        s_peerAckedLocalFrame = ackFrame;
        if (s_packetsReceived <= 10 || (ackFrame % 60) == 0) {
            LOG_NET_DEBUG("[CharSel] Peer ack advanced: peer has local frame %d (remoteLatest=%d sendHead=%d)",
                          s_peerAckedLocalFrame,
                          s_remoteLatestFrame,
                          s_localInputFrame - 1);
        }
    }

    // Apply all inputs from the redundant history
    int newInputs = 0;
    for (int i = 0; i < count; i++) {
        int32_t f = frame - i;
        if (f < 0) continue;
        if (f < s_localFrame) continue;  // Already consumed, too old

        int idx = f & kInputRingMask;
        if (!s_hasRemoteInput[idx]) {
            s_remoteInputs[idx]   = payload->inputs[i];
            s_hasRemoteInput[idx] = true;
            newInputs++;
        }
    }

    // Track latest remote frame for ack
    if (frame > s_remoteLatestFrame) {
        s_remoteLatestFrame = frame;
    }

    if (newInputs > 0 && (s_packetsReceived <= 10 || (frame % 60) == 0)) {
        LOG_NET_DEBUG("[CharSel] Received %d new inputs from P%u (latest remote=%d, peerAck=%d, localFrame=%d)",
                     newInputs,
                     RemoteSlot(),
                     frame,
                     s_peerAckedLocalFrame,
                     s_localFrame);
    }
}

void OnReceiveMatchConfig(const PacketCodec::MatchConfigPayload* payload) {
    if (!s_active) return;

    LOG_NET_INFO("[CharSel] Received MatchConfig (hash=0x%08X)", payload->config_hash);

    // Verify hash
    PacketCodec::MatchConfigPayload copy = *payload;
    uint32_t computedHash = PacketCodec::HashMatchConfig(&copy);
    if (computedHash != payload->config_hash) {
        LOG_NET_ERROR("[CharSel] MatchConfig hash mismatch! received=0x%08X computed=0x%08X",
                      payload->config_hash, computedHash);
        PacketCodec::MatchConfigAckPayload ack = {};
        ack.config_hash = payload->config_hash;
        ack.accepted = 0;
        SessionManager::SendToPeer(PacketCodec::PacketType::MatchConfigAck,
            &ack, sizeof(ack), true);
        return;
    }

    // Accept
    PacketCodec::MatchConfigAckPayload ack = {};
    ack.config_hash = payload->config_hash;
    ack.accepted = 1;
    SessionManager::SendToPeer(PacketCodec::PacketType::MatchConfigAck,
        &ack, sizeof(ack), true);

    LOG_NET_INFO("[CharSel] Accepted MatchConfig, sent ACK");

    // Lock config (client side)
    s_lockedConfig.version_hash    = 0;
    s_lockedConfig.p1_character_id = payload->p1_character;
    s_lockedConfig.p2_character_id = payload->p2_character;
    s_lockedConfig.p1_palette_id   = payload->p1_palette;
    s_lockedConfig.p2_palette_id   = payload->p2_palette;
    s_lockedConfig.stage_id        = payload->stage_id;
    s_lockedConfig.round_count     = payload->round_count;
    s_lockedConfig.timer_setting   = payload->timer_setting;
    s_lockedConfig.gameplay_flags  = payload->gameplay_flags;
    s_lockedConfig.session_seed    = payload->session_seed;
    s_lockedConfig.config_hash     = payload->config_hash;

    s_configLocked = true;
    s_phase = Phase::Locked;
    // Persist for cursor sync on rematch re-entry
    s_lastConfirmedConfig = s_lockedConfig;
    s_hasLastConfirmedConfig = true;
    LOG_NET_INFO("[CharSel] CLIENT: CONFIG LOCKED (hash=0x%08X) — persisted P1_char=%u P2_char=%u stage=%u for cursor restore",
                 s_lockedConfig.config_hash,
                 s_lockedConfig.p1_character_id,
                 s_lockedConfig.p2_character_id,
                 s_lockedConfig.stage_id);
}

void OnReceiveMatchConfigAck(const PacketCodec::MatchConfigAckPayload* payload) {
    if (!s_active) return;

    LOG_NET_INFO("[CharSel] Received MatchConfigAck (hash=0x%08X accepted=%d)",
                 payload->config_hash, payload->accepted);

    if (payload->accepted) {
        s_receivedMatchConfigAck = true;

        if (s_isHost) {
            s_lockedConfig.version_hash    = 0;
            s_lockedConfig.p1_character_id = s_pendingConfig.p1_character;
            s_lockedConfig.p2_character_id = s_pendingConfig.p2_character;
            s_lockedConfig.p1_palette_id   = s_pendingConfig.p1_palette;
            s_lockedConfig.p2_palette_id   = s_pendingConfig.p2_palette;
            s_lockedConfig.stage_id        = s_pendingConfig.stage_id;
            s_lockedConfig.round_count     = s_pendingConfig.round_count;
            s_lockedConfig.timer_setting   = s_pendingConfig.timer_setting;
            s_lockedConfig.gameplay_flags  = s_pendingConfig.gameplay_flags;
            s_lockedConfig.session_seed    = s_pendingConfig.session_seed;
            s_lockedConfig.config_hash     = s_pendingConfig.config_hash;

            s_configLocked = true;
            s_phase = Phase::Locked;
            // Persist for cursor sync on rematch re-entry
            s_lastConfirmedConfig = s_lockedConfig;
            s_hasLastConfirmedConfig = true;
            LOG_NET_INFO("[CharSel] HOST: CONFIG LOCKED (hash=0x%08X) — persisted P1_char=%u P2_char=%u stage=%u for cursor restore",
                         s_lockedConfig.config_hash,
                         s_lockedConfig.p1_character_id,
                         s_lockedConfig.p2_character_id,
                         s_lockedConfig.stage_id);
        }
    } else {
        LOG_NET_ERROR("[CharSel] MatchConfig REJECTED by peer!");
    }
}

// ============================================================================
// Frame Update (called by SessionManager each frame)
// ============================================================================

void FrameUpdate() {
    if (!s_active) return;

    const uint32_t gameMode = GetGameMode();
    const uint32_t subState = GetSubstate();
    const uint32_t stageId = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_STAGE_ID);
    if (gameMode != s_lastLoggedMode || subState != s_lastLoggedSubstate) {
        LOG_NET_INFO("[CharSel] Game state: mode=%u sub=%u (%s) stage=%u phase=%s frame=%d delay=%d remoteLatest=%d peerAck=%d sendHead=%d localSlot=P%u remoteSlot=P%u",
                     gameMode,
                     subState,
                     CharSelSubstateToString(subState),
                     stageId,
                     PhaseToString(s_phase),
                     s_localFrame,
                     s_inputDelay,
                     s_remoteLatestFrame,
                     s_peerAckedLocalFrame,
                     s_localInputFrame - 1,
                     LocalSlot(),
                     RemoteSlot());
        s_lastLoggedMode = gameMode;
        s_lastLoggedSubstate = subState;
    }

    // Track stalls for diagnostics
    if (s_phase == Phase::Active && !HasInputsForCurrentFrame()) {
        const int idx = s_localFrame & kInputRingMask;
        const int haveLocal = s_hasLocalInput[idx] ? 1 : 0;
        const int haveRemote = s_hasRemoteInput[idx] ? 1 : 0;
        s_totalStalls++;
        s_consecutiveStalls++;
        if (s_consecutiveStalls > 0 && (s_consecutiveStalls % kStallLogInterval) == 0) {
            LOG_NET_WARN("[CharSel] Lockstep stall: %u consecutive frames waiting (localFrame=%d, remoteLatest=%d, peerAck=%d, sendHead=%d, curLocal=%d, curRemote=%d, mode=%u, sub=%u:%s)",
                         s_consecutiveStalls,
                         s_localFrame,
                         s_remoteLatestFrame,
                         s_peerAckedLocalFrame,
                         s_localInputFrame - 1,
                         haveLocal,
                         haveRemote,
                         gameMode,
                         subState,
                         CharSelSubstateToString(subState));
        }
        // Re-send latest input during stalls for reliability
        if (s_localInputFrame > 0) {
            SendInputPacket(s_localInputFrame - 1);
        }
    }

    // Monitor game state for confirm detection
    PollGameState();

    // Try to lock config if both ready
    TryLockConfig();
}

// ============================================================================
// Game State Monitoring (config lock detection)
// ============================================================================

static void PollGameState() {
    if (!s_active || s_configLocked) return;

    uint32_t p1Char = *reinterpret_cast<uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID);
    uint32_t p2Char = *reinterpret_cast<uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID);
    uint8_t  p1Pal  = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P1_PALETTE);
    uint8_t  p2Pal  = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P2_PALETTE);
    uint8_t  p1Conf = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P1_CONFIRM);
    uint8_t  p2Conf = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P2_CONFIRM);
    uint8_t  stageId = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_STAGE_ID);

    // Log changes (rate-limited: only on actual change)
    if (p1Char != s_prevP1CharId && s_prevP1CharId != 0xFFFFFFFF) {
        LOG_NET_INFO("[CharSel] P1 character changed: %u -> %u", s_prevP1CharId, p1Char);
    }
    if (p2Char != s_prevP2CharId && s_prevP2CharId != 0xFFFFFFFF) {
        LOG_NET_INFO("[CharSel] P2 character changed: %u -> %u", s_prevP2CharId, p2Char);
    }
    if (p1Pal != s_prevP1Palette && s_prevP1Palette != 0xFF) {
        LOG_NET_INFO("[CharSel] P1 palette changed: %u -> %u", s_prevP1Palette, p1Pal);
    }
    if (p2Pal != s_prevP2Palette && s_prevP2Palette != 0xFF) {
        LOG_NET_INFO("[CharSel] P2 palette changed: %u -> %u", s_prevP2Palette, p2Pal);
    }
    if (p1Conf != s_prevP1Confirm && s_prevP1Confirm != 0xFF) {
        LOG_NET_INFO("[CharSel] P1 confirm changed: %u -> %u", s_prevP1Confirm, p1Conf);
    }
    if (p2Conf != s_prevP2Confirm && s_prevP2Confirm != 0xFF) {
        LOG_NET_INFO("[CharSel] P2 confirm changed: %u -> %u", s_prevP2Confirm, p2Conf);
    }
    if (stageId != s_prevStageId && s_prevStageId != 0xFF) {
        LOG_NET_INFO("[CharSel] Stage ID changed: %u -> %u (frame=%d mode=%u sub=%u:%s)",
                     s_prevStageId,
                     stageId,
                     s_localFrame,
                     GetGameMode(),
                     GetSubstate(),
                     CharSelSubstateToString(GetSubstate()));
    }

    // Detect both confirmed
    if (p1Conf == 1 && p2Conf == 1 && !s_bothConfirmedSeen) {
        s_bothConfirmedSeen = true;
        LOG_NET_INFO("[CharSel] Both players confirmed! P1=char%u/pal%u P2=char%u/pal%u (frame=%d)",
                     p1Char, p1Pal, p2Char, p2Pal, s_localFrame);

        // NOTE: Don't set ready flags yet — stage select may follow.
        // Ready is set when MODE_STAGESEL (7) is entered, guaranteeing the stage ID is finalized.
    }

    // Detect transition to MODE_STAGESEL — stage selection is complete, stage ID is valid.
    // This covers both stage-select-ON (interactive grid) and stage-select-OFF (auto-pick).
    const uint32_t gameMode = GetGameMode();
    if (s_bothConfirmedSeen && !s_localReady && gameMode == MODE_STAGESEL) {
        uint32_t stage = *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_STAGE_ID);
        LOG_NET_INFO("[CharSel] Stage finalized (MODE_STAGESEL entered) — stage=%u (frame=%d)", stage, s_localFrame);
        s_localReady  = true;
        s_remoteReady = true;
    }

    s_prevP1CharId  = p1Char;
    s_prevP2CharId  = p2Char;
    s_prevP1Palette = p1Pal;
    s_prevP2Palette = p2Pal;
    s_prevP1Confirm = p1Conf;
    s_prevP2Confirm = p2Conf;
    s_prevStageId = stageId;
}

// ============================================================================
// Queries
// ============================================================================

Phase GetPhase() { return s_phase; }
bool IsConfigLocked() { return s_configLocked; }

bool GetLockedConfig(LockedMatchConfig* out) {
    if (!s_configLocked || !out) return false;
    *out = s_lockedConfig;
    return true;
}

bool GetLastConfirmedConfig(LockedMatchConfig* out) {
    if (!s_hasLastConfirmedConfig || !out) return false;
    *out = s_lastConfirmedConfig;
    return true;
}

bool RestoreCursorsFromLastConfig() {
    if (!s_hasLastConfirmedConfig) {
        LOG_NET_INFO("[CursorSync] First match entry — no previous config to restore, using game defaults");
        return false;
    }

    const LockedMatchConfig& cfg = s_lastConfirmedConfig;

    // ---- snapshot pre-restore state ----
    const uint32_t oldP1Char   = *reinterpret_cast<const uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID);
    const uint32_t oldP2Char   = *reinterpret_cast<const uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID);
    const uint8_t  oldP1Cursor = *reinterpret_cast<const uint8_t*>(ADDR_CHARSEL_P1_CURSOR);
    const uint8_t  oldP2Cursor = *reinterpret_cast<const uint8_t*>(ADDR_CHARSEL_P2_CURSOR);
    const uint8_t  oldStageId  = *reinterpret_cast<const uint8_t*>(ADDR_CHARSEL_STAGE_ID);
    const uint8_t  oldStageCur = *reinterpret_cast<const uint8_t*>(ADDR_STAGE_CURSOR);

    LOG_NET_INFO("[CursorSync] PRE-RESTORE: P1_char=%u P2_char=%u "
                 "P1_cursor=%u P2_cursor=%u stage_id=%u stage_cursor=%u",
                 oldP1Char, oldP2Char, oldP1Cursor, oldP2Cursor,
                 oldStageId, oldStageCur);

    // ---- write char IDs + stage (consumed by init if it hasn't run yet) ----
    *reinterpret_cast<uint32_t*>(ADDR_CHARSEL_P1_CHAR_ID) = cfg.p1_character_id;
    *reinterpret_cast<uint32_t*>(ADDR_CHARSEL_P2_CHAR_ID) = cfg.p2_character_id;
    *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_STAGE_ID)    = static_cast<uint8_t>(cfg.stage_id);

    // ---- reverse grid-table lookup: char ID → grid index ----
    const uint32_t* gridTable = reinterpret_cast<const uint32_t*>(ADDR_CHARSEL_GRID_TABLE);
    uint8_t p1Grid = 0;   // vanilla default for char-0
    uint8_t p2Grid = 2;   // vanilla default for char-1
    for (int i = 0; i < 21; ++i) {
        if (gridTable[i] == cfg.p1_character_id) p1Grid = static_cast<uint8_t>(i);
        if (gridTable[i] == cfg.p2_character_id) p2Grid = static_cast<uint8_t>(i);
    }

    // ---- write cursor positions directly (covers post-init case) ----
    *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P1_CURSOR) = p1Grid;
    *reinterpret_cast<uint8_t*>(ADDR_CHARSEL_P2_CURSOR) = p2Grid;
    *reinterpret_cast<uint8_t*>(ADDR_STAGE_CURSOR)      = static_cast<uint8_t>(cfg.stage_id);

    LOG_NET_INFO("[CursorSync] RESTORED: P1_char=%u->%u P2_char=%u->%u "
                 "P1_cursor=%u->%u P2_cursor=%u->%u stage=%u->%u "
                 "(hash=0x%08X)",
                 oldP1Char, cfg.p1_character_id,
                 oldP2Char, cfg.p2_character_id,
                 oldP1Cursor, p1Grid,
                 oldP2Cursor, p2Grid,
                 oldStageId, cfg.stage_id,
                 cfg.config_hash);
    return true;
}

void GetSnapshot(Snapshot* out) {
    if (!out) return;
    const uint32_t gameMode = GetGameMode();
    const uint32_t subState = GetSubstate();
    const int idx = s_localFrame & kInputRingMask;
    out->phase            = s_phase;
    out->local_frame      = s_localFrame;
    out->remote_confirmed = s_remoteLatestFrame;
    out->peer_acked_local = s_peerAckedLocalFrame;
    out->local_send_head  = s_localInputFrame - 1;
    out->input_delay      = s_inputDelay;
    out->stalls           = s_totalStalls;
    out->local_slot       = LocalSlot();
    out->remote_slot      = RemoteSlot();
    out->config_locked    = s_configLocked;
    out->config_hash      = s_lockedConfig.config_hash;

    snprintf(out->status, sizeof(out->status),
             "Phase=%s Frame=%d Remote=%d PeerAck=%d Send=%d Delay=%d Stalls=%u Cur[L=%d R=%d] Game[%u:%u] Slots[%u/%u] %s",
             PhaseToString(s_phase), s_localFrame, s_remoteLatestFrame,
             s_peerAckedLocalFrame,
             s_localInputFrame - 1,
             s_inputDelay, s_totalStalls,
             s_hasLocalInput[idx] ? 1 : 0,
             s_hasRemoteInput[idx] ? 1 : 0,
             gameMode,
             subState,
             LocalSlot(),
             RemoteSlot(),
             s_configLocked ? "LOCKED" : "");
}

} // namespace CharSelSync
