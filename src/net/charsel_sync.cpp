/**
 * Alice Senki 2 - Character Select / Stage Select Sync
 *
 * Reads local charsel state from game memory, sends to remote peer,
 * and writes remote peer's state into the opposite player slot.
 *
 * Side mapping:
 *   Host = local P1 addresses, remote controls P2
 *   Join = local P1 addresses (game sees us as P1 in VS_HUMAN),
 *          but we swap: our local input drives P2 on the host side
 *
 * For simplicity in the charsel phase, both peers run GAMETYPE_VS_HUMAN
 * and both control P1 locally. The remote peer's cursor/confirm is
 * written into P2 addresses. The host_side assignment in LockedMatchConfig
 * handles the final side mapping for gameplay.
 */

#include "net/charsel_sync.h"
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

namespace {

using namespace Net;

// ============================================================================
// Memory access helpers
// ============================================================================

static uint8_t ReadU8(uintptr_t a, uint8_t d = 0) {
    __try { return *(volatile uint8_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; }
}

static void WriteU8(uintptr_t a, uint8_t v) {
    __try { *(volatile uint8_t*)a = v; } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static uint32_t ReadU32(uintptr_t a, uint32_t d = 0) {
    __try { return *(volatile uint32_t*)a; } __except(EXCEPTION_EXECUTE_HANDLER) { return d; }
}

// ============================================================================
// Internal state
// ============================================================================

static bool     s_initialized        = false;
static bool     s_active             = false;
static bool     s_inStagePhase       = false;

// Local tracking
static uint8_t  s_localCursor        = 0;
static uint8_t  s_localConfirmed     = 0;
static uint8_t  s_localPalette       = 0;
static uint8_t  s_localCharId        = 0;

// Remote tracking
static uint8_t  s_remoteCursor       = 0;
static uint8_t  s_remoteConfirmed    = 0;
static uint8_t  s_remotePalette      = 0;
static uint8_t  s_remoteCharId       = 0;
static bool     s_remoteCharLocked   = false;

// Stage tracking
static uint8_t  s_localStage         = 0;
static bool     s_localStageLocked   = false;
static uint8_t  s_remoteStage        = 0;
static bool     s_remoteStageLocked  = false;
static uint8_t  s_finalStageId       = 0;

// Both-locked flags
static bool     s_bothCharsLocked    = false;
static bool     s_bothStageLocked    = false;

// Lock-sent tracking
static bool     s_localLockSent      = false;

// Throttle: only send input when it changes
static uint8_t  s_lastSentCursor     = 0xFF;
static uint8_t  s_lastSentConfirmed  = 0xFF;
static uint8_t  s_lastSentPalette    = 0xFF;

// ============================================================================
// Grid-to-character lookup
// ============================================================================

static uint8_t LookupCharId(uint8_t gridIndex) {
    if (gridIndex > 20) return 0;
    uint32_t charId = ReadU32(ADDR_CHARSEL_GRID_TABLE + gridIndex * 4, 0);
    return (uint8_t)(charId & 0xFF);
}

// ============================================================================
// Read local state from game memory
// ============================================================================

static void ReadLocalCharSelState() {
    // We always read P1 addresses — in GAMETYPE_VS_HUMAN the local player is P1
    s_localCursor    = ReadU8(ADDR_CHARSEL_P1_CURSOR, 0);
    s_localConfirmed = ReadU8(ADDR_CHARSEL_P1_CONFIRM, 0);
    s_localPalette   = ReadU8(ADDR_CHARSEL_P1_PALETTE, 0);

    if (s_localConfirmed) {
        s_localCharId = LookupCharId(s_localCursor);
    }
}

// ============================================================================
// Write remote state into P2 game memory
// ============================================================================

static void WriteRemoteCharSelState() {
    WriteU8(ADDR_CHARSEL_P2_CURSOR, s_remoteCursor);

    // Only write confirm if remote has locked
    if (s_remoteCharLocked) {
        WriteU8(ADDR_CHARSEL_P2_CONFIRM, 1);
    } else {
        WriteU8(ADDR_CHARSEL_P2_CONFIRM, s_remoteConfirmed);
    }
}

// ============================================================================
// Send local state to remote peer
// ============================================================================

static void SendLocalCharSelInput() {
    // Don't send if nothing changed
    if (s_localCursor == s_lastSentCursor &&
        s_localConfirmed == s_lastSentConfirmed &&
        s_localPalette == s_lastSentPalette) {
        return;
    }

    CharSelInputPayload payload{};
    payload.cursor = s_localCursor;
    payload.confirmed = s_localConfirmed;
    payload.palette = s_localPalette;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::CharSelInput,
                       &payload, sizeof(payload), true);

    s_lastSentCursor = s_localCursor;
    s_lastSentConfirmed = s_localConfirmed;
    s_lastSentPalette = s_localPalette;
}

static void SendCharSelLock() {
    CharSelLockPayload payload{};
    payload.character_id = s_localCharId;
    payload.palette = s_localPalette;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::CharSelLock,
                       &payload, sizeof(payload), true);

    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Sent CharSelLock: char=%u pal=%u",
        payload.character_id, payload.palette);
}

static void SendStageSync(uint8_t stageId, bool confirmed) {
    StageSyncPayload payload{};
    payload.stage_id = stageId;
    payload.confirmed = confirmed ? 1 : 0;

    Session_SendPacket(CHANNEL_CONTROL, PacketType::StageSync,
                       &payload, sizeof(payload), true);
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void CharSelSync_Init() {
    if (s_initialized) return;
    s_active = false;
    s_inStagePhase = false;
    s_initialized = true;
    LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Initialized");
}

void CharSelSync_Shutdown() {
    if (!s_initialized) return;
    s_active = false;
    s_initialized = false;
    LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Shutdown");
}

// Tracking for detailed before/after logging
static uint8_t s_prevLocalCursor    = 0xFF;
static uint8_t s_prevLocalConfirmed = 0xFF;
static uint8_t s_prevRemoteCursor   = 0xFF;
static uint8_t s_prevRemoteConfirmed= 0xFF;
static uint8_t s_prevLocalStage     = 0xFF;
static uint8_t s_prevRemoteStage    = 0xFF;

void CharSelSync_Begin() {
    s_active = true;
    s_inStagePhase = false;

    s_localCursor = 0;
    s_localConfirmed = 0;
    s_localPalette = 0;
    s_localCharId = 0;

    s_remoteCursor = 0;
    s_remoteConfirmed = 0;
    s_remotePalette = 0;
    s_remoteCharId = 0;
    s_remoteCharLocked = false;

    s_localStage = 0;
    s_localStageLocked = false;
    s_remoteStage = 0;
    s_remoteStageLocked = false;
    s_finalStageId = 0;
    s_bothCharsLocked = false;
    s_bothStageLocked = false;
    s_localLockSent = false;

    s_lastSentCursor = 0xFF;
    s_lastSentConfirmed = 0xFF;
    s_lastSentPalette = 0xFF;

    // Reset before/after tracking
    s_prevLocalCursor = 0xFF;
    s_prevLocalConfirmed = 0xFF;
    s_prevRemoteCursor = 0xFF;
    s_prevRemoteConfirmed = 0xFF;
    s_prevLocalStage = 0xFF;
    s_prevRemoteStage = 0xFF;

    Rollback::NetplayLog_Write("CHARSEL", -1, "=== CHARACTER SELECT STARTED ===");
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Begin");
}

void CharSelSync_BeginStagePhase() {
    s_inStagePhase = true;
    s_localStageLocked = false;
    s_remoteStageLocked = false;
    s_bothStageLocked = false;
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Stage phase begin");
}

void CharSelSync_Abort() {
    if (!s_active) return;
    s_active = false;
    s_inStagePhase = false;
    s_bothCharsLocked = false;
    s_bothStageLocked = false;
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Aborted");
}

void CharSelSync_FrameUpdate() {
    if (!s_active) return;

    uint32_t mode = GetGameMode();
    if (mode != MODE_CHARSEL) return;

    if (!s_inStagePhase) {
        // Character selection phase
        uint8_t prevCursor = s_localCursor;
        uint8_t prevConfirm = s_localConfirmed;

        ReadLocalCharSelState();
        SendLocalCharSelInput();
        WriteRemoteCharSelState();

        // Detailed cursor/confirm logging (before/after)
        if (s_localCursor != s_prevLocalCursor) {
            Rollback::NetplayLog_Write("CHARSEL", -1,
                "Local cursor: %u -> %u (charId=%u)",
                s_prevLocalCursor, s_localCursor, LookupCharId(s_localCursor));
            LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Local cursor: %u -> %u",
                s_prevLocalCursor, s_localCursor);
            s_prevLocalCursor = s_localCursor;
        }
        if (s_localConfirmed != s_prevLocalConfirmed) {
            Rollback::NetplayLog_Write("CHARSEL", -1,
                "Local confirm: %u -> %u (cursor=%u charId=%u palette=%u)",
                s_prevLocalConfirmed, s_localConfirmed,
                s_localCursor, s_localCharId, s_localPalette);
            LOG_NETPLAY(LOG_INFO, "[CharSelSync] Local confirm: %u (char=%u pal=%u)",
                s_localConfirmed, s_localCharId, s_localPalette);
            s_prevLocalConfirmed = s_localConfirmed;
        }

        // Check if local player just confirmed
        if (s_localConfirmed && !s_bothCharsLocked) {
            if (!s_localLockSent) {
                SendCharSelLock();
                s_localLockSent = true;
                Rollback::NetplayLog_Write("CHARSEL", -1,
                    "Local character LOCKED: char=%u palette=%u cursor=%u",
                    s_localCharId, s_localPalette, s_localCursor);
            }
        }

        // Check both locked
        if (s_localConfirmed && s_remoteCharLocked && !s_bothCharsLocked) {
            s_bothCharsLocked = true;
            Rollback::NetplayLog_Write("CHARSEL", -1,
                "=== BOTH CHARACTERS LOCKED: local=%u remote=%u ===",
                s_localCharId, s_remoteCharId);
            LOG_NETPLAY(LOG_INFO, "[CharSelSync] Both characters locked: local=%u remote=%u",
                s_localCharId, s_remoteCharId);
        }
    } else {
        // Stage selection phase
        uint32_t sub = GetSubstate();

        if (sub == CHARSEL_SUB_MATCHUP_COMMIT || sub == CHARSEL_SUB_STAGESEL_GRID) {
            uint8_t currentStage = ReadU8(ADDR_CHARSEL_STAGE_ID, 0);

            if (currentStage != s_localStage) {
                Rollback::NetplayLog_Write("STAGESEL", -1,
                    "Local stage cursor: %u -> %u (sub=%u)",
                    s_localStage, currentStage, sub);
                LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Stage cursor: %u -> %u",
                    s_localStage, currentStage);
                s_localStage = currentStage;
                SendStageSync(s_localStage, false);
            }

            if (sub == CHARSEL_SUB_MATCHUP_COMMIT) {
                uint8_t stageConfirm = ReadU8(ADDR_STAGE_CURSOR + 1, 0);
                if (stageConfirm && !s_localStageLocked) {
                    s_localStageLocked = true;
                    s_finalStageId = currentStage;
                    SendStageSync(currentStage, true);
                    Rollback::NetplayLog_Write("STAGESEL", -1,
                        "Local stage LOCKED: stage=%u", currentStage);
                    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Local stage locked: %u", currentStage);
                }
            }
        }

        // Check both stage locked
        if (s_localStageLocked && s_remoteStageLocked && !s_bothStageLocked) {
            SessionRole role = Session_GetRole();
            if (role == SessionRole::Host) {
                s_finalStageId = s_localStage;
            } else {
                s_finalStageId = s_remoteStage;
            }
            s_bothStageLocked = true;
            Rollback::NetplayLog_Write("STAGESEL", -1,
                "=== BOTH STAGES LOCKED: local=%u remote=%u final=%u (role=%s) ===",
                s_localStage, s_remoteStage, s_finalStageId,
                (role == SessionRole::Host) ? "Host" : "Join");
            LOG_NETPLAY(LOG_INFO, "[CharSelSync] Both stages locked: final=%u", s_finalStageId);
        }
    }
}

// ============================================================================
// Packet reception
// ============================================================================

void CharSelSync_OnRemoteInput(const CharSelInputPayload* p) {
    if (!s_active || !p) return;

    // Before/after logging for remote cursor changes
    if (p->cursor != s_prevRemoteCursor) {
        Rollback::NetplayLog_Write("CHARSEL", -1,
            "Remote cursor: %u -> %u (confirmed=%u palette=%u)",
            s_prevRemoteCursor, p->cursor, p->confirmed, p->palette);
        s_prevRemoteCursor = p->cursor;
    }
    if (p->confirmed != s_prevRemoteConfirmed) {
        Rollback::NetplayLog_Write("CHARSEL", -1,
            "Remote confirm: %u -> %u (cursor=%u palette=%u)",
            s_prevRemoteConfirmed, p->confirmed, p->cursor, p->palette);
        s_prevRemoteConfirmed = p->confirmed;
    }

    s_remoteCursor = p->cursor;
    s_remoteConfirmed = p->confirmed;
    s_remotePalette = p->palette;

    LOG_NETPLAY(LOG_DEBUG, "[CharSelSync] Remote input: cursor=%u confirmed=%u palette=%u",
        p->cursor, p->confirmed, p->palette);
}

void CharSelSync_OnRemoteLock(const CharSelLockPayload* p) {
    if (!s_active || !p) return;

    s_remoteCharId = p->character_id;
    s_remotePalette = p->palette;
    s_remoteCharLocked = true;
    s_remoteConfirmed = 1;

    Rollback::NetplayLog_Write("CHARSEL", -1,
        "Remote character LOCKED: char=%u palette=%u",
        p->character_id, p->palette);
    LOG_NETPLAY(LOG_INFO, "[CharSelSync] Remote character locked: char=%u palette=%u",
        p->character_id, p->palette);
}

void CharSelSync_OnRemoteStage(const StageSyncPayload* p) {
    if (!s_active || !p) return;

    if (p->stage_id != s_prevRemoteStage) {
        Rollback::NetplayLog_Write("STAGESEL", -1,
            "Remote stage cursor: %u -> %u (confirmed=%u)",
            s_prevRemoteStage, p->stage_id, p->confirmed);
        s_prevRemoteStage = p->stage_id;
    }

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
    out->local_cursor = s_localCursor;
    out->local_confirmed = s_localConfirmed;
    out->remote_cursor = s_remoteCursor;
    out->remote_confirmed = s_remoteConfirmed;
    out->both_characters_locked = s_bothCharsLocked;

    // Map to P1/P2: host is P1, join is P2
    SessionRole role = Session_GetRole();
    if (role == SessionRole::Host) {
        out->p1_character = s_localCharId;
        out->p1_palette = s_localPalette;
        out->p2_character = s_remoteCharId;
        out->p2_palette = s_remotePalette;
    } else {
        out->p1_character = s_remoteCharId;
        out->p1_palette = s_remotePalette;
        out->p2_character = s_localCharId;
        out->p2_palette = s_localPalette;
    }

    out->local_stage = s_localStage;
    out->local_stage_confirmed = s_localStageLocked;
    out->remote_stage = s_remoteStage;
    out->remote_stage_confirmed = s_remoteStageLocked;
    out->both_stage_locked = s_bothStageLocked;
    out->stage_id = s_finalStageId;
}

} // namespace Net
