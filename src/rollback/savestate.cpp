/**
 * Alice Senki 2 - Manual Savestate System
 *
 * Conservative broad-capture approach:
 *   - Full contiguous region from ADDR_MATCH_BASE through P2 entity end
 *   - Scattered globals: RNG seed, frame counter, mode/substate, input buffers,
 *     per-frame temp scratch, match phase timer, pre-match gap
 *
 * This is NOT the live rollback history ring. Manual F5/F6 uses one slot,
 * and rollback bootstrap baseline handoff uses a separate netplay-only slot.
 *
 * Verified state region (from determinism system):
 *   Start: ADDR_MATCH_BASE       = 0x76C5F8
 *   End:   ADDR_P2_ENTITY_BASE + ENTITY_SIZE = 0x7AB880
 *   Size:  0x3F288 = 259,720 bytes (~254 KB)
 *
 * This region contains:
 *   - Match header (16 bytes)
 *   - Match context: camera scroll, screen shake, weather particles (7456 bytes)
 *   - Effect array: 200 slots × 32 bytes (6400 bytes)
 *   - Summon array: 100 slots × 272 bytes (27200 bytes)
 *   - P1 entity: 108,812 bytes (HP, meter, position, action, animation,
 *     combo, hitstun, knockdown, recovery, guard, state blocks A/B, combat)
 *   - P2 entity: 108,812 bytes (same)
 */

#include "rollback/savestate.h"
#include "rollback/determinism_verify.h"
#include "rollback/game_snapshot.h"
#include "rollback/block_digest.h"
#include "rollback/rollback_session.h"
#include "training/frame_advantage.h"
#include "training/input_macro.h"
#include "training/practice_tools.h"
#include "net/session_manager.h"
#include "net/spectator_playback.h"
#include "as2_constants.h"
#include "patches/memory_utils.h"
#include "log_window.h"
#include "imgui.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>
#include <xmmintrin.h>

// ============================================================================
// Savestate Storage
// ============================================================================

struct SavestateSlot {
    // Metadata
    SavestateInfo info;

    Rollback::GameSnapshot snapshot;

    bool practice_control_swap;
    PracticeToolsRuntimeState practice_runtime;

    // FPU state — captured for diagnostics but NOT restored by default.
    // Uncomment the restore lines in Savestate_Load if desync evidence
    // points to FPU drift being the cause.
    uint16_t fpu_cw;
    uint32_t fpu_mxcsr;
};

static SavestateSlot g_manualSlot;
static SavestateSlot g_roundStartSlot;
static SavestateSlot g_rollbackBaselineSlot;
static bool g_offlineMatchContextActive = false;
static bool g_roundStartAutosaveArmed = false;

// Edge detection for F5/F6
static bool g_f5WasDown = false;
static bool g_f6WasDown = false;

// ============================================================================
// File Logging (integrates with determinism log directory)
// ============================================================================

static char  g_logDir[MAX_PATH] = {0};
static FILE* g_logFile = nullptr;

void Savestate_SetLogDir(const char* dir) {
    if (dir) {
        strncpy(g_logDir, dir, MAX_PATH - 1);
        g_logDir[MAX_PATH - 1] = '\0';
    }
}

static void EnsureLogFile() {
    if (g_logFile) return;

    char path[MAX_PATH];
    if (g_logDir[0]) {
        CreateDirectoryA(g_logDir, NULL);
        snprintf(path, sizeof(path), "%s\\savestate_log.txt", g_logDir);
    } else {
        CreateDirectoryA("logs", NULL);
        snprintf(path, sizeof(path), "logs\\savestate_log.txt");
    }

    g_logFile = fopen(path, "w");
    if (g_logFile) {
        fprintf(g_logFile, "# Alice Senki 2 - Savestate Log\n");
        fprintf(g_logFile, "# Build: %s %s\n", __DATE__, __TIME__);
        fprintf(g_logFile, "# Main region: 0x%08X size=%u (%u KB)\n",
                ADDR_MATCH_BASE,
                (unsigned)Rollback::GAME_SNAPSHOT_MAIN_SIZE,
                (unsigned)(Rollback::GAME_SNAPSHOT_MAIN_SIZE / 1024));
        fprintf(g_logFile, "#\n");
        fflush(g_logFile);
    }
}

static void LogToFile(const char* fmt, ...) {
    EnsureLogFile();
    if (!g_logFile) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

// ============================================================================
// FPU Capture (diagnostic only)
// ============================================================================

static inline uint16_t CaptureX87CW() {
    uint16_t cw = 0;
    __asm { fnstcw word ptr [cw] }
    return cw;
}

static inline uint32_t CaptureMXCSR() {
    return _mm_getcsr();
}

// ============================================================================
// State Region Checksum
// ============================================================================

static uint32_t ComputeMainChecksum() {
    __try {
        struct ChecksumParts {
            uint32_t main_crc;
            uint32_t effect_index;
        } parts{};

        // Skips the bytes GameSnapshot_Restore deliberately leaves alone
        // (render-owned display timers). Comparing a captured checksum against
        // live memory without that skip reported a CHECKSUM MISMATCH error on
        // every baseline restore -- a false alarm, but one that made a clean
        // acceptance log look broken.
        parts.main_crc = Rollback::GameSnapshot_MainFingerprintSkippingExcluded(
            (const uint8_t*)ADDR_MATCH_BASE);
        parts.effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
        // Must be the SAME combine SnapshotChecksum uses, or the comparison is
        // between two different hash functions and can never match. That is
        // what the "BASELINE RESTORED with CHECKSUM MISMATCH" error actually
        // was: the capture side moved to the four-lane fingerprint while this
        // live side still finished with CalcCRC32.
        return Rollback::StateFingerprint32(&parts, sizeof(parts));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADDEAD;
    }
}

// ============================================================================
// Context helpers
// ============================================================================

static void ClearSavestateSlot(SavestateSlot* slot,
                               const char* slotLabel,
                               const char* reason) {
    if (!slot) {
        return;
    }

    const bool hadState = slot->info.valid;

    memset(slot, 0, sizeof(*slot));
    Rollback::GameSnapshot_Clear(&slot->snapshot);

    if (hadState) {
        LOG_INFO("[Savestate] Cleared %s slot: %s",
                 slotLabel ? slotLabel : "savestate",
                 reason ? reason : "unknown");
        LogToFile("CLEAR slot=%s reason=%s\n",
                  slotLabel ? slotLabel : "savestate",
                  reason ? reason : "unknown");
    }
}

static void ClearManualSavestateSlot(const char* reason) {
    ClearSavestateSlot(&g_manualSlot, "manual", reason);
}

static void ClearRoundStartSavestateSlot(const char* reason) {
    ClearSavestateSlot(&g_roundStartSlot, "round_start", reason);
}

static void ClearRollbackBaselineSlot(const char* reason) {
    ClearSavestateSlot(&g_rollbackBaselineSlot, "rollback_baseline", reason);
}

static bool CapturePracticeSavestateSlot(SavestateSlot* slot,
                                         const char* slotLabel) {
    if (!slot) {
        return false;
    }

    if (!Rollback::GameSnapshot_Capture(
            &slot->snapshot,
            (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER))) {
        LOG_ERROR("[Savestate] %s capture FAILED — snapshot capture rejected",
                  slotLabel ? slotLabel : "savestate");
        return false;
    }

    slot->fpu_cw = CaptureX87CW();
    slot->fpu_mxcsr = CaptureMXCSR();

    slot->info.valid = true;
    slot->info.frame = slot->snapshot.sim_frame;
    slot->info.checksum = slot->snapshot.checksum;
    slot->info.rng_seed = slot->snapshot.rng_seed;
    slot->info.game_mode = slot->snapshot.game_mode;
    slot->info.substate = slot->snapshot.substate;
    slot->practice_control_swap = PracticeTools_IsControlSwapped();
    PracticeTools_CaptureRuntimeState(&slot->practice_runtime);
    return true;
}

static bool RestorePracticeSavestateSlot(const SavestateSlot* slot,
                                         const char* slotLabel) {
    if (!slot || !slot->info.valid) {
        LOG_WARN("[Savestate] Cannot load %s — no savestate exists.",
                 slotLabel ? slotLabel : "savestate");
        return false;
    }

    if (!Savestate_CanSaveLoad()) {
        LOG_WARN("[Savestate] Cannot load %s — offline playable match required (mode=%d sub=%d type=%d net=%d rb=%d)",
                 slotLabel ? slotLabel : "savestate",
                 ReadMemory<uint32_t>(ADDR_GAME_MODE),
                 ReadMemory<uint32_t>(ADDR_SUB_STATE),
                 ReadMemory<uint32_t>(ADDR_GAME_TYPE),
                 Net::Session_IsConnected() ? 1 : 0,
                 Rollback::RollbackSession_IsActive() ? 1 : 0);
        return false;
    }

    uint32_t preFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    uint32_t preRng = DetVer_GetRngSeed();
    uint32_t preChecksum = ComputeMainChecksum();

    LOG_INFO("[Savestate] LOADING %s — restoring frame %d (current frame %d, swap=%d)",
             slotLabel ? slotLabel : "savestate",
             slot->info.frame,
             preFrame,
             slot->practice_control_swap ? 1 : 0);

    if (!Rollback::GameSnapshot_Restore(&slot->snapshot)) {
        LOG_ERROR("[Savestate] %s restore FAILED — snapshot restore rejected",
                  slotLabel ? slotLabel : "savestate");
        return false;
    }

    FrameAdvantage_CancelCalculation();
    InputMacro_OnSavestateLoad();
    PracticeTools_ApplyControlSwapState(slot->practice_control_swap);
    PracticeTools_RestoreRuntimeState(&slot->practice_runtime);
    PracticeTools_SyncControlSwapState();

    uint32_t postChecksum = ComputeMainChecksum();
    uint32_t postRng = DetVer_GetRngSeed();
    uint32_t postFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    const bool checksumMatch = (postChecksum == slot->info.checksum);

    if (checksumMatch) {
        LOG_INFO("[Savestate] LOADED %s OK — frame %d->%d checksum=0x%08X (match) rng=0x%08X paused=%d step=%u",
                 slotLabel ? slotLabel : "savestate",
                 preFrame,
                 postFrame,
                 postChecksum,
                 postRng,
                 slot->practice_runtime.paused ? 1 : 0,
                 slot->practice_runtime.stepCounter);
    } else {
        LOG_ERROR("[Savestate] LOADED %s with CHECKSUM MISMATCH! expected=0x%08X got=0x%08X",
                  slotLabel ? slotLabel : "savestate",
                  slot->info.checksum,
                  postChecksum);
    }

    LogToFile("LOAD slot=%s pre_frame=%d pre_checksum=0x%08X pre_rng=0x%08X\n",
              slotLabel ? slotLabel : "savestate",
              preFrame,
              preChecksum,
              preRng);
    LogToFile("     restored_frame=%d post_checksum=0x%08X rng=0x%08X match=%s\n",
              postFrame,
              postChecksum,
              postRng,
              checksumMatch ? "YES" : "NO");

    return true;
}

static bool IsOfflineSavestateGameType(uint32_t gameType) {
    switch (gameType) {
        case GAMETYPE_ARCADE:
        case GAMETYPE_VS_CPU:
        case GAMETYPE_VS_HUMAN:
        case GAMETYPE_TRAINING:
            return true;
        default:
            return false;
    }
}

static bool IsOnlineOwnedContext() {
    if (Net::Session_IsConnected()) {
        return true;
    }

    // re0.7 M4 guard adapter (inventory §7 / plan M4 task 4): the engine
    // facade is the session-active authority; gameplay_bridge is retired
    // with the M6 cutover.
    if (Rollback::RollbackSession_IsActive()) {
        return true;
    }

    Net::SpectatorPlaybackSnapshot spectatorPlayback{};
    Net::SpectatorPlayback_GetSnapshot(&spectatorPlayback);
    return spectatorPlayback.active || spectatorPlayback.gameplay_owned || spectatorPlayback.playing;
}

static bool IsOfflineMatchContextActive() {
    return GetGameMode() == MODE_MATCH &&
           IsOfflineSavestateGameType(GetGameType()) &&
           !IsOnlineOwnedContext();
}

static bool IsOfflinePlayableSavestateContext() {
    return IsOfflineMatchContextActive() &&
           IsInPlayableMatchGameplay();
}

static bool IsRollbackBaselineContext() {
    return GetGameMode() == MODE_MATCH &&
           GetSubstate() == MATCH_SUB_GAMEPLAY &&
           Net::Session_IsConnected() &&
           !Rollback::RollbackSession_IsActive();
}

static void UpdateSavestateContext() {
    const bool offlineMatchActive = IsOfflineMatchContextActive();

    if (g_offlineMatchContextActive && !offlineMatchActive) {
        ClearManualSavestateSlot("left offline match context");
        ClearRoundStartSavestateSlot("left offline match context");
        g_roundStartAutosaveArmed = false;
    }

    if (!g_offlineMatchContextActive && offlineMatchActive) {
        g_roundStartAutosaveArmed = true;
        LOG_INFO("[Savestate] Offline match entered — round-start auto-save armed");
    }

    g_offlineMatchContextActive = offlineMatchActive;
    if (!offlineMatchActive) {
        return;
    }

    // Match substate 2 is the round setup path before the opening lock, and the
    // intro-active signal covers the remaining "ROUND/FIGHT" pacing inside substate 3.
    if (GetSubstate() == MATCH_SUB_INIT || IsMatchIntroActive()) {
        g_roundStartAutosaveArmed = true;
    }

    if (g_roundStartAutosaveArmed && IsOfflinePlayableSavestateContext()) {
        if (CapturePracticeSavestateSlot(&g_roundStartSlot, "round_start autosave")) {
            LOG_INFO("[Savestate] Auto-saved first interactable offline frame");
            LogToFile("AUTO_SAVE frame=%u reason=first_interactable_offline_frame\n",
                      g_roundStartSlot.info.frame);
            CapturePracticeSavestateSlot(&g_manualSlot, "manual autosave mirror");
        } else {
            LOG_WARN("[Savestate] Round-start auto-save failed");
        }
        g_roundStartAutosaveArmed = false;
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void Savestate_Init() {
    ClearManualSavestateSlot("init");
    ClearRoundStartSavestateSlot("init");
    ClearRollbackBaselineSlot("init");
    g_offlineMatchContextActive = false;
    g_roundStartAutosaveArmed = false;
    g_f5WasDown = false;
    g_f6WasDown = false;
    LOG_INFO("[Savestate] Initialized (main region: 0x%08X, %u bytes / %u KB)",
             ADDR_MATCH_BASE,
             (unsigned)Rollback::GAME_SNAPSHOT_MAIN_SIZE,
             (unsigned)(Rollback::GAME_SNAPSHOT_MAIN_SIZE / 1024));
}

void Savestate_Shutdown() {
    if (g_logFile) {
        fprintf(g_logFile, "# Shutdown\n");
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// ============================================================================
// Safety Check
// ============================================================================

bool Savestate_CanSaveLoad() {
    return IsOfflinePlayableSavestateContext();
}

// ============================================================================
// SAVE
// ============================================================================

bool Savestate_Save() {
    if (!Savestate_CanSaveLoad()) {
        LOG_WARN("[Savestate] Cannot save — offline playable match required (mode=%d sub=%d type=%d net=%d rb=%d)",
                 ReadMemory<uint32_t>(ADDR_GAME_MODE),
                 ReadMemory<uint32_t>(ADDR_SUB_STATE),
                 ReadMemory<uint32_t>(ADDR_GAME_TYPE),
                 Net::Session_IsConnected() ? 1 : 0,
                 Rollback::RollbackSession_IsActive() ? 1 : 0);
        return false;
    }

    if (!CapturePracticeSavestateSlot(&g_manualSlot, "manual")) {
        return false;
    }

    LOG_INFO("[Savestate] SAVED at frame %d — checksum=0x%08X rng=0x%08X mode=%d sub=%d swap=%d paused=%d step=%u",
             g_manualSlot.info.frame, g_manualSlot.info.checksum, g_manualSlot.info.rng_seed,
             g_manualSlot.info.game_mode, g_manualSlot.info.substate,
             g_manualSlot.practice_control_swap ? 1 : 0,
             g_manualSlot.practice_runtime.paused ? 1 : 0,
             g_manualSlot.practice_runtime.stepCounter);

    // Log to file for determinism analysis
    LogToFile("SAVE frame=%d checksum=0x%08X rng=0x%08X mode=%d sub=%d timer=%d swap=%d paused=%d step=%u step_req=%d fpu_cw=0x%04X mxcsr=0x%08X\n",
              g_manualSlot.info.frame, g_manualSlot.info.checksum, g_manualSlot.info.rng_seed,
              g_manualSlot.info.game_mode, g_manualSlot.info.substate, g_manualSlot.snapshot.match_phase_timer,
              g_manualSlot.practice_control_swap ? 1 : 0,
              g_manualSlot.practice_runtime.paused ? 1 : 0,
              g_manualSlot.practice_runtime.stepCounter,
              g_manualSlot.practice_runtime.stepRequested ? 1 : 0,
              g_manualSlot.fpu_cw, g_manualSlot.fpu_mxcsr);

    return true;
}

bool Savestate_CaptureRollbackBaseline() {
    if (!IsRollbackBaselineContext()) {
        LOG_WARN("[Savestate] Cannot capture rollback baseline — online match sub3 required (mode=%d sub=%d type=%d net=%d rb=%d)",
                 ReadMemory<uint32_t>(ADDR_GAME_MODE),
                 ReadMemory<uint32_t>(ADDR_SUB_STATE),
                 ReadMemory<uint32_t>(ADDR_GAME_TYPE),
                 Net::Session_IsConnected() ? 1 : 0,
                 Rollback::RollbackSession_IsActive() ? 1 : 0);
        return false;
    }

    if (!Rollback::GameSnapshot_Capture(
            &g_rollbackBaselineSlot.snapshot,
            (int32_t)ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER))) {
        LOG_ERROR("[Savestate] Rollback baseline capture FAILED — snapshot capture rejected");
        return false;
    }

    g_rollbackBaselineSlot.fpu_cw = CaptureX87CW();
    g_rollbackBaselineSlot.fpu_mxcsr = CaptureMXCSR();

    g_rollbackBaselineSlot.info.valid = true;
    g_rollbackBaselineSlot.info.frame = g_rollbackBaselineSlot.snapshot.sim_frame;
    g_rollbackBaselineSlot.info.checksum = g_rollbackBaselineSlot.snapshot.checksum;
    g_rollbackBaselineSlot.info.rng_seed = g_rollbackBaselineSlot.snapshot.rng_seed;
    g_rollbackBaselineSlot.info.game_mode = g_rollbackBaselineSlot.snapshot.game_mode;
    g_rollbackBaselineSlot.info.substate = g_rollbackBaselineSlot.snapshot.substate;
    g_rollbackBaselineSlot.practice_control_swap = false;

    LOG_INFO("[Savestate] ROLLBACK BASELINE CAPTURED at frame %d — checksum=0x%08X rng=0x%08X mode=%d sub=%d",
             g_rollbackBaselineSlot.info.frame,
             g_rollbackBaselineSlot.info.checksum,
             g_rollbackBaselineSlot.info.rng_seed,
             g_rollbackBaselineSlot.info.game_mode,
             g_rollbackBaselineSlot.info.substate);

    LogToFile("RB_BASELINE_SAVE frame=%d checksum=0x%08X rng=0x%08X mode=%d sub=%d timer=%d fpu_cw=0x%04X mxcsr=0x%08X\n",
              g_rollbackBaselineSlot.info.frame,
              g_rollbackBaselineSlot.info.checksum,
              g_rollbackBaselineSlot.info.rng_seed,
              g_rollbackBaselineSlot.info.game_mode,
              g_rollbackBaselineSlot.info.substate,
              g_rollbackBaselineSlot.snapshot.match_phase_timer,
              g_rollbackBaselineSlot.fpu_cw,
              g_rollbackBaselineSlot.fpu_mxcsr);

    return true;
}

// ============================================================================
// LOAD
// ============================================================================

bool Savestate_Load() {
    return RestorePracticeSavestateSlot(&g_manualSlot, "manual");
}

bool Savestate_LoadRoundStart() {
    return RestorePracticeSavestateSlot(&g_roundStartSlot, "round_start");
}

bool Savestate_RestoreRollbackBaseline() {
    if (!g_rollbackBaselineSlot.info.valid) {
        LOG_WARN("[Savestate] Cannot restore rollback baseline — no baseline exists.");
        return false;
    }

    if (!IsRollbackBaselineContext()) {
        LOG_WARN("[Savestate] Cannot restore rollback baseline — online match sub3 required (mode=%d sub=%d type=%d net=%d rb=%d)",
                 ReadMemory<uint32_t>(ADDR_GAME_MODE),
                 ReadMemory<uint32_t>(ADDR_SUB_STATE),
                 ReadMemory<uint32_t>(ADDR_GAME_TYPE),
                 Net::Session_IsConnected() ? 1 : 0,
                 Rollback::RollbackSession_IsActive() ? 1 : 0);
        return false;
    }

    uint32_t preFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    uint32_t preRng = DetVer_GetRngSeed();
    uint32_t preChecksum = ComputeMainChecksum();

    LOG_INFO("[Savestate] ROLLBACK BASELINE RESTORE — restoring frame %d (current frame %d)",
             g_rollbackBaselineSlot.info.frame,
             preFrame);

    if (!Rollback::GameSnapshot_Restore(&g_rollbackBaselineSlot.snapshot)) {
        LOG_ERROR("[Savestate] Rollback baseline restore FAILED — snapshot restore rejected");
        return false;
    }

    uint32_t postChecksum = ComputeMainChecksum();
    uint32_t postRng = DetVer_GetRngSeed();
    uint32_t postFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);

    const bool checksumMatch = (postChecksum == g_rollbackBaselineSlot.info.checksum);

    if (checksumMatch) {
        LOG_INFO("[Savestate] ROLLBACK BASELINE RESTORED OK — frame %d->%d checksum=0x%08X (match) rng=0x%08X",
                 preFrame,
                 postFrame,
                 postChecksum,
                 postRng);
    } else {
        LOG_ERROR("[Savestate] ROLLBACK BASELINE RESTORED with CHECKSUM MISMATCH! expected=0x%08X got=0x%08X",
                  g_rollbackBaselineSlot.info.checksum,
                  postChecksum);
    }

    LogToFile("RB_BASELINE_LOAD pre_frame=%d pre_checksum=0x%08X pre_rng=0x%08X\n",
              preFrame,
              preChecksum,
              preRng);
    LogToFile("                restored_frame=%d post_checksum=0x%08X rng=0x%08X match=%s\n",
              postFrame,
              postChecksum,
              postRng,
              checksumMatch ? "YES" : "NO");

    return true;
}

// ============================================================================
// Queries
// ============================================================================

const SavestateInfo* Savestate_GetInfo() {
    return &g_manualSlot.info;
}

const SavestateInfo* Savestate_GetRollbackBaselineInfo() {
    return &g_rollbackBaselineSlot.info;
}

const SavestateInfo* Savestate_GetRoundStartInfo() {
    return &g_roundStartSlot.info;
}

void Savestate_ClearRollbackBaseline(const char* reason) {
    ClearRollbackBaselineSlot(reason);
}

// ============================================================================
// Hotkey Processing
// ============================================================================

void Savestate_ProcessHotkeys() {
    UpdateSavestateContext();

    bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;

    if (!Savestate_CanSaveLoad()) {
        g_f5WasDown = f5Down;
        g_f6WasDown = f6Down;
        return;
    }

    // F5: Save (on key-down edge)
    if (f5Down && !g_f5WasDown) {
        if (Savestate_Save()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "State Saved (F%d)", g_manualSlot.info.frame);
            PracticeTools_Toast(buf, 0xFF64FF64);  // green
        } else {
            PracticeTools_Toast("Save Failed", 0xFF6464FF);  // red
        }
    }

    // F6: Load (on key-down edge)
    if (f6Down && !g_f6WasDown) {
        if (Savestate_Load()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "State Loaded (F%d)", g_manualSlot.info.frame);
            PracticeTools_Toast(buf, 0xFF64C8FF);  // cyan
        } else {
            PracticeTools_Toast("Load Failed", 0xFF6464FF);  // red
        }
    }

    g_f5WasDown = f5Down;
    g_f6WasDown = f6Down;
}

// ============================================================================
// Debug UI
// ============================================================================

void Savestate_RenderImGui() {
    const SavestateInfo* info = &g_manualSlot.info;
    const SavestateInfo* roundStartInfo = &g_roundStartSlot.info;

    ImGui::Text("Offline Savestate (Auto-save + F5/F6)");
    ImGui::Separator();

    if (info->valid) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Slot: VALID");
        ImGui::Text("Frame:    %d", info->frame);
        ImGui::Text("Checksum: 0x%08X", info->checksum);
        ImGui::Text("RNG Seed: 0x%08X", info->rng_seed);
        ImGui::Text("Mode:     %d  Sub: %d", info->game_mode, info->substate);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Slot: EMPTY");
        ImGui::TextDisabled("Auto-saves on the first playable offline frame of each round");
    }

    if (roundStartInfo->valid) {
        ImGui::Text("Round Start Slot: frame %d", roundStartInfo->frame);
    } else {
        ImGui::TextDisabled("Round Start Slot: empty");
    }

    ImGui::Separator();

    bool canSaveLoad = Savestate_CanSaveLoad();
    ImGui::Text("Can Save/Load: %s", canSaveLoad ? "Yes" : "No");
    ImGui::TextDisabled("Offline Mode 8 gameplay only. Disabled for netplay, spectator, replay, and demo.");

    if (canSaveLoad) {
        // Show current live state for comparison
        uint32_t curFrame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        uint32_t curRng   = DetVer_GetRngSeed();
        ImGui::Text("Current Frame: %d", curFrame);
        ImGui::Text("Current RNG:   0x%08X", curRng);

        if (info->valid) {
            int frameDiff = (int)curFrame - (int)info->frame;
            ImGui::Text("Frames since save: %d", frameDiff);
        }
    }

    ImGui::Separator();

    // Manual buttons (in addition to hotkeys)
    if (canSaveLoad) {
        if (ImGui::Button("Save State (F5)")) {
            Savestate_Save();
        }
        ImGui::SameLine();
        if (info->valid) {
            if (ImGui::Button("Load State (F6)")) {
                Savestate_Load();
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Load State (F6)");
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (roundStartInfo->valid) {
            if (ImGui::Button("Load Round Start")) {
                Savestate_LoadRoundStart();
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Load Round Start");
            ImGui::EndDisabled();
        }
    } else {
        ImGui::TextDisabled("Start a match to enable save/load");
    }

    ImGui::Separator();
    ImGui::TextDisabled("State region: 0x%08X (%u KB)",
        ADDR_MATCH_BASE,
        (unsigned)(Rollback::GAME_SNAPSHOT_MAIN_SIZE / 1024));
    ImGui::TextDisabled("FPU: diagnostic only (not restored)");
}
