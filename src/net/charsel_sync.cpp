/**
 * Alice Senki 2 - Character Select / Stage Select Sync
 *
 * Input-driven lockstep implementation.
 *
 * Source-of-truth: decompilation of sub_5625E0 (Input_TryGetNextFrame).
 * Reference: old mod implementation (input_sync_hooks.cpp / charsel_sync.cpp),
 *            CCCaster charsel lockstep pattern.
 *
 * Both peers exchange raw per-frame button inputs with redundant history.
 * The Hook_InputDispatcher (on sub_5625E0) feeds lockstep data into the
 * game's native charsel handler so both sides see identical cursor movement.
 *
 * Side assignment:
 *   Host  = P1 (left),  local input -> out[0]
 *   Join  = P2 (right), local input -> out[1]
 */

#include "net/charsel_sync.h"
#include "net/stagesel_sync.h"
#include "net/barrier_protocol.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/protocol.h"
#include "core/game_state.h"
#include "core/as2_constants.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include <math.h>

namespace {

using namespace Net;

// ============================================================================
// Constants
// ============================================================================

static const int RING_SIZE           = 512;
static const int RING_MASK           = RING_SIZE - 1;
static const int INPUT_REDUNDANCY    =
    (int)(sizeof(((CharSelFrameInputPayload*)0)->inputs) / sizeof(uint16_t));
static const int MIN_INPUT_DELAY     = 2;
static const int MAX_INPUT_DELAY     = 10;
static const int DEFAULT_INPUT_DELAY = 2;
static const int SEND_HEAD_BUFFER    = RING_SIZE / 2;   // Max send-ahead beyond consumeFrame
static const int LOCKSTEP_TIMEOUT_MS = 10000;            // 10s timeout (same as CCCaster)
static const int RESEND_INTERVAL_MS  = 100;              // Re-send latest input every 100ms while waiting

static_assert(INPUT_REDUNDANCY > 0, "CharSelFrameInputPayload must contain input history slots");

// ============================================================================
// Memory access helpers
// ============================================================================

static uint8_t ReadU8(uintptr_t a, uint8_t d = 0) {
    __try { return *(volatile uint8_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; }
}

static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) {
    __try { return *(volatile uint32_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; }
}

static bool IsStageSelBrowsingSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_STAGESEL_GRID ||
           substate == CHARSEL_SUB_STAGESEL_CONFIRM ||
           substate == CHARSEL_SUB_FADE_OUT;
}

static bool IsStageSelCommittedSubstate(uint32_t substate) {
    return substate == CHARSEL_SUB_MATCHUP_COMMIT ||
           substate == CHARSEL_SUB_TO_MATCH;
}

// ============================================================================
// Internal state
// ============================================================================

static bool     s_initialized        = false;
static bool     s_active             = false;
static bool     s_isHost             = false;

// --- Lockstep frame tracking ---
static uint32_t s_consumeFrame       = 0;     // Next frame to consume (game side)
static uint32_t s_localInputFrame    = 0;     // Next local frame to write (ahead by delay)
static uint32_t s_remoteLatestFrame  = 0;     // Latest remote frame we've received

// --- Ring buffers ---
static uint16_t s_localInputs[RING_SIZE];
static uint16_t s_remoteInputs[RING_SIZE];
static bool     s_hasLocalInput[RING_SIZE];
static bool     s_hasRemoteInput[RING_SIZE];

// --- Input delay ---
static int      s_inputDelay         = DEFAULT_INPUT_DELAY;
static bool     s_delayLocked        = false;

// --- Stage phase ---
static bool     s_inStagePhase       = false;

// --- Lockstep timeout / resend ---
static DWORD    s_lastRemoteInputTime = 0;   // GetTickCount() of last new remote input
static DWORD    s_lastResendTime      = 0;   // GetTickCount() of last resend
static DWORD    s_lastTargetedResendTime = 0; // GetTickCount() of last targeted resend burst
static bool     s_timedOut            = false;

// --- Confirm detection (polled from game memory) ---
static bool     s_bothCharsLocked    = false;
static bool     s_bothStageLocked    = false;
static bool     s_localCharConfirmed = false;
static bool     s_remoteCharLocked   = false;

// Cached character IDs — captured at lock time, safe from mode transitions
static uint8_t  s_localChar          = 0;
static uint8_t  s_localPalette       = 0;
static uint8_t  s_remoteChar         = 0;
static uint8_t  s_remotePalette      = 0;

// Stage tracking
static uint8_t  s_localStage         = 0;
static bool     s_localStageLocked   = false;
static uint8_t  s_remoteStage        = 0;
static bool     s_remoteStageLocked  = false;
static uint8_t  s_finalStageId       = 0;

// Lock packet sent tracking
static bool     s_localLockSent      = false;

// ============================================================================
// Grid-to-character lookup
// ============================================================================

static uint8_t LookupCharId(uint8_t gridIndex) {
    if (gridIndex > 20) return 0;
    uint32_t charId = ReadU32(ADDR_CHARSEL_GRID_TABLE + gridIndex * 4, 0);
    return (uint8_t)(charId & 0xFF);
}

// ============================================================================
// Send lockstep input packet (with redundant history)
// ============================================================================

static void SendFrameInputPacket(uint32_t frame) {
    CharSelFrameInputPayload payload{};
    payload.frame = frame;
    payload.ack_frame = s_consumeFrame;  // Tell remote what frame we need from them

    int count = 0;
    for (int i = 0; i < INPUT_REDUNDANCY; i++) {
        int f = (int)frame - i;
        if (f < 0) break;
        int idx = f & RING_MASK;
        if (!s_hasLocalInput[idx]) break;
        payload.inputs[i] = s_localInputs[idx];
        count++;
    }
    payload.input_count = (uint16_t)count;

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "SendFrameInputPacket: frame=%u ack=%u count=%u inputs=[0x%04X,0x%04X,0x%04X,0x%04X] consume=%u localFrame=%u remoteLatest=%u",
        payload.frame,
        payload.ack_frame,
        payload.input_count,
        payload.inputs[0],
        payload.inputs[1],
        payload.inputs[2],
        payload.inputs[3],
        s_consumeFrame,
        s_localInputFrame,
        s_remoteLatestFrame);
    Rollback::NetplayLog_Flush();

    const bool sent = BarrierProtocol_SendPacket(PacketType::CharSelFrameInput,
                                                 &payload, sizeof(payload));
    if (!sent) {
        Rollback::NetplayLog_Write("CHARSEL", -1,
            "SendFrameInputPacket FAILED: frame=%u ack=%u count=%u state-active=%d",
            payload.frame, payload.ack_frame, payload.input_count, s_active ? 1 : 0);
        Rollback::NetplayLog_Flush();
    }
}

// ============================================================================
// Send character lock notification (reliable, for pregame_sync)
// ============================================================================

static void SendCharSelLock() {
    // On Host, local is P1. On Join, local is P2.
    uintptr_t cursorAddr = s_isHost ? ADDR_CHARSEL_P1_CURSOR : ADDR_CHARSEL_P2_CURSOR;
    uintptr_t paletteAddr = s_isHost ? ADDR_CHARSEL_P1_PALETTE : ADDR_CHARSEL_P2_PALETTE;

    uint8_t cursor = ReadU8(cursorAddr, 0);
    uint8_t charId = LookupCharId(cursor);
    uint8_t palette = ReadU8(paletteAddr, 0);

    // Cache locally — game memory may be stale by the time snapshot is read
    s_localChar = charId;
    s_localPalette = palette;

    CharSelLockPayload payload{};
    payload.character_id = charId;
    payload.palette = palette;

    BarrierProtocol_SendPacket(PacketType::CharSelLock,
                              &payload, sizeof(payload));

    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Sent CharSelLock: char=%u pal=%u",
        charId, palette);
}

// ============================================================================
// Send stage sync (reliable)
// ============================================================================

static void SendStageSync(uint8_t stageId, bool confirmed) {
    StageSyncPayload payload{};
    payload.stage_id = stageId;
    payload.confirmed = confirmed ? 1 : 0;

    BarrierProtocol_SendPacket(PacketType::StageSync,
                              &payload, sizeof(payload));
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void CharSelSync_Init() {
    if (s_initialized) return;
    s_active = false;
    s_initialized = true;
    StageSelSync_Init();
    LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Initialized");
}

void CharSelSync_Shutdown() {
    if (!s_initialized) return;
    s_active = false;
    s_initialized = false;
    StageSelSync_Shutdown();
    LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Shutdown");
}

void CharSelSync_Begin() {
    s_active = true;
    s_isHost = (Session_GetRole() == SessionRole::Host);
    s_inStagePhase = false;

    // Reset lockstep state
    s_consumeFrame = 0;
    s_localInputFrame = 0;
    s_remoteLatestFrame = 0;
    s_inputDelay = DEFAULT_INPUT_DELAY;
    s_delayLocked = false;

    memset(s_localInputs, 0, sizeof(s_localInputs));
    memset(s_remoteInputs, 0, sizeof(s_remoteInputs));
    memset(s_hasLocalInput, 0, sizeof(s_hasLocalInput));
    memset(s_hasRemoteInput, 0, sizeof(s_hasRemoteInput));

    // Reset timeout / resend state
    s_lastRemoteInputTime = GetTickCount();
    s_lastResendTime = 0;
    s_lastTargetedResendTime = 0;
    s_timedOut = false;

    // Reset confirm/lock state
    s_bothCharsLocked = false;
    s_bothStageLocked = false;
    s_localCharConfirmed = false;
    s_remoteCharLocked = false;
    s_localLockSent = false;
    s_localChar = 0;
    s_localPalette = 0;
    s_remoteChar = 0;
    s_remotePalette = 0;

    s_localStage = 0;
    s_localStageLocked = false;
    s_remoteStage = 0;
    s_remoteStageLocked = false;
    s_finalStageId = 0;

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "=== CHARSEL LOCKSTEP BEGIN (role=%s delay=%d) ===",
        s_isHost ? "Host" : "Join", s_inputDelay);
    Rollback::NetplayLog_Flush();
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Begin (role=%s)",
        s_isHost ? "Host" : "Join");
}

void CharSelSync_BeginStagePhase() {
    s_inStagePhase = true;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_bothStageLocked = false;

    // ── Lockstep reset ──────────────────────────────────────────────
    // Both sides accumulated different consumeFrame totals during charsel
    // (host=1748, client=1732 in test logs — 16-frame drift).
    // After both characters lock, the remaining subs (4→5→6) are
    // non-interactive animation (96 frames). No input capture or
    // consumption happens until sub=7 (stage grid).
    // Resetting here gives both sides a clean, aligned start.
    uint32_t oldConsume = s_consumeFrame;
    uint32_t oldLocal   = s_localInputFrame;

    s_consumeFrame      = 0;
    s_localInputFrame   = 0;
    s_remoteLatestFrame = 0;
    s_delayLocked       = false;   // re-derive from current RTT

    memset(s_hasLocalInput,  0, sizeof(s_hasLocalInput));
    memset(s_hasRemoteInput, 0, sizeof(s_hasRemoteInput));

    s_lastRemoteInputTime    = GetTickCount();
    s_lastResendTime         = 0;
    s_lastTargetedResendTime = 0;

    // Activate the stage select input merge module
    StageSelSync_Begin();

    LOG_NETPLAY(LOG_INFO,
        "[CharSelSync] Stage phase begin (lockstep reset: consume %u->0, local %u->0)",
        oldConsume, oldLocal);
    Rollback::NetplayLog_Write("CHARSEL", -1,
        "BeginStagePhase: lockstep RESET consume=%u->0 localFrame=%u->0 delay will re-derive",
        oldConsume, oldLocal);
    Rollback::NetplayLog_Flush();
}

void CharSelSync_Abort() {
    if (!s_active) return;
    s_active = false;
    s_inStagePhase = false;
    s_bothCharsLocked = false;
    s_bothStageLocked = false;
    StageSelSync_Abort();
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Aborted");
}

// ============================================================================
// Lockstep input capture (called from Hook_InputProcess)
// ============================================================================

void CharSelSync_CaptureLocalInput(uint16_t packedInput) {
    if (!s_active) return;

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "CaptureLocalInput ENTER: input=0x%04X delayLocked=%d localFrame=%u consumeFrame=%u",
        packedInput, (int)s_delayLocked, s_localInputFrame, s_consumeFrame);
    Rollback::NetplayLog_Flush();

    // Initialize input delay on first capture
    if (!s_delayLocked) {
        s_delayLocked = true;

        // Compute delay from RTT
        ConnectionStats stats{};
        Session_GetStats(&stats);
        float rttMs = stats.rtt_ms;
        s_inputDelay = (int)ceilf(rttMs * 60.0f / 1000.0f / 2.0f) + 1;
        if (s_inputDelay < MIN_INPUT_DELAY) s_inputDelay = MIN_INPUT_DELAY;
        if (s_inputDelay > MAX_INPUT_DELAY) s_inputDelay = MAX_INPUT_DELAY;

        LOG_NETPLAY(LOG_INFO, "[CharSelSync] Input delay locked: %d frames (RTT=%.1fms)",
            s_inputDelay, rttMs);
        Rollback::NetplayLog_Write("CHARSEL", -1,
            "Input delay locked: %d frames (RTT=%.1f ms)", s_inputDelay, rttMs);
        Rollback::NetplayLog_Flush();

        // Pre-fill delay frames with neutral input (0) so lockstep can start
        for (int f = 0; f < s_inputDelay; f++) {
            int idx = f & RING_MASK;
            s_localInputs[idx] = 0;
            s_hasLocalInput[idx] = true;
        }
        s_localInputFrame = (uint32_t)s_inputDelay;

        Rollback::NetplayLog_Write("CHARSEL", -1,
            "Pre-filled %d delay frames, localInputFrame now=%u", s_inputDelay, s_localInputFrame);
        Rollback::NetplayLog_Flush();

        // Send pre-fill frames to peer so they also have our neutral inputs
        for (int f = 0; f < s_inputDelay; f++) {
            SendFrameInputPacket((uint32_t)f);
        }

        Rollback::NetplayLog_Write("CHARSEL", -1,
            "Sent %d pre-fill packets to peer", s_inputDelay);
        Rollback::NetplayLog_Flush();
    }

    // Send-then-wait pattern (CCCaster style):
    const uint32_t maxSendFrame = s_consumeFrame + (uint32_t)SEND_HEAD_BUFFER;

    if (s_localInputFrame > maxSendFrame) {
        Rollback::NetplayLog_Write("CHARSEL", -1,
            "CaptureLocalInput: CAPPED at maxSendFrame=%u (localInputFrame=%u consumeFrame=%u)",
            maxSendFrame, s_localInputFrame, s_consumeFrame);
        Rollback::NetplayLog_Flush();
        if (s_localInputFrame > 0) {
            SendFrameInputPacket(s_localInputFrame - 1);
        }
        return;
    }

    // Store this frame's input at localInputFrame (always advances)
    uint32_t targetFrame = s_localInputFrame;
    int idx = (int)(targetFrame & RING_MASK);
    s_localInputs[idx] = packedInput;
    s_hasLocalInput[idx] = true;

    // Send to peer
    SendFrameInputPacket(targetFrame);

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "CaptureLocalInput: stored+sent frame=%u idx=%d input=0x%04X (consume=%u remoteLatest=%u delay=%d)",
        targetFrame, idx, packedInput, s_consumeFrame, s_remoteLatestFrame, s_inputDelay);
    Rollback::NetplayLog_Flush();

    s_localInputFrame = targetFrame + 1;
}

// ============================================================================
// Lockstep frame queries (called from Hook_InputDispatcher)
// ============================================================================

bool CharSelSync_IsLockstepActive() {
    return s_active;
}

bool CharSelSync_HasInputsForCurrentFrame() {
    if (!s_active) return false;
    int idx = (int)(s_consumeFrame & RING_MASK);
    bool hasLocal = s_hasLocalInput[idx];
    bool hasRemote = s_hasRemoteInput[idx];
    bool ready = hasLocal && hasRemote;

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "HasInputs: consumeFrame=%u idx=%d hasLocal=%d hasRemote=%d ready=%d",
        s_consumeFrame, idx, (int)hasLocal, (int)hasRemote, (int)ready);
    Rollback::NetplayLog_Flush();

    if (!ready && !s_timedOut) {
        DWORD now = GetTickCount();

        // Timeout: if we haven't received a NEW remote input in LOCKSTEP_TIMEOUT_MS, disconnect
        if ((now - s_lastRemoteInputTime) > (DWORD)LOCKSTEP_TIMEOUT_MS) {
            s_timedOut = true;
            LOG_NETPLAY(LOG_ERROR, "[CharSelSync] TIMEOUT: no remote input for %d ms — cancelling session",
                LOCKSTEP_TIMEOUT_MS);
            Rollback::NetplayLog_Write("CHARSEL", -1,
                "TIMEOUT: no remote input for %d ms (consume=%u remoteLatest=%u)",
                LOCKSTEP_TIMEOUT_MS, s_consumeFrame, s_remoteLatestFrame);
            Rollback::NetplayLog_Flush();
            Net::Session_Cancel();
            return false;
        }

        // Periodic resend: while waiting for remote, re-send our latest input
        // every RESEND_INTERVAL_MS for reliability against packet loss
        if (s_localInputFrame > 0 && (now - s_lastResendTime) >= (DWORD)RESEND_INTERVAL_MS) {
            s_lastResendTime = now;
            SendFrameInputPacket(s_localInputFrame - 1);
        }
    }

    return ready;
}

bool CharSelSync_ConsumeCurrentFrame(uint16_t* outP1, uint16_t* outP2) {
    if (!s_active || !outP1 || !outP2) return false;

    int idx = (int)(s_consumeFrame & RING_MASK);
    uint16_t localInput  = s_localInputs[idx];
    uint16_t remoteInput = s_remoteInputs[idx];

    // Side assignment: Host = P1, Join = P2
    if (s_isHost) {
        *outP1 = localInput;    // Host's input -> P1
        *outP2 = remoteInput;   // Join's input -> P2
    } else {
        *outP1 = remoteInput;   // Host's input -> P1
        *outP2 = localInput;    // Join's input -> P2
    }

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "ConsumeFrame: frame=%u idx=%d local=0x%04X remote=0x%04X -> P1=0x%04X P2=0x%04X (host=%d)",
        s_consumeFrame, idx, localInput, remoteInput, *outP1, *outP2, (int)s_isHost);
    Rollback::NetplayLog_Flush();

    // Clear consumed slot for reuse
    s_hasLocalInput[idx] = false;
    s_hasRemoteInput[idx] = false;

    s_consumeFrame++;
    return true;
}

// ============================================================================
// Per-frame update (called from pregame_sync via ModOnFrame)
// ============================================================================

void CharSelSync_FrameUpdate() {
    if (!s_active) return;

    uint32_t mode = GetGameMode();

    // Only poll game memory when actually in charsel mode
    if (mode == MODE_CHARSEL) {
        uint8_t p1Confirm = ReadU8(ADDR_CHARSEL_P1_CONFIRM, 0);
        uint8_t p2Confirm = ReadU8(ADDR_CHARSEL_P2_CONFIRM, 0);

        // Detect local character confirmation (P1 for Host, P2 for Join)
        if (s_isHost) {
            if (p1Confirm && !s_localCharConfirmed) {
                s_localCharConfirmed = true;
                Rollback::NetplayLog_Write("CHARSEL", -1,
                    "Local (Host/P1) character confirmed: cursor=%u char=%u",
                    ReadU8(ADDR_CHARSEL_P1_CURSOR, 0),
                    LookupCharId(ReadU8(ADDR_CHARSEL_P1_CURSOR, 0)));
            }
        } else {
            if (p2Confirm && !s_localCharConfirmed) {
                s_localCharConfirmed = true;
                Rollback::NetplayLog_Write("CHARSEL", -1,
                    "Local (Join/P2) character confirmed: cursor=%u char=%u",
                    ReadU8(ADDR_CHARSEL_P2_CURSOR, 0),
                    LookupCharId(ReadU8(ADDR_CHARSEL_P2_CURSOR, 0)));
            }
        }

        // Send lock packet when local character is confirmed
        if (s_localCharConfirmed && !s_localLockSent) {
            SendCharSelLock();
            s_localLockSent = true;
        }

        // Stage phase tracking
        if (s_inStagePhase) {
            uint32_t sub = GetSubstate();
            if (IsStageSelBrowsingSubstate(sub)) {
                uint8_t liveStage = ReadU8(ADDR_STAGE_CURSOR, s_localStage);
                if (liveStage != s_localStage) {
                    s_localStage = liveStage;
                    SendStageSync(s_localStage, false);
                }
            }

            if (IsStageSelCommittedSubstate(sub)) {
                uint8_t committedStage = ReadU8(ADDR_CHARSEL_STAGE_ID, s_localStage);
                if (committedStage != s_localStage) {
                    s_localStage = committedStage;
                    SendStageSync(s_localStage, false);
                }

                if (!s_localStageLocked) {
                    s_localStageLocked = true;
                    s_finalStageId = committedStage;
                    SendStageSync(committedStage, true);
                    Rollback::NetplayLog_Write("STAGESEL", -1,
                        "Local stage LOCKED: stage=%u sub=%u", committedStage, sub);
                    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Local stage locked: %u (sub=%u)",
                        committedStage, sub);
                }
            }
        }
    }

    // Both-locked checks run every frame regardless of game mode
    // (remote lock packets may arrive after mode transition)

    if (s_localCharConfirmed && s_remoteCharLocked && !s_bothCharsLocked) {
        s_bothCharsLocked = true;
        // Use cached values — game memory may already be stale
        uint8_t p1Char = s_isHost ? s_localChar : s_remoteChar;
        uint8_t p2Char = s_isHost ? s_remoteChar : s_localChar;
        Rollback::NetplayLog_Write("CHARSEL", -1,
            "=== BOTH CHARACTERS LOCKED: P1=%u P2=%u (local=%u remote=%u) ===",
            p1Char, p2Char, s_localChar, s_remoteChar);
        LOG_NETPLAY(LOG_INFO, "[CharSelSync] Both characters locked: P1=%u P2=%u",
            p1Char, p2Char);
    }

    if (s_localStageLocked && s_remoteStageLocked && !s_bothStageLocked) {
        if (s_isHost) {
            s_finalStageId = s_localStage;
        } else {
            s_finalStageId = s_remoteStage;
        }
        s_bothStageLocked = true;
        Rollback::NetplayLog_Write("STAGESEL", -1,
            "=== BOTH STAGES LOCKED: local=%u remote=%u final=%u ===",
            s_localStage, s_remoteStage, s_finalStageId);
        LOG_NETPLAY(LOG_INFO, "[CharSelSync] Both stages locked: final=%u",
            s_finalStageId);
    }
}

// ============================================================================
// Packet reception
// ============================================================================

void CharSelSync_OnRemoteFrameInput(const CharSelFrameInputPayload* p) {
    if (!s_active || !p) return;

    uint32_t frame = p->frame;
    int count = (int)p->input_count;
    if (count < 1) count = 1;
    if (count > INPUT_REDUNDANCY) count = INPUT_REDUNDANCY;

    bool isFirst = (s_remoteLatestFrame == 0 && frame > 0);
    int newFramesApplied = 0;

    // Apply all inputs from redundant history (newest to oldest)
    for (int i = 0; i < count; i++) {
        int f = (int)frame - i;
        if (f < 0) continue;
        if ((uint32_t)f < s_consumeFrame) continue; // Already consumed

        int idx = f & RING_MASK;
        if (!s_hasRemoteInput[idx]) {
            s_remoteInputs[idx] = p->inputs[i];
            s_hasRemoteInput[idx] = true;
            newFramesApplied++;
        }
    }

    // Track latest remote frame
    if (frame > s_remoteLatestFrame) {
        s_remoteLatestFrame = frame;
    }

    // Reset timeout timer whenever we receive new data
    if (newFramesApplied > 0) {
        s_lastRemoteInputTime = GetTickCount();
    }

    // Log every remote frame during debug
    Rollback::NetplayLog_Write("CHARSEL", -1,
        "OnRemoteInput: frame=%u ack=%u count=%d new=%d remoteLatest=%u consumeFrame=%u inputs=[0x%04X,0x%04X,0x%04X,0x%04X]",
        frame,
        p->ack_frame,
        count,
        newFramesApplied,
        s_remoteLatestFrame,
        s_consumeFrame,
        p->inputs[0],
        p->inputs[1],
        p->inputs[2],
        p->inputs[3]);
    Rollback::NetplayLog_Flush();

    if (isFirst) {
        LOG_NETPLAY(LOG_INFO, "[CharSelSync] First remote frame received: frame=%u count=%d input=0x%04X",
            frame, count, p->inputs[0]);
    }

    // Targeted resend: if remote is stuck waiting for frames we already sent,
    // resend a burst covering their consumeFrame (carried in ack_frame).
    // This recovers from dropped packets beyond the redundancy window.
    uint32_t remoteConsume = p->ack_frame;
    if (remoteConsume < s_localInputFrame && 
        (s_localInputFrame - remoteConsume) > (uint32_t)INPUT_REDUNDANCY) {
        DWORD now = GetTickCount();
        if ((now - s_lastTargetedResendTime) >= (DWORD)RESEND_INTERVAL_MS) {
            s_lastTargetedResendTime = now;
            // Send one packet whose redundancy window covers remoteConsume
            uint32_t resendFrame = remoteConsume + (uint32_t)INPUT_REDUNDANCY - 1;
            if (resendFrame >= s_localInputFrame)
                resendFrame = s_localInputFrame - 1;
            if (s_hasLocalInput[resendFrame & RING_MASK]) {
                Rollback::NetplayLog_Write("CHARSEL", -1,
                    "Targeted resend: remote stuck at consume=%u, resending frame=%u (our local=%u)",
                    remoteConsume, resendFrame, s_localInputFrame);
                Rollback::NetplayLog_Flush();
                SendFrameInputPacket(resendFrame);
            }
        }
    }
}

void CharSelSync_OnRemoteLock(const CharSelLockPayload* p) {
    if (!s_active || !p) return;

    s_remoteCharLocked = true;
    s_remoteChar = p->character_id;
    s_remotePalette = p->palette;

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "Remote character LOCKED: char=%u palette=%u",
        p->character_id, p->palette);
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Remote character locked: char=%u palette=%u",
        p->character_id, p->palette);
}

void CharSelSync_OnRemoteStage(const StageSyncPayload* p) {
    if (!s_active || !p) return;

    s_remoteStage = p->stage_id;
    if (p->confirmed) {
        s_remoteStageLocked = true;
        Rollback::NetplayLog_Write("STAGESEL", -1,
            "Remote stage LOCKED: stage=%u", p->stage_id);
        LOG_NETPLAY(LOG_INFO, "[CharSelSync] Remote stage locked: %u", p->stage_id);
    }
}

// ============================================================================
// Snapshot
// ============================================================================

void CharSelSync_GetSnapshot(CharSelSyncSnapshot* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->active = s_active;
    out->in_stage_phase = s_inStagePhase;

    // Read current game state for snapshot
    out->local_cursor = ReadU8(ADDR_CHARSEL_P1_CURSOR, 0);
    out->local_confirmed = ReadU8(ADDR_CHARSEL_P1_CONFIRM, 0);
    out->remote_cursor = ReadU8(ADDR_CHARSEL_P2_CURSOR, 0);
    out->remote_confirmed = ReadU8(ADDR_CHARSEL_P2_CONFIRM, 0);
    out->both_characters_locked = s_bothCharsLocked;

    // P1/P2 character IDs from cached lock-time values (game memory may be
    // stale if the game transitioned out of charsel before this snapshot).
    // Host controls P1 (local), Join controls P2 (local).
    if (s_isHost) {
        out->p1_character = s_localChar;
        out->p1_palette   = s_localPalette;
        out->p2_character = s_remoteChar;
        out->p2_palette   = s_remotePalette;
    } else {
        out->p1_character = s_remoteChar;
        out->p1_palette   = s_remotePalette;
        out->p2_character = s_localChar;
        out->p2_palette   = s_localPalette;
    }

    // Stage
    out->local_stage = s_localStage;
    out->local_stage_confirmed = s_localStageLocked;
    out->remote_stage = s_remoteStage;
    out->remote_stage_confirmed = s_remoteStageLocked;
    out->both_stage_locked = s_bothStageLocked;
    out->stage_id = s_finalStageId;

    // Diagnostics
    out->lockstep_frame = s_consumeFrame;
    out->local_input_frame = s_localInputFrame;
    out->input_delay = s_inputDelay;
}

} // namespace Net
