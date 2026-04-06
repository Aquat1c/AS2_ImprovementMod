/**
 * Alice Senki 2 - Automated Test Harness Implementation
 *
 * State machine for automated session + match lifecycle.
 * Deep game-state profiling writes snapshots into a named shared memory
 * region so the external launcher (as2_test_harness.exe) can display a
 * real-time ImGui dashboard for BOTH game instances.
 */

#include "test_harness.h"
#include "harness_shared_memory.h"
#include "session_manager.h"
#include "netplay_config.h"
#include "charsel_sync.h"
#include "input_sync_hooks.h"
#include "netplay_hooks.h"
#include "rollback_session.h"
#include "game_state.h"
#include "as2_constants.h"
#include "as2_rollback.h"
#include "log_window.h"
#include "input_system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

namespace TestHarness {

// ============================================================================
// Addresses not in as2_constants.h
// ============================================================================

static constexpr uintptr_t ADDR_P1_CPU_FLAG   = 0x8E9F0C;
static constexpr uintptr_t ADDR_P2_CPU_FLAG   = 0x8E9FDC;
static constexpr uintptr_t ADDR_SET_GAME_MODE = 0x5D2EB0;

static const char* kConfigFilename = "as2_autoconnect.cfg";

// ============================================================================
// Phase enum
// ============================================================================

enum class Phase {
    Idle,
    WaitingForGame,
    StartingSession,
    WaitingForConnection,
    LaunchingCharSel,
    WaitingForCharSel,
    AutoSelectingChar,
    WaitingForMatch,
    InMatch,
    Done,
};

static const char* kPhaseNames[] = {
    "Idle", "WaitingForGame", "StartingSession", "WaitingForConnection",
    "LaunchingCharSel", "WaitingForCharSel", "AutoSelectingChar",
    "WaitingForMatch", "InMatch", "Done"
};

// ============================================================================
// Internal state
// ============================================================================

static Phase             s_phase = Phase::Idle;
static AutoConnectConfig s_config = {};
static bool              s_active = false;
static int               s_frameCounter = 0;
static int               s_phaseTimer = 0;
static int               s_charSelectDelay = 0;
static FILE*             s_logFile = nullptr;
static bool              s_charSent = false;
static uint32_t          s_matchStartFrame = 0;

// Deep profiler accumulation
static FrameSnapshot s_latestSnapshot = {};
static ProfileStats  s_stats = {};
static uint32_t      s_prevRngSeed = 0;
static double        s_sumSaveUs = 0, s_sumLoadUs = 0, s_sumAdvanceUs = 0;
static int           s_timingCount = 0;
static double        s_sumRtt = 0;
static int           s_rttCount = 0;
static uint32_t      s_prevSimFrame = 0;
static uint32_t      s_prevDisplayFrame = 0;
static uint32_t      s_prevCharSelSubState = UINT32_MAX;
static uint32_t      s_prevStageId = UINT32_MAX;
static int           s_stagePreviewFrames = 0;
static bool          s_stagePreviewSeen = false;
static bool          s_stageConfirmSent = false;
static uint16_t      s_lastCharSelOverride = 0;

// ============================================================================
// Shared memory handle
// ============================================================================

static HANDLE              s_shmHandle = nullptr;
static HarnessSharedData*  s_shm = nullptr;

static bool CreateSharedMemory() {
    const char* name = s_config.is_host
                     ? HARNESS_SHM_NAME_HOST
                     : HARNESS_SHM_NAME_CLIENT;

    s_shmHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(HarnessSharedData), name);

    if (!s_shmHandle) {
        LOG_ERROR("[Harness] CreateFileMapping failed: %lu", GetLastError());
        return false;
    }

    s_shm = (HarnessSharedData*)MapViewOfFile(
        s_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HarnessSharedData));

    if (!s_shm) {
        LOG_ERROR("[Harness] MapViewOfFile failed: %lu", GetLastError());
        CloseHandle(s_shmHandle);
        s_shmHandle = nullptr;
        return false;
    }

    memset(s_shm, 0, sizeof(HarnessSharedData));
    s_shm->magic   = HARNESS_SHM_MAGIC;
    s_shm->version = HARNESS_SHM_VERSION;
    s_shm->active  = 1;
    s_shm->is_host = s_config.is_host ? 1 : 0;
    strncpy_s(s_shm->nickname, sizeof(s_shm->nickname), s_config.nickname, _TRUNCATE);

    // Store the session log directory so the launcher can find log files
    const char* logDir = LogWindow_GetLogDir();
    if (logDir && logDir[0]) {
        strncpy_s(s_shm->session_log_dir, sizeof(s_shm->session_log_dir), logDir, _TRUNCATE);
    }
    const char* traceLog = AS2_GetRollbackTraceTextPath();
    const char* traceBin = AS2_GetRollbackTraceBinaryPath();
    if (traceLog && traceLog[0]) {
        strncpy_s(s_shm->rollback_trace_log, sizeof(s_shm->rollback_trace_log), traceLog, _TRUNCATE);
    }
    if (traceBin && traceBin[0]) {
        strncpy_s(s_shm->rollback_trace_bin, sizeof(s_shm->rollback_trace_bin), traceBin, _TRUNCATE);
    }

    LOG_INFO("[Harness] Shared memory created: %s", name);
    return true;
}

static void DestroySharedMemory() {
    if (s_shm) {
        s_shm->active = 0;
        s_shm->write_seq++;
        UnmapViewOfFile(s_shm);
        s_shm = nullptr;
    }
    if (s_shmHandle) {
        CloseHandle(s_shmHandle);
        s_shmHandle = nullptr;
    }
}

// Push a line into the shared-memory log ring buffer
static void ShmLog(const char* fmt, ...) {
    if (!s_shm) return;
    uint32_t idx = s_shm->log_head % HARNESS_LOG_LINES;
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_shm->log_lines[idx], HARNESS_LOG_LINE_LEN, fmt, args);
    va_end(args);
    s_shm->log_head++;
    s_shm->log_count++;
}

// Flush all current state into shared memory
static void FlushToSharedMemory() {
    if (!s_shm) return;

    s_shm->phase = (uint32_t)s_phase;
    s_shm->frame_counter = (uint32_t)s_frameCounter;
    int pidx = (int)s_phase;
    strncpy_s(s_shm->phase_name, sizeof(s_shm->phase_name),
              (pidx >= 0 && pidx <= 9) ? kPhaseNames[pidx] : "?", _TRUNCATE);

    // Connection info
    SessionManager::Snapshot snap = {};
    if (SessionManager::GetSnapshot(&snap)) {
        s_shm->session_state    = (uint32_t)snap.state;
        s_shm->conn_rtt_ms      = snap.rtt_ms;
        s_shm->packets_sent     = snap.packets_sent;
        s_shm->packets_received = snap.packets_received;
        s_shm->desync_count     = snap.desync_count;
        strncpy_s(s_shm->peer_nickname, sizeof(s_shm->peer_nickname), snap.peer_nickname, _TRUNCATE);
        strncpy_s(s_shm->status_text, sizeof(s_shm->status_text), snap.status, _TRUNCATE);
        strncpy_s(s_shm->error_text, sizeof(s_shm->error_text), snap.last_error, _TRUNCATE);
    }

    // Game state from latest snapshot
    const FrameSnapshot& sn = s_latestSnapshot;
    s_shm->game_mode       = sn.game_mode;
    s_shm->sub_state       = sn.sub_state;
    s_shm->stage_id        = sn.stage_id;
    s_shm->game_type       = sn.game_type;
    s_shm->sim_frame       = sn.sim_frame;
    s_shm->display_frame   = sn.display_frame;
    s_shm->write_frame     = sn.write_frame;
    s_shm->net_frame       = sn.net_frame;
    s_shm->vanilla_role    = sn.vanilla_role;
    s_shm->vanilla_connected = sn.vanilla_connected;
    s_shm->rng_seed        = sn.rng_seed;
    s_shm->checksum        = sn.checksum;

    // Entity
    s_shm->p1_hp     = sn.p1_hp;    s_shm->p2_hp     = sn.p2_hp;
    s_shm->p1_meter  = sn.p1_meter; s_shm->p2_meter  = sn.p2_meter;
    s_shm->p1_x      = sn.p1_x;    s_shm->p1_y      = sn.p1_y;
    s_shm->p2_x      = sn.p2_x;    s_shm->p2_y      = sn.p2_y;
    s_shm->p1_action  = sn.p1_action; s_shm->p2_action = sn.p2_action;
    s_shm->p1_input   = sn.p1_input;  s_shm->p2_input  = sn.p2_input;

    // Stats
    s_shm->total_frames           = s_stats.total_frames;
    s_shm->total_rollbacks        = s_stats.total_rollbacks;
    s_shm->max_rollback_depth     = s_stats.max_rollback_depth;
    s_shm->total_saves            = s_stats.total_saves;
    s_shm->total_loads            = s_stats.total_loads;
    s_shm->desync_warnings        = s_stats.desync_warnings;
    s_shm->avg_save_us            = s_stats.avg_save_us;
    s_shm->peak_save_us           = s_stats.peak_save_us;
    s_shm->avg_load_us            = s_stats.avg_load_us;
    s_shm->peak_load_us           = s_stats.peak_load_us;
    s_shm->avg_advance_us         = s_stats.avg_advance_us;
    s_shm->peak_advance_us        = s_stats.peak_advance_us;
    s_shm->avg_rtt_ms             = s_stats.avg_rtt_ms;
    s_shm->peak_rtt_ms            = s_stats.peak_rtt_ms;
    s_shm->vanilla_flag_violations = s_stats.vanilla_flag_violations;
    s_shm->rng_jumps              = s_stats.rng_jumps;
    s_shm->frame_counter_anomalies = s_stats.frame_counter_anomalies;

    // Net sim config
    s_shm->sim_latency_ms = s_config.net_sim.latency_ms;
    s_shm->sim_jitter_ms  = s_config.net_sim.jitter_ms;
    s_shm->sim_loss_pct   = s_config.net_sim.packet_loss_pct;
    s_shm->sim_dup_pct    = s_config.net_sim.duplicate_pct;

    const char* traceLog = AS2_GetRollbackTraceTextPath();
    const char* traceBin = AS2_GetRollbackTraceBinaryPath();
    if (traceLog && traceLog[0]) {
        strncpy_s(s_shm->rollback_trace_log, sizeof(s_shm->rollback_trace_log), traceLog, _TRUNCATE);
    }
    if (traceBin && traceBin[0]) {
        strncpy_s(s_shm->rollback_trace_bin, sizeof(s_shm->rollback_trace_bin), traceBin, _TRUNCATE);
    }

    // GekkoNet rollback state
    {
        RollbackSession::Snapshot rbSnap = {};
        if (RollbackSession::GetSnapshot(&rbSnap)) {
            s_shm->rb_local_frame    = rbSnap.local_frame;
            s_shm->rb_remote_frame   = rbSnap.remote_frame;
            s_shm->rb_frames_ahead   = rbSnap.frames_ahead;
            s_shm->rb_state          = (uint32_t)rbSnap.state;
            s_shm->rb_advance_count  = rbSnap.advance_count;
            s_shm->rb_adapter_send   = rbSnap.adapter_send_count;
            s_shm->rb_adapter_recv   = rbSnap.adapter_recv_count;
            s_shm->rb_timesync_skips = rbSnap.timesync_skips;
        }
    }

    // Memory fence + sequence bump
    _ReadWriteBarrier();
    s_shm->write_seq++;
}

// ============================================================================
// Memory helpers
// ============================================================================

static uint8_t  RdU8 (uintptr_t a) { return *reinterpret_cast<volatile uint8_t*>(a);  }
static int16_t  RdS16(uintptr_t a) { return *reinterpret_cast<volatile int16_t*>(a);  }
static uint16_t RdU16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
static uint32_t RdU32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
static void WrU8 (uintptr_t a, uint8_t  v) { *reinterpret_cast<volatile uint8_t*>(a)  = v; }
static void WrU32(uintptr_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(a) = v; }

// ============================================================================
// INI parser
// ============================================================================

static void Trim(char* s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1]=='\r'||s[len-1]=='\n'||s[len-1]==' '||s[len-1]=='\t'))
        s[--len] = '\0';
    int st = 0;
    while (s[st]==' '||s[st]=='\t') st++;
    if (st > 0) memmove(s, s+st, len-st+1);
}

static bool ParseConfigFile(const char* path, AutoConnectConfig* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;

    memset(out, 0, sizeof(*out));
    out->delay_frames = 0;
    out->profiling.enabled = true;
    out->profiling.log_inputs = true;
    out->profiling.log_checksums = true;
    out->profiling.log_rollbacks = true;
    out->profiling.log_frame_timing = true;
    out->profiling.log_rtt = true;
    out->profiling.log_game_state = true;
    out->profiling.log_entity_detail = true;
    out->profiling.log_rng = true;
    out->profiling.log_vanilla_flags = true;
    out->profiling.log_stage_debug = false;
    out->stage_mash_frames = 180;
    strncpy_s(out->nickname, sizeof(out->nickname), "TestPlayer", _TRUNCATE);
    strncpy_s(out->profiling.log_file, sizeof(out->profiling.log_file), "harness.log", _TRUNCATE);

    enum { S_NONE, S_AUTO, S_NET, S_PROF } sec = S_NONE;
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        Trim(line);
        if (!line[0] || line[0]=='#' || line[0]==';') continue;
        if (line[0]=='[') {
            if (strstr(line,"[autoconnect]")) sec=S_AUTO;
            else if (strstr(line,"[network_sim]")) sec=S_NET;
            else if (strstr(line,"[profiling]")) sec=S_PROF;
            else sec=S_NONE;
            continue;
        }
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line; char* val = eq+1;
        Trim(key); Trim(val);

        switch (sec) {
        case S_AUTO:
            if (!strcmp(key,"enabled"))          out->enabled = atoi(val)!=0;
            else if (!strcmp(key,"role"))        out->is_host = _stricmp(val,"host")==0;
            else if (!strcmp(key,"nickname"))    strncpy_s(out->nickname,sizeof(out->nickname),val,_TRUNCATE);
            else if (!strcmp(key,"port"))        out->listen_port = (uint16_t)atoi(val);
            else if (!strcmp(key,"target_ip"))   strncpy_s(out->target_ip,sizeof(out->target_ip),val,_TRUNCATE);
            else if (!strcmp(key,"target_port")) out->target_port = (uint16_t)atoi(val);
            else if (!strcmp(key,"character_id"))out->character_id = atoi(val);
            else if (!strcmp(key,"palette"))     out->palette = atoi(val);
            else if (!strcmp(key,"delay_frames"))out->delay_frames = atoi(val);
            else if (!strcmp(key,"match_duration_sec")) out->match_duration_sec = atoi(val);
            else if (!strcmp(key,"stage_mash_test")) out->stage_mash_test = atoi(val)!=0;
            else if (!strcmp(key,"stage_mash_frames")) out->stage_mash_frames = atoi(val);
            break;
        case S_NET:
            if (!strcmp(key,"latency_ms"))       out->net_sim.latency_ms = atoi(val);
            else if (!strcmp(key,"jitter_ms"))   out->net_sim.jitter_ms = atoi(val);
            else if (!strcmp(key,"packet_loss_pct")) out->net_sim.packet_loss_pct = (float)atof(val);
            else if (!strcmp(key,"duplicate_pct"))   out->net_sim.duplicate_pct = (float)atof(val);
            else if (!strcmp(key,"timesync_threshold")) out->net_sim.timesync_threshold = (float)atof(val);
            break;
        case S_PROF:
            if (!strcmp(key,"enabled"))          out->profiling.enabled = atoi(val)!=0;
            else if (!strcmp(key,"log_file"))    strncpy_s(out->profiling.log_file,sizeof(out->profiling.log_file),val,_TRUNCATE);
            else if (!strcmp(key,"log_inputs"))  out->profiling.log_inputs = atoi(val)!=0;
            else if (!strcmp(key,"log_checksums"))  out->profiling.log_checksums = atoi(val)!=0;
            else if (!strcmp(key,"log_rollbacks"))  out->profiling.log_rollbacks = atoi(val)!=0;
            else if (!strcmp(key,"log_frame_timing")) out->profiling.log_frame_timing = atoi(val)!=0;
            else if (!strcmp(key,"log_rtt"))     out->profiling.log_rtt = atoi(val)!=0;
            else if (!strcmp(key,"log_game_state")) out->profiling.log_game_state = atoi(val)!=0;
            else if (!strcmp(key,"log_entity_detail")) out->profiling.log_entity_detail = atoi(val)!=0;
            else if (!strcmp(key,"log_rng"))     out->profiling.log_rng = atoi(val)!=0;
            else if (!strcmp(key,"log_vanilla_flags")) out->profiling.log_vanilla_flags = atoi(val)!=0;
            else if (!strcmp(key,"log_stage_debug")) out->profiling.log_stage_debug = atoi(val)!=0;
            break;
        default: break;
        }
    }
    fclose(f);
    return true;
}

// ============================================================================
// File logging (still used for post-run analysis)
// ============================================================================

static void OpenLog() {
    if (!s_config.profiling.enabled || s_logFile) return;
    // Place harness log in the mod's session log directory if available
    char logPath[MAX_PATH];
    const char* logDir = LogWindow_GetLogDir();
    if (logDir && logDir[0]) {
        snprintf(logPath, sizeof(logPath), "%s\\%s", logDir, s_config.profiling.log_file);
    } else {
        strncpy_s(logPath, sizeof(logPath), s_config.profiling.log_file, _TRUNCATE);
    }
    if (fopen_s(&s_logFile, logPath, "w") != 0) {
        s_logFile = nullptr;
        LOG_ERROR("[Harness] Failed to open log: %s", logPath);
        return;
    }
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_s(&local, &now);
    char tb[64];
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(s_logFile, "# AS2 Test Harness Deep Profiling Log\n");
    fprintf(s_logFile, "# Started: %s  Role: %s  Nick: %s\n", tb,
            s_config.is_host ? "Host" : "Client", s_config.nickname);
    fprintf(s_logFile, "#\n# TYPE | FRAME | DATA...\n#\n");
    fflush(s_logFile);
}

static void LogLine(const char* fmt, ...) {
    if (!s_logFile) return;
    va_list a; va_start(a, fmt);
    vfprintf(s_logFile, fmt, a);
    va_end(a);
    fprintf(s_logFile, "\n");
    static int lc = 0;
    if (++lc % 100 == 0) fflush(s_logFile);
}

static void PhaseLog(const char* fmt, ...) {
    char buf[512];
    va_list a; va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    LOG_INFO("[Harness] %s", buf);
    LogLine("PHASE | %d | %s", s_frameCounter, buf);
    ShmLog("PHASE | %d | %s", s_frameCounter, buf);
}

static bool IsStageDebugEnabled() {
    return s_config.profiling.log_stage_debug || s_config.stage_mash_test;
}

static const char* CharSelSubstateName(uint32_t subState) {
    switch (subState) {
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

static uint16_t BuildStageMashInput(int previewFrame) {
    const uint16_t allDirections = (uint16_t)(INPUT_LEFT | INPUT_RIGHT | INPUT_UP | INPUT_DOWN);

    // Keep the repro focused on contradictory directional state only.
    // Do not include START/SELECT or cancel buttons here; they can trigger
    // menu/back-out paths instead of exercising the shared stage cursor.
    const uint16_t hostPattern[6] = {
        allDirections,
        0,
        (uint16_t)(INPUT_LEFT | INPUT_RIGHT),
        0,
        (uint16_t)(INPUT_UP | INPUT_DOWN),
        0,
    };
    const uint16_t clientPattern[6] = {
        0,
        allDirections,
        0,
        (uint16_t)(INPUT_UP | INPUT_DOWN),
        0,
        (uint16_t)(INPUT_LEFT | INPUT_RIGHT),
    };

    const int patternIndex = previewFrame % 6;
    return s_config.is_host ? hostPattern[patternIndex] : clientPattern[patternIndex];
}

// ============================================================================
// Deep game state snapshot
// ============================================================================

static void CaptureSnapshot(FrameSnapshot* snap) {
    memset(snap, 0, sizeof(*snap));
    snap->frame = (uint32_t)s_frameCounter;

    snap->game_mode  = RdU32(ADDR_GAME_MODE);
    snap->sub_state  = RdU32(ADDR_SUB_STATE);
    snap->stage_id   = RdU8(ADDR_CHARSEL_STAGE_ID);
    snap->game_type  = RdU32(ADDR_GAME_TYPE);

    snap->sim_frame     = RdU32(ADDR_FRAME_SIMULATION);
    snap->display_frame = RdU32(ADDR_FRAME_DISPLAY);
    snap->write_frame   = RdU32(ADDR_FRAME_WRITE_IDX);
    snap->net_frame     = RdU32(ADDR_FRAME_NET_IDX);

    snap->vanilla_role      = RdU8(ADDR_NETPLAY_ROLE);
    snap->vanilla_connected = RdU8(ADDR_NETPLAY_CONNECTED);

    snap->rng_seed = AS2_GetRngSeed();

    // Reconstruct combined bitmask from per-button held arrays.
    // ADDR_P1_INPUT_STATE (0x8E9E9A) is actually the first word of the
    // just-pressed per-button array (each word = 0 or 1), NOT a combined bitmask.
    // Read the held array at ADDR_P1_INPUT_BUFFER and combine.
    {
        static const uint16_t kButtonMasks[10] = {
            0x0001, 0x0002, 0x0004, 0x0008, 0x0010,
            0x0020, 0x0040, 0x0080, 0x0100, 0x0200
        };
        uint16_t p1 = 0, p2 = 0;
        for (int i = 0; i < 10; i++) {
            if (RdU16(ADDR_P1_INPUT_BUFFER + i * 2)) p1 |= kButtonMasks[i];
            if (RdU16(ADDR_P2_INPUT_BUFFER + i * 2)) p2 |= kButtonMasks[i];
        }
        snap->p1_input = p1;
        snap->p2_input = p2;
    }

    uintptr_t p1 = GetEntityBase(0);
    uintptr_t p2 = GetEntityBase(1);
    if (p1) {
        snap->p1_hp     = RdS16(p1 + ENTITY_OFF_HP);
        snap->p1_meter  = RdU16(p1 + ENTITY_OFF_METER);
        snap->p1_x      = RdS16(p1 + ENTITY_OFF_X_POS);
        snap->p1_y      = RdS16(p1 + ENTITY_OFF_Y_POS);
        snap->p1_action = RdU32(p1 + ENTITY_OFF_ACTION_ID);
    }
    if (p2) {
        snap->p2_hp     = RdS16(p2 + ENTITY_OFF_HP);
        snap->p2_meter  = RdU16(p2 + ENTITY_OFF_METER);
        snap->p2_x      = RdS16(p2 + ENTITY_OFF_X_POS);
        snap->p2_y      = RdS16(p2 + ENTITY_OFF_Y_POS);
        snap->p2_action = RdU32(p2 + ENTITY_OFF_ACTION_ID);
    }

    snap->checksum = AS2_GetQuickChecksum();
}

static void LogSnapshot(const FrameSnapshot* snap) {
    if (!s_logFile) return;

    if (s_config.profiling.log_game_state) {
        LogLine("STATE | %u | mode=%u sub=%u stage=%u type=%u sim=%u disp=%u write=%u net=%u rng=0x%08X chk=0x%04X",
            snap->frame, snap->game_mode, snap->sub_state, snap->stage_id, snap->game_type,
                snap->sim_frame, snap->display_frame, snap->write_frame, snap->net_frame,
                snap->rng_seed, snap->checksum);
    }
    if (s_config.profiling.log_entity_detail) {
        LogLine("ENTITY | %u | P1[hp=%d m=%u x=%d y=%d act=%u] P2[hp=%d m=%u x=%d y=%d act=%u]",
                snap->frame,
                snap->p1_hp, snap->p1_meter, snap->p1_x, snap->p1_y, snap->p1_action,
                snap->p2_hp, snap->p2_meter, snap->p2_x, snap->p2_y, snap->p2_action);
    }
    if (s_config.profiling.log_inputs) {
        LogLine("INPUT | %u | P1=0x%04X P2=0x%04X", snap->frame, snap->p1_input, snap->p2_input);
    }
    if (s_config.profiling.log_checksums) {
        LogLine("CHECKSUM | %u | 0x%04X", snap->frame, snap->checksum);
    }

    // Anomaly: vanilla flags
    if (s_config.profiling.log_vanilla_flags) {
        if (snap->vanilla_role || snap->vanilla_connected) {
            LogLine("ANOMALY | %u | VANILLA_FLAGS role=%u connected=%u",
                    snap->frame, snap->vanilla_role, snap->vanilla_connected);
            ShmLog("ANOMALY: vanilla flags role=%u conn=%u", snap->vanilla_role, snap->vanilla_connected);
            s_stats.vanilla_flag_violations++;
        }
    }

    // RNG tracking
    if (s_config.profiling.log_rng && s_prevRngSeed != 0 && snap->frame % 60 == 0) {
        LogLine("RNG | %u | seed=0x%08X prev=0x%08X", snap->frame, snap->rng_seed, s_prevRngSeed);
    }
    s_prevRngSeed = snap->rng_seed;

    // Frame counter anomalies — only flag when rollback is NOT active.
    // During rollback, the game's sim counter jumps around as GekkoNet
    // loads/resimulates states — that's normal, not an anomaly.
    if (snap->game_mode == MODE_MATCH && snap->sub_state >= 3) {
        uint32_t delta = snap->sim_frame - s_prevSimFrame;
        bool rollbackRunning = (RollbackSession::GetState() == RollbackSession::State::Running);
        if (s_prevSimFrame > 0 && delta > 1 && delta < 0x80000000 && !rollbackRunning) {
            LogLine("ANOMALY | %u | SIM_SKIP sim=%u prev=%u delta=%u",
                    snap->frame, snap->sim_frame, s_prevSimFrame, delta);
            ShmLog("ANOMALY: sim skip %u->%u", s_prevSimFrame, snap->sim_frame);
            s_stats.frame_counter_anomalies++;
        }
        s_prevSimFrame = snap->sim_frame;
        s_prevDisplayFrame = snap->display_frame;
    }
}

// ============================================================================
// Phase transitions
// ============================================================================

static void TransitionTo(Phase next, const char* reason) {
    int from = (int)s_phase, to = (int)next;
    PhaseLog("Phase %s -> %s (%s)",
             (from>=0&&from<=9)?kPhaseNames[from]:"?",
             (to>=0&&to<=9)?kPhaseNames[to]:"?", reason);

    // Clean up input overrides when leaving active phases
    if (s_phase == Phase::InMatch || s_phase == Phase::AutoSelectingChar)
        InputSystem_ClearOverride(0);

    s_phase = next;
    s_phaseTimer = 0;
}

// ============================================================================
// Phase handlers
// ============================================================================

static void HandleWaitingForGame() {
    if (RdU32(ADDR_GAME_MODE) >= (uint32_t)MODE_MENU)
        TransitionTo(Phase::StartingSession, "game reached menu");
    else if (s_phaseTimer > 600)
        TransitionTo(Phase::Done, "timeout waiting for game menu");
}

static void HandleStartingSession() {
    NetplayConfig::Config cfg = {};
    NetplayConfig::SetDefaults(&cfg);
    strncpy_s(cfg.nickname, sizeof(cfg.nickname), s_config.nickname, _TRUNCATE);
    cfg.listen_port = s_config.listen_port;
    cfg.preferred_delay_frames = s_config.delay_frames;

    if (!s_config.is_host) {
        unsigned a,b,c,d;
        if (sscanf_s(s_config.target_ip, "%u.%u.%u.%u", &a,&b,&c,&d) == 4) {
            cfg.target_ip[0]=a; cfg.target_ip[1]=b; cfg.target_ip[2]=c; cfg.target_ip[3]=d;
        } else {
            cfg.target_ip[0]=127; cfg.target_ip[3]=1;
        }
        cfg.target_port = s_config.target_port;
    }
    NetplayConfig::ApplyToGameMemory(&cfg);

    bool ok = s_config.is_host
            ? SessionManager::StartHost(&cfg)
            : SessionManager::StartJoin(&cfg);
    PhaseLog("%s session on port %u -> %s:%u",
             s_config.is_host?"HOST":"JOIN", s_config.listen_port,
             s_config.target_ip, s_config.target_port);
    TransitionTo(ok ? Phase::WaitingForConnection : Phase::Done,
                 ok ? "session started" : "session start failed");
}

static void HandleWaitingForConnection() {
    SessionManager::FrameUpdate();
    if (SessionManager::ConsumePendingCharSelStart()) {
        TransitionTo(Phase::LaunchingCharSel, "host sent charsel start");
        return;
    }
    SessionManager::Snapshot snap = {};
    if (!SessionManager::GetSnapshot(&snap)) return;
    switch (snap.state) {
    case SessionManager::State::Connected:
        PhaseLog("Connected to '%s' RTT=%.1fms", snap.peer_nickname, snap.rtt_ms);
        if (s_config.is_host) TransitionTo(Phase::LaunchingCharSel, "connected as host");
        break;
    case SessionManager::State::Error:
        PhaseLog("Connection error: %s", snap.last_error);
        TransitionTo(Phase::Done, "connection error");
        break;
    default:
        // 120 seconds (7200 frames) — must exceed game boot time (~50s)
        if (s_phaseTimer > 7200) TransitionTo(Phase::Done, "connection timeout");
        break;
    }
}

static void HandleLaunchingCharSel() {
    WrU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    WrU8(ADDR_NETPLAY_CONNECTED, 0);
    WrU8(ADDR_NETPLAY_ROLE, 0);
    WrU8(ADDR_P1_CPU_FLAG, 0);
    WrU8(ADDR_P2_CPU_FLAG, 0);

    typedef int(__cdecl* SetGameMode_t)(int, int);
    int r = ((SetGameMode_t)ADDR_SET_GAME_MODE)(MODE_CHARSEL, 1);
    WrU32(ADDR_GAME_TYPE, GAMETYPE_VS_HUMAN);
    SessionManager::EnterCharSel();

    PhaseLog("Launched CharSel (result=%d)", r);
    s_charSent = false;
    s_charSelectDelay = 0;
    s_prevCharSelSubState = UINT32_MAX;
    s_prevStageId = UINT32_MAX;
    s_stagePreviewFrames = 0;
    s_stagePreviewSeen = false;
    s_stageConfirmSent = false;
    s_lastCharSelOverride = 0;
    TransitionTo(Phase::WaitingForCharSel, "charsel launched");
}

static void HandleWaitingForCharSel() {
    SessionManager::FrameUpdate();
    if (RdU32(ADDR_GAME_MODE) == (uint32_t)MODE_CHARSEL)
        TransitionTo(Phase::AutoSelectingChar, "game entered charsel");
    else if (s_phaseTimer > 300)
        TransitionTo(Phase::Done, "timeout waiting for charsel");
}

static void HandleAutoSelectingChar() {
    SessionManager::FrameUpdate();
    s_charSelectDelay++;
    if (s_charSelectDelay < 30) return;

    const uint32_t mode = RdU32(ADDR_GAME_MODE);
    const uint32_t subState = RdU32(ADDR_SUB_STATE);
    const uint8_t stageId = RdU8(ADDR_CHARSEL_STAGE_ID);

    if (IsStageDebugEnabled()) {
        if (subState != s_prevCharSelSubState) {
            if (s_prevCharSelSubState == UINT32_MAX) {
                PhaseLog("CharSel substate initial: %s (mode=%u stage=%u)",
                         CharSelSubstateName(subState), mode, stageId);
            } else {
                PhaseLog("CharSel substate changed: %s -> %s (mode=%u stage=%u)",
                         CharSelSubstateName(s_prevCharSelSubState),
                         CharSelSubstateName(subState),
                         mode,
                         stageId);
            }
            RecordStageEvent("HarnessSubstate",
                             (uint32_t)s_frameCounter,
                             s_stagePreviewSeen ? s_stagePreviewFrames : -1,
                             mode,
                             subState,
                             stageId,
                             s_lastCharSelOverride,
                             0,
                             0,
                             0,
                             0,
                             s_lastCharSelOverride,
                             0);
            s_prevCharSelSubState = subState;
        }
        if ((uint32_t)stageId != s_prevStageId) {
            if (s_prevStageId == UINT32_MAX) {
                PhaseLog("Stage cursor initial: %u (sub=%u %s)",
                         stageId, subState, CharSelSubstateName(subState));
            } else {
                PhaseLog("Stage cursor changed: %u -> %u (sub=%u %s)",
                         s_prevStageId, stageId, subState, CharSelSubstateName(subState));
            }
            RecordStageEvent("HarnessStageId",
                             (uint32_t)s_frameCounter,
                             s_stagePreviewSeen ? s_stagePreviewFrames : -1,
                             mode,
                             subState,
                             stageId,
                             s_lastCharSelOverride,
                             0,
                             0,
                             0,
                             0,
                             s_lastCharSelOverride,
                             0);
            s_prevStageId = stageId;
        }
    }

    // Check for mode transitions — with stage select enabled the flow is:
    // CharSel sub2→4 (char confirm) → sub7/8 (stage grid) → sub14 → MODE_STAGESEL → MODE_MATCH
    if (mode == (uint32_t)MODE_MATCH) {
        InputSystem_ClearOverride(0);
        s_lastCharSelOverride = 0;
        TransitionTo(Phase::InMatch, "game entered match");
        return;
    }
    if (mode == (uint32_t)MODE_STAGESEL) {
        InputSystem_ClearOverride(0);
        s_lastCharSelOverride = 0;
        TransitionTo(Phase::WaitingForMatch, "game entered stage select cinematic");
        return;
    }

    uint16_t overrideInput = 0;
    bool setOverride = false;

    // ── Input-driven character selection ──────────────────────────────────
    // DO NOT write charsel memory directly — those writes are LOCAL-only
    // and cause desync between host and client.  Instead, inject inputs
    // through InputSystem_SetOverride so they flow through the CharSel
    // lockstep and reach both sides identically.
    //
    // In GAMETYPE_VS_HUMAN (2), P2 starts already confirmed at its default
    // grid position.  Only P1 needs to confirm.  Both host and client
    // inject INPUT_A on their local input poll (player 0); the lockstep
    // routes host's input to P1 and client's input to P2.

    if (!s_charSent) {
        // Inject A button for one frame — triggers "just pressed" in lockstep
        overrideInput = INPUT_A;
        setOverride = true;
        s_charSent = true;
        PhaseLog("Injected confirm (INPUT_A) through charsel lockstep");
    } else if (s_config.stage_mash_test && subState == CHARSEL_SUB_PREVIEW) {
        if (!s_stagePreviewSeen) {
            s_stagePreviewSeen = true;
            s_stagePreviewFrames = 0;
            s_stageConfirmSent = false;
            PhaseLog("Stage mash test entered preview at stage=%u", stageId);
        }

        if (s_stagePreviewFrames < s_config.stage_mash_frames) {
            overrideInput = BuildStageMashInput(s_stagePreviewFrames);
            setOverride = true;
        } else if (!s_stageConfirmSent) {
            overrideInput = INPUT_A;
            setOverride = true;
            s_stageConfirmSent = true;
            PhaseLog("Stage mash test confirming stage after %d preview frames (stage=%u)",
                     s_stagePreviewFrames,
                     stageId);
        }

        if (IsStageDebugEnabled() &&
            (((setOverride ? overrideInput : 0) != s_lastCharSelOverride) ||
             (s_stagePreviewFrames % 30) == 0)) {
            RecordStageEvent("HarnessMash",
                             (uint32_t)s_frameCounter,
                             s_stagePreviewFrames,
                             mode,
                             subState,
                             stageId,
                             overrideInput,
                             0,
                             0,
                             0,
                             0,
                             setOverride ? overrideInput : 0,
                             0);
        }

        s_stagePreviewFrames++;
    }

    // Re-inject A every 90 frames starting at frame 90 to also confirm stage select grid
    if (!setOverride && !s_stagePreviewSeen && s_phaseTimer > 90 && (s_phaseTimer % 90 == 0)) {
        overrideInput = INPUT_A;
        setOverride = true;
        PhaseLog("Re-injecting confirm at phaseTimer=%d (charsel sub=%u)", s_phaseTimer, RdU32(ADDR_SUB_STATE));
    }

    if (setOverride) {
        InputSystem_SetOverride(0, overrideInput);
        s_lastCharSelOverride = overrideInput;
    } else {
        InputSystem_ClearOverride(0);
        s_lastCharSelOverride = 0;
    }

    if (s_phaseTimer > 1800) {
        InputSystem_ClearOverride(0);
        s_lastCharSelOverride = 0;
        TransitionTo(Phase::Done, "timeout charsel lock");
    }
}

static void HandleInMatch() {
    SessionManager::FrameUpdate();

    if (s_matchStartFrame == 0) {
        s_matchStartFrame = s_frameCounter;
        PhaseLog("Match started at frame %u", s_matchStartFrame);
    }

    // ── Fighting Game AI ──────────────────────────────────────────
    // Deterministic per-frame AI that executes real fighting game
    // sequences: dashes, quarter circles, dragon punches, jumps,
    // blockstrings, throws, etc.  Each side runs its own AI state
    // machine; the rollback system syncs everything.

    static struct {
        enum Action : uint8_t {
            Idle, Approach, Retreat, Jump, AirAttack,
            QCF,  // Quarter circle forward (236 + button)
            DP,   // Dragon punch (623 + button)
            HCF,  // Half circle forward (41236 + button)
            DashForward, DashBack,
            Poke, Combo, Throw, Block, Wakeup
        };
        Action  action     = Idle;
        int     step       = 0;      // sub-frame within current action
        int     remaining  = 0;      // frames left in current action
        uint32_t rng       = 0;
    } ai;

    auto aiRand = [&]() -> uint32_t {
        ai.rng = ai.rng * 214013u + 2531011u;
        return (ai.rng >> 16) & 0x7FFF;
    };

    uint32_t matchFrame = s_frameCounter - s_matchStartFrame;
    if (matchFrame == 0) {
        ai = {};
        ai.rng = s_config.is_host ? 0xDEADBEEF : 0xCAFEBABE;
    }

    uint16_t input = 0;

    // Pick a new action when the current one finishes
    if (ai.remaining <= 0) {
        uint32_t r = aiRand() % 100;
        if      (r < 15) { ai.action = ai.Approach;    ai.remaining = 20 + (aiRand() % 30); }
        else if (r < 25) { ai.action = ai.Retreat;     ai.remaining = 15 + (aiRand() % 20); }
        else if (r < 35) { ai.action = ai.DashForward; ai.remaining = 10; }
        else if (r < 42) { ai.action = ai.DashBack;    ai.remaining = 10; }
        else if (r < 52) { ai.action = ai.Jump;        ai.remaining = 6; }
        else if (r < 58) { ai.action = ai.AirAttack;   ai.remaining = 20; }
        else if (r < 68) { ai.action = ai.Poke;        ai.remaining = 8 + (aiRand() % 10); }
        else if (r < 76) { ai.action = ai.Combo;       ai.remaining = 24; }
        else if (r < 82) { ai.action = ai.QCF;         ai.remaining = 8; }
        else if (r < 87) { ai.action = ai.DP;          ai.remaining = 8; }
        else if (r < 91) { ai.action = ai.HCF;         ai.remaining = 12; }
        else if (r < 95) { ai.action = ai.Throw;       ai.remaining = 6; }
        else if (r < 98) { ai.action = ai.Block;       ai.remaining = 20 + (aiRand() % 30); }
        else              { ai.action = ai.Wakeup;      ai.remaining = 10; }
        ai.step = 0;
    }

    // Direction toward opponent (host = P1 faces right, client = P2 faces left)
    const uint16_t FWD  = s_config.is_host ? INPUT_RIGHT : INPUT_LEFT;
    const uint16_t BACK = s_config.is_host ? INPUT_LEFT  : INPUT_RIGHT;

    switch (ai.action) {
    case ai.Idle:
        break;

    case ai.Approach:
        input = FWD;
        if (ai.step % 12 < 2) input |= INPUT_A;  // jab while walking
        break;

    case ai.Retreat:
        input = BACK;
        break;

    case ai.DashForward:
        // Double-tap: FWD, neutral, FWD, then hold
        if      (ai.step == 0) input = FWD;
        else if (ai.step == 1) input = 0;
        else if (ai.step == 2) input = FWD;
        else                   input = FWD;
        break;

    case ai.DashBack:
        if      (ai.step == 0) input = BACK;
        else if (ai.step == 1) input = 0;
        else if (ai.step == 2) input = BACK;
        else                   input = BACK;
        break;

    case ai.Jump:
        if (ai.step < 2)      input = INPUT_UP;
        else if (ai.step < 4) input = INPUT_UP | FWD;
        else                   input = FWD;
        break;

    case ai.AirAttack:
        // Jump forward then press buttons at the peak
        if      (ai.step < 2)  input = INPUT_UP | FWD;
        else if (ai.step < 5)  input = FWD;
        else if (ai.step == 5) input = FWD | INPUT_A;
        else if (ai.step == 8) input = FWD | INPUT_B;
        else if (ai.step ==11) input = FWD | INPUT_C;
        else                    input = 0;
        break;

    case ai.Poke:
        // Ground normals: alternate between buttons
        switch (ai.step % 8) {
        case 0: input = FWD | INPUT_A; break;
        case 2: input = FWD | INPUT_B; break;
        case 4: input = INPUT_DOWN | INPUT_A; break;   // crouching light
        case 6: input = INPUT_DOWN | INPUT_B; break;   // crouching medium
        default: input = 0; break;                      // recovery frames
        }
        break;

    case ai.Combo:
        // Ground chain: A → A → B → C (with slight gaps)
        switch (ai.step) {
        case 0:  input = FWD | INPUT_A; break;          // close jab
        case 3:  input = INPUT_A; break;                 // jab 2
        case 6:  input = INPUT_B; break;                 // medium
        case 10: input = FWD | INPUT_C; break;           // heavy launcher
        case 14: input = INPUT_DOWN | INPUT_C; break;    // sweep/knockdown
        // QCF+A cancel after the chain
        case 17: input = INPUT_DOWN; break;
        case 18: input = INPUT_DOWN | FWD; break;
        case 19: input = FWD | INPUT_A; break;
        default: input = 0; break;
        }
        break;

    case ai.QCF:
        // 236 + button: Down, Down-Forward, Forward + A
        switch (ai.step) {
        case 0: case 1: input = INPUT_DOWN; break;
        case 2: case 3: input = INPUT_DOWN | FWD; break;
        case 4:         input = FWD | INPUT_A; break;
        default:        input = 0; break;
        }
        break;

    case ai.DP:
        // 623 + button: Forward, Down, Down-Forward + B
        switch (ai.step) {
        case 0:         input = FWD; break;
        case 1: case 2: input = INPUT_DOWN; break;
        case 3:         input = INPUT_DOWN | FWD; break;
        case 4:         input = FWD | INPUT_B; break;
        default:        input = 0; break;
        }
        break;

    case ai.HCF:
        // 41236 + C: Back, Down-Back, Down, Down-Forward, Forward + C
        switch (ai.step) {
        case 0: case 1: input = BACK; break;
        case 2:         input = BACK | INPUT_DOWN; break;
        case 3: case 4: input = INPUT_DOWN; break;
        case 5:         input = INPUT_DOWN | FWD; break;
        case 6:         input = FWD | INPUT_C; break;
        default:        input = 0; break;
        }
        break;

    case ai.Throw:
        // Walk forward then press A+B simultaneously
        if      (ai.step < 2) input = FWD;
        else if (ai.step == 2) input = FWD | INPUT_A | INPUT_B;
        else                   input = 0;
        break;

    case ai.Block:
        // Hold back (occasionally crouch-block)
        input = BACK;
        if ((ai.step / 8) % 3 == 0) input |= INPUT_DOWN;  // mix crouch block
        break;

    case ai.Wakeup:
        // Mash A on wakeup, occasionally with DP motion
        if (ai.step < 3) {
            input = INPUT_A;
        } else {
            // Wakeup DP
            switch (ai.step - 3) {
            case 0: input = FWD; break;
            case 1: input = INPUT_DOWN; break;
            case 2: input = INPUT_DOWN | FWD; break;
            case 3: input = FWD | INPUT_C; break;
            default: input = 0; break;
            }
        }
        break;
    }

    ai.step++;
    ai.remaining--;
    InputSystem_SetOverride(0, input);

    // Deep snapshot every frame
    CaptureSnapshot(&s_latestSnapshot);
    s_stats.total_frames++;

    // Pull rollback stats from GekkoNet session
    static RollbackSession::Snapshot s_rbSnap = {};
    {
        RollbackSession::Snapshot rbSnap = {};
        if (RollbackSession::GetSnapshot(&rbSnap)) {
            s_rbSnap = rbSnap;
            s_stats.total_rollbacks   = rbSnap.rollback_count;
            s_stats.max_rollback_depth = (rbSnap.rollback_frames > (int)s_stats.max_rollback_depth)
                                         ? (uint32_t)rbSnap.rollback_frames
                                         : s_stats.max_rollback_depth;
            s_stats.total_saves       = rbSnap.save_count;
            s_stats.total_loads       = rbSnap.load_count;
        }
    }

    LogSnapshot(&s_latestSnapshot);

    // Log GekkoNet frame state every frame (critical for debugging stuck sessions)
    if (s_logFile && s_rbSnap.state != RollbackSession::State::Inactive) {
        LogLine("GEKKO | %u | local=%d remote=%d ahead=%.2f state=%u adv=%u rb=%u saves=%u loads=%u "
                "tx=%u rx=%u tsync=%u",
                s_latestSnapshot.frame,
                s_rbSnap.local_frame,
                s_rbSnap.remote_frame,
                s_rbSnap.frames_ahead,
                (uint32_t)s_rbSnap.state,
                s_rbSnap.advance_count,
                s_rbSnap.rollback_count,
                s_rbSnap.save_count,
                s_rbSnap.load_count,
                s_rbSnap.adapter_send_count,
                s_rbSnap.adapter_recv_count,
                s_rbSnap.timesync_skips);
    }

    // Check match end — detect both mode transition and substate 5 (MATCH_SUB_END)
    if (s_latestSnapshot.game_mode != (uint32_t)MODE_MATCH) {
        PhaseLog("Match ended at frame %u (mode changed to %u, duration=%u)",
                 s_frameCounter, s_latestSnapshot.game_mode,
                 s_frameCounter - s_matchStartFrame);
        TransitionTo(Phase::Done, "match ended");
        return;
    }
    if (s_latestSnapshot.sub_state == 5 /* MATCH_SUB_END */) {
        PhaseLog("Match finished at frame %u (substate=END, duration=%u)",
                 s_frameCounter, s_frameCounter - s_matchStartFrame);
        TransitionTo(Phase::Done, "match finished");
        return;
    }

    // Log round transitions (substate 2 = MATCH_SUB_INIT = new round starting)
    {
        static uint32_t s_prevSubState = 3;
        if (s_latestSnapshot.sub_state != s_prevSubState) {
            PhaseLog("Match substate changed: %u -> %u at frame %u",
                     s_prevSubState, s_latestSnapshot.sub_state, s_frameCounter);
            s_prevSubState = s_latestSnapshot.sub_state;
        }
    }

    // Safety timeout (very generous — 10 minutes; primary detection is above)
    if (s_config.match_duration_sec > 0) {
        int elapsed = (s_frameCounter - s_matchStartFrame) / 60;
        if (elapsed >= s_config.match_duration_sec) {
            PhaseLog("Safety timeout after %d seconds", elapsed);
            TransitionTo(Phase::Done, "duration limit");
            return;
        }
    }

    // Periodic stats to log
    if (s_phaseTimer > 0 && s_phaseTimer % 300 == 0) {
        SessionManager::Snapshot snap = {};
        if (SessionManager::GetSnapshot(&snap)) {
            LogLine("STATS | %d | rtt=%.1f sent=%u recv=%u desyncs=%u rb=%u sv=%u ld=%u anom=%u viol=%u "
                    "gekko_local=%d gekko_remote=%d ahead=%.2f gekko_state=%u gekko_adv=%u "
                    "adapter_tx=%u adapter_rx=%u timesync_skips=%u",
                    s_frameCounter, snap.rtt_ms, snap.packets_sent, snap.packets_received,
                    snap.desync_count, s_stats.total_rollbacks, s_stats.total_saves,
                    s_stats.total_loads, s_stats.frame_counter_anomalies,
                    s_stats.vanilla_flag_violations,
                    s_rbSnap.local_frame, s_rbSnap.remote_frame, s_rbSnap.frames_ahead,
                    (uint32_t)s_rbSnap.state, s_rbSnap.advance_count,
                    s_rbSnap.adapter_send_count, s_rbSnap.adapter_recv_count,
                    s_rbSnap.timesync_skips);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

bool Init() {
    s_phase = Phase::Idle;
    s_active = false;
    s_frameCounter = 0;
    s_matchStartFrame = 0;
    s_logFile = nullptr;
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_latestSnapshot, 0, sizeof(s_latestSnapshot));
    s_prevRngSeed = 0;
    s_sumSaveUs = s_sumLoadUs = s_sumAdvanceUs = 0;
    s_timingCount = 0;
    s_sumRtt = 0;
    s_rttCount = 0;
    s_prevSimFrame = s_prevDisplayFrame = 0;
    s_prevCharSelSubState = UINT32_MAX;
    s_prevStageId = UINT32_MAX;
    s_stagePreviewFrames = 0;
    s_stagePreviewSeen = false;
    s_stageConfirmSent = false;
    s_lastCharSelOverride = 0;

    if (!ParseConfigFile(kConfigFilename, &s_config)) return false;
    if (!s_config.enabled) {
        LOG_INFO("[Harness] Config found but disabled");
        return false;
    }

    s_active = true;
    SetVerboseLogging(true);
    AS2_SetExtraVerboseRngLogging(true);
    AS2_SetRollbackTraceLogging(true);
    OpenLog();
    CreateSharedMemory();

    PhaseLog("Harness ACTIVE: role=%s port=%u -> %s:%u char=%d del=%d stageMash=%d/%d stageLog=%d",
             s_config.is_host?"Host":"Client",
             s_config.listen_port, s_config.target_ip, s_config.target_port,
             s_config.character_id, s_config.delay_frames,
             s_config.stage_mash_test ? 1 : 0,
             s_config.stage_mash_frames,
             s_config.profiling.log_stage_debug ? 1 : 0);
    PhaseLog("Rollback trace enabled: text=%s bin=%s",
             AS2_GetRollbackTraceTextPath()[0] ? AS2_GetRollbackTraceTextPath() : "<unavailable>",
             AS2_GetRollbackTraceBinaryPath()[0] ? AS2_GetRollbackTraceBinaryPath() : "<unavailable>");

    // Network simulation config (logged for reference; ENet manages transport now)
    if (s_config.net_sim.latency_ms > 0 || s_config.net_sim.jitter_ms > 0 ||
        s_config.net_sim.packet_loss_pct > 0.0f || s_config.net_sim.duplicate_pct > 0.0f) {
        PhaseLog("NetSim config: latency=%dms jitter=%dms loss=%.1f%% dup=%.1f%% (NOTE: ENet netsim not yet implemented)",
                 s_config.net_sim.latency_ms, s_config.net_sim.jitter_ms,
                 s_config.net_sim.packet_loss_pct, s_config.net_sim.duplicate_pct);
    }

    TransitionTo(Phase::WaitingForGame, "config loaded");
    return true;
}

void FrameUpdate() {
    if (!s_active || s_phase == Phase::Done || s_phase == Phase::Idle) return;

    s_frameCounter++;
    s_phaseTimer++;
    // NOTE: UdpSocket::TickSimulation() removed — ENet manages transport now

    switch (s_phase) {
    case Phase::WaitingForGame:       HandleWaitingForGame();       break;
    case Phase::StartingSession:      HandleStartingSession();      break;
    case Phase::WaitingForConnection: HandleWaitingForConnection(); break;
    case Phase::LaunchingCharSel:     HandleLaunchingCharSel();     break;
    case Phase::WaitingForCharSel:    HandleWaitingForCharSel();    break;
    case Phase::AutoSelectingChar:    HandleAutoSelectingChar();     break;
    case Phase::WaitingForMatch:
        SessionManager::FrameUpdate();
        {
            uint32_t wm = RdU32(ADDR_GAME_MODE);
            if (wm == (uint32_t)MODE_MATCH)
                TransitionTo(Phase::InMatch, "match started");
            else if (wm != (uint32_t)MODE_STAGESEL && s_phaseTimer > 600)
                TransitionTo(Phase::Done, "timeout waiting for match");
            // MODE_STAGESEL is the cinematic before match — keep waiting
        }
        break;
    case Phase::InMatch:              HandleInMatch();               break;
    default: break;
    }

    if (s_phase != Phase::InMatch) {
        CaptureSnapshot(&s_latestSnapshot);
    }

    // Flush data to shared memory every frame
    FlushToSharedMemory();
}

void Shutdown() {
    if (s_logFile) {
        LogLine("#\n# === FINAL SUMMARY ===");
        LogLine("# Frames: %u  Rollbacks: %u (max %u)  Saves: %u  Loads: %u",
                s_stats.total_frames, s_stats.total_rollbacks,
                s_stats.max_rollback_depth, s_stats.total_saves, s_stats.total_loads);
        {
            RollbackSession::Snapshot rbFinal = {};
            if (RollbackSession::GetSnapshot(&rbFinal)) {
                LogLine("# GekkoNet: local=%d remote=%d ahead=%.2f adv=%u adapter_tx=%u adapter_rx=%u timesync_skips=%u",
                        rbFinal.local_frame, rbFinal.remote_frame, rbFinal.frames_ahead,
                        rbFinal.advance_count, rbFinal.adapter_send_count, rbFinal.adapter_recv_count,
                        rbFinal.timesync_skips);
            }
        }
        LogLine("# Desyncs: %u  VanillaViol: %u  FrameAnom: %u  RngJumps: %u",
                s_stats.desync_warnings, s_stats.vanilla_flag_violations,
                s_stats.frame_counter_anomalies, s_stats.rng_jumps);
        if (s_timingCount > 0)
            LogLine("# Avg save/load/adv: %.0f/%.0f/%.0f us  Peak: %.0f/%.0f/%.0f us",
                    s_sumSaveUs/s_timingCount, s_sumLoadUs/s_timingCount, s_sumAdvanceUs/s_timingCount,
                    s_stats.peak_save_us, s_stats.peak_load_us, s_stats.peak_advance_us);
        if (s_rttCount > 0)
            LogLine("# Avg/peak RTT: %.1f / %.1f ms", s_sumRtt/s_rttCount, s_stats.peak_rtt_ms);
        PhaseLog("Harness shutting down at frame %d", s_frameCounter);
        fflush(s_logFile);
        fclose(s_logFile);
        s_logFile = nullptr;
    }
    DestroySharedMemory();
    AS2_SetRollbackTraceLogging(false);
    AS2_SetExtraVerboseRngLogging(false);
    SetVerboseLogging(false);
    s_active = false;
    s_phase = Phase::Idle;
}

bool IsActive() { return s_active; }
bool InMatch()  { return s_active && s_phase == Phase::InMatch; }

float GetTimesyncThreshold() {
    return s_active ? s_config.net_sim.timesync_threshold : 0.0f;
}

// ============================================================================
// Profiling API (called from hooks)
// ============================================================================

void RecordFrameTiming(uint32_t frame, int saveUs, int loadUs, int advanceUs) {
    if (!s_active) return;
    s_sumSaveUs += saveUs; s_sumLoadUs += loadUs; s_sumAdvanceUs += advanceUs;
    s_timingCount++;
    if (saveUs > 0)  s_stats.total_saves++;
    if (loadUs > 0)  s_stats.total_loads++;
    if ((float)saveUs    > s_stats.peak_save_us)    s_stats.peak_save_us    = (float)saveUs;
    if ((float)loadUs    > s_stats.peak_load_us)    s_stats.peak_load_us    = (float)loadUs;
    if ((float)advanceUs > s_stats.peak_advance_us) s_stats.peak_advance_us = (float)advanceUs;
    if (s_timingCount > 0) {
        s_stats.avg_save_us    = (float)(s_sumSaveUs    / s_timingCount);
        s_stats.avg_load_us    = (float)(s_sumLoadUs    / s_timingCount);
        s_stats.avg_advance_us = (float)(s_sumAdvanceUs / s_timingCount);
    }
    if (s_config.profiling.log_frame_timing)
        LogLine("TIMING | %u | save=%d load=%d advance=%d", frame, saveUs, loadUs, advanceUs);
}

void RecordRollback(uint32_t frame, int depth, int replayFrames) {
    if (!s_active) return;
    s_stats.total_rollbacks++;
    if ((uint32_t)depth > s_stats.max_rollback_depth) s_stats.max_rollback_depth = (uint32_t)depth;
    if (s_config.profiling.log_rollbacks)
        LogLine("ROLLBACK | %u | depth=%d replay=%d", frame, depth, replayFrames);
    ShmLog("ROLLBACK f=%u d=%d r=%d", frame, depth, replayFrames);
}

void RecordInputs(uint32_t frame, uint16_t p1, uint16_t p2) {
    if (!s_active) return;
    if (s_config.profiling.log_inputs) {
        LogLine("RBINPUT | %u | p1=0x%04X p2=0x%04X", frame, p1, p2);
    }
}

void RecordChecksum(uint32_t frame, uint32_t checksum) {
    if (!s_active) return;
    if (s_config.profiling.log_checksums) {
        LogLine("RBCHECK | %u | 0x%08X", frame, checksum);
    }
}

void RecordRtt(uint32_t frame, float rttMs) {
    if (!s_active) return;
    s_sumRtt += rttMs; s_rttCount++;
    if (rttMs > s_stats.peak_rtt_ms) s_stats.peak_rtt_ms = rttMs;
    s_stats.avg_rtt_ms = (float)(s_sumRtt / s_rttCount);
    if (s_config.profiling.log_rtt) LogLine("RTT | %u | %.2f", frame, rttMs);
}

void RecordDesync(uint32_t frame, uint32_t localChecksum, uint32_t remoteChecksum, const char* source) {
    if (!s_active) return;
    s_stats.desync_warnings++;
    LogLine("DESYNC | %u | src=%s local=0x%08X remote=0x%08X",
            frame,
            source ? source : "unknown",
            localChecksum,
            remoteChecksum);
    ShmLog("DESYNC f=%u src=%s local=%08X remote=%08X",
           frame,
           source ? source : "unknown",
           localChecksum,
           remoteChecksum);
}

void RecordStageEvent(const char* source, uint32_t gameFrame, int32_t lockstepFrame,
                      uint32_t mode, uint32_t subState, uint8_t stageId,
                      uint16_t localInput, uint16_t remoteInput,
                      uint16_t p1Input, uint16_t p2Input,
                      uint16_t sharedInput,
                      uint16_t appliedP1, uint16_t appliedP2) {
    if (!s_active || !IsStageDebugEnabled()) return;
    if (!source) source = "?";

    LogLine("STAGE | %u | src=%s lf=%d mode=%u sub=%u stage=%u local=0x%04X remote=0x%04X p1=0x%04X p2=0x%04X shared=0x%04X ap1=0x%04X ap2=0x%04X",
            gameFrame, source, lockstepFrame, mode, subState, stageId,
            localInput, remoteInput, p1Input, p2Input, sharedInput, appliedP1, appliedP2);
    ShmLog("STAGE f=%u src=%s lf=%d sub=%u stg=%u loc=%04X rem=%04X p1=%04X p2=%04X sh=%04X ap=%04X/%04X",
           gameFrame, source, lockstepFrame, subState, stageId,
           localInput, remoteInput, p1Input, p2Input, sharedInput, appliedP1, appliedP2);
}

void FlushLog() {
    if (s_logFile) fflush(s_logFile);
}

} // namespace TestHarness
