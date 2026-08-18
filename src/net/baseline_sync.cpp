/**
 * Alice Senki 2 - Baseline Sync Diagnostics
 *
 * Centralized baseline breakdown capture/comparison/dump path used by
 * match_bootstrap.cpp.
 */

#include "net/baseline_sync.h"

#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "rollback/desync_dump.h"
#include "rollback/determinism_verify.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

static bool s_haveLocalBreakdown = false;
static bool s_haveRemoteBreakdown = false;
static BaselineBreakdownPayload s_localBreakdown{};
static BaselineBreakdownPayload s_remoteBreakdown{};
static uint32_t s_localBreakdownCount = 0;
static uint32_t s_remoteBreakdownCount = 0;
static uint32_t s_mismatchDumpSequence = 0;

constexpr size_t kBaselineHashMainSize = (ADDR_P2_ENTITY_BASE + ENTITY_SIZE) - ADDR_MATCH_BASE;
constexpr size_t kBaselineHashHeaderSize = 16;
constexpr size_t kBaselineHashContextSize = ADDR_EFFECT_ARRAY - (ADDR_MATCH_BASE + kBaselineHashHeaderSize);
constexpr size_t kBaselineHashEffectSize = ADDR_SUMMON_ARRAY - ADDR_EFFECT_ARRAY;
constexpr size_t kBaselineHashSummonSize = ADDR_P1_ENTITY_BASE - ADDR_SUMMON_ARRAY;
// F10a: match+12 is the first prm.bin sprite handle, and its LOW word
// (bytes 12..13) is sub_612DF0's free-slot index -- process history, not
// gameplay. The agreement CRC must stop at 12 or its mismatch dumps point at a
// phantom. Diagnostic only: the actual gate is the masked digest, and nothing
// branches on header_agreement_crc.
constexpr size_t kAgreementHeaderStableBytes = 12;
constexpr size_t kAgreementEntityStablePrefixSize = 0x07D0;
constexpr size_t kAgreementEntityAnimDataOffset = 0x1000;
constexpr size_t kAgreementEntityAnimDataSize = 0x9000;
constexpr size_t kAgreementEntityCharStateOffset = 0xA000;
constexpr size_t kAgreementEntityCharStateSize = 0x0660;
constexpr size_t kEntityInputBankBaseOffset = 0x0008;
constexpr size_t kEntityInputBankCount = 6;
constexpr size_t kEntityInputBankWordCount = 14;
constexpr size_t kEntityInputDerivedFirstWord = 10;
constexpr size_t kEntityInputDerivedWordCount = 4;
constexpr size_t kEntityInputBankStrideBytes = kEntityInputBankWordCount * sizeof(uint16_t);
constexpr size_t kEntityInputDerivedBytesPerBank = kEntityInputDerivedWordCount * sizeof(uint16_t);
constexpr size_t kEntityInputDerivedMaskedBytes =
    kEntityInputBankCount * kEntityInputDerivedBytesPerBank;

static_assert(kAgreementEntityStablePrefixSize == ENTITY_COMBAT_END,
              "baseline entity agreement prefix must match the verified combat-state window");
static_assert(kEntityInputDerivedMaskedBytes == 48,
              "expected six 8-byte derived input-cache slices per entity");
static_assert(kEntityInputBankBaseOffset +
                  (kEntityInputBankCount - 1) * kEntityInputBankStrideBytes +
                  (kEntityInputDerivedFirstWord + kEntityInputDerivedWordCount) * sizeof(uint16_t)
              <= kAgreementEntityStablePrefixSize,
              "derived input-cache mask must stay inside the agreement prefix");

static uint32_t SafeRegionCRC(uintptr_t address, size_t size) {
    __try {
        return CalcCRC32(reinterpret_cast<const void*>(address), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xDEADDEAD;
    }
}

static void SafeCopyBytes(uint8_t* dst, uintptr_t src, size_t size) {
    if (!dst || size == 0) return;
    memset(dst, 0, size);
    __try {
        memcpy(dst, reinterpret_cast<const void*>(src), size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Keep zeroed bytes; this still makes failure visible in logs.
    }
}

static uint32_t BuildHeaderAgreementCRC() {
    uint8_t stableBytes[kAgreementHeaderStableBytes] = {};
    SafeCopyBytes(stableBytes, ADDR_MATCH_BASE, sizeof(stableBytes));
    return CalcCRC32(stableBytes, sizeof(stableBytes));
}

static const char* EntityAgreementLabel(uintptr_t entityBase) {
    if (entityBase == ADDR_P1_ENTITY_BASE) return "P1";
    if (entityBase == ADDR_P2_ENTITY_BASE) return "P2";
    return "entity";
}

static size_t EntityInputDerivedOffset(size_t bank) {
    return kEntityInputBankBaseOffset +
           bank * kEntityInputBankStrideBytes +
           kEntityInputDerivedFirstWord * sizeof(uint16_t);
}

static uint32_t BuildEntityDerivedInputCacheCRC(const uint8_t* stablePrefix, size_t size) {
    uint8_t derivedBytes[kEntityInputDerivedMaskedBytes] = {};
    if (!stablePrefix || size < kAgreementEntityStablePrefixSize) {
        return CalcCRC32(derivedBytes, sizeof(derivedBytes));
    }

    for (size_t bank = 0; bank < kEntityInputBankCount; ++bank) {
        const size_t srcOffset = EntityInputDerivedOffset(bank);
        const size_t dstOffset = bank * kEntityInputDerivedBytesPerBank;
        memcpy(derivedBytes + dstOffset,
               stablePrefix + srcOffset,
               kEntityInputDerivedBytesPerBank);
    }

    return CalcCRC32(derivedBytes, sizeof(derivedBytes));
}

static void NormalizeEntityInputDerivedCache(uint8_t* stablePrefix, size_t size) {
    if (!stablePrefix || size < kAgreementEntityStablePrefixSize) {
        return;
    }

    for (size_t bank = 0; bank < kEntityInputBankCount; ++bank) {
        memset(stablePrefix + EntityInputDerivedOffset(bank),
               0,
               kEntityInputDerivedBytesPerBank);
    }
}

static uint32_t BuildEntityStablePrefixAgreementCRC(uintptr_t entityBase) {
    uint8_t stablePrefix[kAgreementEntityStablePrefixSize] = {};
    SafeCopyBytes(stablePrefix, entityBase, sizeof(stablePrefix));

    const uint32_t rawPrefixCrc = CalcCRC32(stablePrefix, sizeof(stablePrefix));
    const uint32_t derivedCacheCrc =
        BuildEntityDerivedInputCacheCRC(stablePrefix, sizeof(stablePrefix));

    // The game shifts and clears only the first ten words in each entity input
    // bank during match bootstrap. Words 10-13 are derived caches that are
    // recomputed by native gameplay after the baseline point, so hash a local
    // normalized copy instead of clearing live game memory.
    NormalizeEntityInputDerivedCache(stablePrefix, sizeof(stablePrefix));

    const uint32_t normalizedPrefixCrc = CalcCRC32(stablePrefix, sizeof(stablePrefix));
    Rollback::NetplayLog_Write(
        "BASELINE", -1,
        "Entity agreement normalization: entity=%s base=0x%08X stable_raw=0x%08X stable_norm=0x%08X derived_cache=0x%08X masked_bytes=%u banks=%u bank_words=%u derived_words=%u live_write=0",
        EntityAgreementLabel(entityBase),
        (uint32_t)entityBase,
        rawPrefixCrc,
        normalizedPrefixCrc,
        derivedCacheCrc,
        (unsigned)kEntityInputDerivedMaskedBytes,
        (unsigned)kEntityInputBankCount,
        (unsigned)kEntityInputBankWordCount,
        (unsigned)kEntityInputDerivedWordCount);
    return normalizedPrefixCrc;
}

static uint32_t BuildEntityAgreementCRC(uintptr_t entityBase) {
    struct EntityAgreementParts {
        uint32_t stable_prefix_crc;
        uint32_t anim_data_crc;
        uint32_t char_state_crc;
    } parts{};

    // Exclude the timers/handle window (0x7D0-0x1000) and the large tail
    // region (0xA660-end). Those ranges currently pick up process-local
    // handle/resource allocations during match bootstrap.
    parts.stable_prefix_crc = BuildEntityStablePrefixAgreementCRC(entityBase);
    parts.anim_data_crc = SafeRegionCRC(entityBase + kAgreementEntityAnimDataOffset,
                                        kAgreementEntityAnimDataSize);
    parts.char_state_crc = SafeRegionCRC(entityBase + kAgreementEntityCharStateOffset,
                                         kAgreementEntityCharStateSize);
    return CalcCRC32(&parts, sizeof(parts));
}

static void BytesToHex(const uint8_t* bytes, size_t len, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!bytes || len == 0) return;

    size_t pos = 0;
    for (size_t i = 0; i < len; ++i) {
        const int wrote = _snprintf_s(
            out + pos,
            (pos < outSize) ? (outSize - pos) : 0,
            _TRUNCATE,
            (i == 0) ? "%02X" : " %02X",
            (unsigned)bytes[i]);
        if (wrote <= 0) {
            break;
        }
        pos += (size_t)wrote;
        if (pos >= outSize) {
            out[outSize - 1] = '\0';
            break;
        }
    }
}

static void WriteBytesLine(FILE* f, const char* label, const uint8_t* bytes, size_t len) {
    if (!f) return;
    char hex[512];
    BytesToHex(bytes, len, hex, sizeof(hex));
    fprintf(f, "%s%s\n", label ? label : "", hex);
}

static void WriteBreakdownSection(FILE* f, const char* label, const BaselineBreakdownPayload& p) {
    if (!f) return;
    fprintf(f, "\n[%s]\n", label ? label : "breakdown");
    fprintf(f, "main=0x%08X header=0x%08X context=0x%08X effects=0x%08X summons=0x%08X p1=0x%08X p2=0x%08X\n",
            p.main_crc, p.header_crc, p.context_crc, p.effect_crc, p.summon_crc, p.p1_entity_crc, p.p2_entity_crc);
    fprintf(f, "agreement: header=0x%08X p1=0x%08X p2=0x%08X\n",
            p.header_agreement_crc, p.p1_agreement_crc, p.p2_agreement_crc);
    fprintf(f, "pre_gap=0x%08X p1_input=0x%08X p2_input=0x%08X temp=0x%08X\n",
            p.pre_match_gap_crc, p.p1_input_crc, p.p2_input_crc, p.per_frame_temp_crc);
    fprintf(f, "rng=0x%08X sim=%u display=%u mode=%u sub=%u type=%u phase_timer=%u\n",
            p.rng_seed, p.sim_frame, p.display_frame, p.game_mode, p.substate, p.game_type, p.match_phase_timer);
    fprintf(f, "vanilla(sync lineage): frame_sim=%u frame_display=%u frame_write=%u frame_last_sent/net=%u remote_frame=%u\n",
            p.frame_simulation, p.frame_display, p.frame_write_idx, p.frame_net_idx, p.remote_frame_idx);
    WriteBytesLine(f, "match_header_bytes: ", p.match_header_bytes, sizeof(p.match_header_bytes));
    WriteBytesLine(f, "pre_match_gap_bytes: ", p.pre_match_gap_bytes, sizeof(p.pre_match_gap_bytes));
}

static void LogBreakdownToNetplayLog(const char* label,
                                     const BaselineSyncStateView& state,
                                     const BaselineBreakdownPayload& p,
                                     uint32_t referenceCrc) {
    const int32_t frame = (int32_t)p.sim_frame;
    const bool refMatch = (referenceCrc != 0 && referenceCrc == p.main_crc);

    Rollback::NetplayLog_Write(
        "BASELINE", frame,
        "%s checked: phase=%s main=0x%08X header=0x%08X context=0x%08X effects=0x%08X summons=0x%08X p1=0x%08X p2=0x%08X agree_header=0x%08X agree_p1=0x%08X agree_p2=0x%08X ref=0x%08X ref_match=%d",
        label ? label : "Baseline",
        state.phase_name ? state.phase_name : "?",
        p.main_crc,
        p.header_crc,
        p.context_crc,
        p.effect_crc,
        p.summon_crc,
        p.p1_entity_crc,
        p.p2_entity_crc,
        p.header_agreement_crc,
        p.p1_agreement_crc,
        p.p2_agreement_crc,
        referenceCrc,
        refMatch ? 1 : 0);

    Rollback::NetplayLog_Write(
        "BASELINE", frame,
        "%s adjacent: pre_gap=0x%08X p1_input=0x%08X p2_input=0x%08X temp=0x%08X rng=0x%08X sim=%u display=%u mode=%u sub=%u type=%u phase_timer=%u frame_sim=%u frame_display=%u frame_write=%u frame_last_sent/net=%u remote_frame=%u local_loaded=%d remote_loaded=%d local_ready=%d remote_ready=%d digest_sent=%d agreed=%d",
        label ? label : "Baseline",
        p.pre_match_gap_crc,
        p.p1_input_crc,
        p.p2_input_crc,
        p.per_frame_temp_crc,
        p.rng_seed,
        p.sim_frame,
        p.display_frame,
        p.game_mode,
        p.substate,
        p.game_type,
        p.match_phase_timer,
        p.frame_simulation,
        p.frame_display,
        p.frame_write_idx,
        p.frame_net_idx,
        p.remote_frame_idx,
        state.local_loaded ? 1 : 0,
        state.remote_loaded ? 1 : 0,
        state.local_ready ? 1 : 0,
        state.remote_ready ? 1 : 0,
        state.digest_sent ? 1 : 0,
        state.baseline_agreed ? 1 : 0);

    char headerHex[64];
    char gapHex[64];
    BytesToHex(p.match_header_bytes, sizeof(p.match_header_bytes), headerHex, sizeof(headerHex));
    BytesToHex(p.pre_match_gap_bytes, sizeof(p.pre_match_gap_bytes), gapHex, sizeof(gapHex));
    Rollback::NetplayLog_Write(
        "BASELINE", frame,
        "%s bytes: match_header=[%s] pre_match_gap=[%s]",
        label ? label : "Baseline",
        headerHex,
        gapHex);
}

static void WriteMismatchDetailFile(const BaselineSyncStateView& state,
                                    const BaselineBreakdownPayload& local,
                                    bool haveLocal,
                                    const BaselineBreakdownPayload& remote,
                                    bool haveRemote) {
    const char* logDir = LogWindow_GetLogDir();
    if (!logDir || !logDir[0]) {
        return;
    }

    const int32_t frame = haveLocal ? (int32_t)local.sim_frame : state.local_sim_frame;
    const DWORD pid = GetCurrentProcessId();
    s_mismatchDumpSequence++;

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\baseline_sync_compare_%lu_f%d_n%u.txt",
                logDir, (unsigned long)pid, frame, (unsigned)s_mismatchDumpSequence);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        LOG_ERROR("[BaselineSync] Failed to write baseline compare dump: %s", path);
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "================================================================================\n");
    fprintf(f, "BASELINE SYNC MISMATCH DETAIL DUMP\n");
    fprintf(f, "================================================================================\n");
    fprintf(f, "Time: %04d-%02d-%02d %02d:%02d:%02d.%03d\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(f, "PID: %lu\n", (unsigned long)pid);
    fprintf(f, "Phase: %s\n", state.phase_name ? state.phase_name : "?");
    fprintf(f, "Session seed: 0x%08X\n", state.session_seed);
    fprintf(f, "State: local_loaded=%d remote_loaded=%d local_ready=%d remote_ready=%d digest_sent=%d agreed=%d\n",
            state.local_loaded ? 1 : 0,
            state.remote_loaded ? 1 : 0,
            state.local_ready ? 1 : 0,
            state.remote_ready ? 1 : 0,
            state.digest_sent ? 1 : 0,
            state.baseline_agreed ? 1 : 0);
    fprintf(f, "Baseline digest: local=0x%08X remote=0x%08X\n", state.local_crc, state.remote_crc);
    fprintf(f, "Local capture: mode=%u sub=%u sim=%d\n",
            (unsigned)state.local_mode, (unsigned)state.local_substate, state.local_sim_frame);
    fprintf(f, "Remote capture: mode=%u sub=%u sim=%d\n",
            (unsigned)state.remote_mode, (unsigned)state.remote_substate, state.remote_sim_frame);
    fprintf(f, "\nDecomp lineage reference (sub_562550 Netplay_InitialSync):\n");
    fprintf(f, "  Frame_Simulation dword_816490 @0x%08X\n", (unsigned)ADDR_FRAME_SIMULATION);
    fprintf(f, "  Frame_Display    dword_816494 @0x%08X\n", (unsigned)ADDR_FRAME_DISPLAY);
    fprintf(f, "  Frame_WriteIndex dword_816498 @0x%08X\n", (unsigned)ADDR_FRAME_WRITE_IDX);
    fprintf(f, "  Frame_LastSent   dword_81649C @0x%08X (mirrored as net_idx)\n", (unsigned)ADDR_FRAME_NET_IDX);
    fprintf(f, "  RemoteFrame      dword_87FC20 @0x%08X\n", (unsigned)ADDR_REMOTE_FRAME);
    fprintf(f, "  LocalInputHist   word_8164A0  @0x%08X size=%u\n", (unsigned)ADDR_VANILLA_LOCAL_INPUTS, (unsigned)VANILLA_LOCAL_INPUT_SIZE);
    fprintf(f, "  RemoteInputHist  word_87FC24  @0x%08X size=%u\n", (unsigned)ADDR_VANILLA_REMOTE_INPUTS, (unsigned)VANILLA_REMOTE_INPUT_SIZE);
    fprintf(f, "  SyncFlagsLocal   @0x%08X\n", (unsigned)ADDR_VANILLA_SYNC_LOCAL);
    fprintf(f, "  SyncFlagsRemote  @0x%08X\n", (unsigned)ADDR_VANILLA_SYNC_REMOTE);

    if (haveLocal) {
        WriteBreakdownSection(f, "LOCAL_BREAKDOWN", local);
    } else {
        fprintf(f, "\n[LOCAL_BREAKDOWN]\nmissing\n");
    }
    if (haveRemote) {
        WriteBreakdownSection(f, "REMOTE_BREAKDOWN", remote);
    } else {
        fprintf(f, "\n[REMOTE_BREAKDOWN]\nmissing (peer did not send BaselineBreakdown)\n");
    }

    fprintf(f, "\n[COMPARE]\n");
    if (haveLocal && haveRemote) {
        struct FieldCmp {
            const char* name;
            uint32_t local;
            uint32_t remote;
        };
        const FieldCmp fields[] = {
            {"main_crc", local.main_crc, remote.main_crc},
            {"header_crc", local.header_crc, remote.header_crc},
            {"context_crc", local.context_crc, remote.context_crc},
            {"effect_crc", local.effect_crc, remote.effect_crc},
            {"summon_crc", local.summon_crc, remote.summon_crc},
            {"p1_entity_crc", local.p1_entity_crc, remote.p1_entity_crc},
            {"p2_entity_crc", local.p2_entity_crc, remote.p2_entity_crc},
            {"header_agreement_crc", local.header_agreement_crc, remote.header_agreement_crc},
            {"p1_agreement_crc", local.p1_agreement_crc, remote.p1_agreement_crc},
            {"p2_agreement_crc", local.p2_agreement_crc, remote.p2_agreement_crc},
            {"pre_match_gap_crc", local.pre_match_gap_crc, remote.pre_match_gap_crc},
            {"p1_input_crc", local.p1_input_crc, remote.p1_input_crc},
            {"p2_input_crc", local.p2_input_crc, remote.p2_input_crc},
            {"per_frame_temp_crc", local.per_frame_temp_crc, remote.per_frame_temp_crc},
            {"rng_seed", local.rng_seed, remote.rng_seed},
            {"sim_frame", local.sim_frame, remote.sim_frame},
            {"display_frame", local.display_frame, remote.display_frame},
            {"game_mode", local.game_mode, remote.game_mode},
            {"substate", local.substate, remote.substate},
            {"game_type", local.game_type, remote.game_type},
            {"match_phase_timer", local.match_phase_timer, remote.match_phase_timer},
            {"frame_simulation", local.frame_simulation, remote.frame_simulation},
            {"frame_display", local.frame_display, remote.frame_display},
            {"frame_write_idx", local.frame_write_idx, remote.frame_write_idx},
            {"frame_net_idx/last_sent", local.frame_net_idx, remote.frame_net_idx},
            {"remote_frame_idx", local.remote_frame_idx, remote.remote_frame_idx},
        };

        for (const FieldCmp& field : fields) {
            fprintf(f, "  %-24s local=0x%08X remote=0x%08X %s\n",
                    field.name,
                    field.local,
                    field.remote,
                    (field.local == field.remote) ? "MATCH" : "DIFF");
        }
    } else {
        fprintf(f, "  Incomplete compare (local=%d remote=%d)\n", haveLocal ? 1 : 0, haveRemote ? 1 : 0);
    }

    fclose(f);
    LOG_INFO("[BaselineSync] Baseline compare dump written: %s", path);
}

static BaselineBreakdownPayload CaptureCurrentBreakdown() {
    BaselineBreakdownPayload out{};
    out.main_crc = SafeRegionCRC(ADDR_MATCH_BASE, kBaselineHashMainSize);
    out.header_crc = SafeRegionCRC(ADDR_MATCH_BASE, kBaselineHashHeaderSize);
    out.context_crc = SafeRegionCRC(ADDR_MATCH_BASE + kBaselineHashHeaderSize, kBaselineHashContextSize);
    out.effect_crc = SafeRegionCRC(ADDR_EFFECT_ARRAY, kBaselineHashEffectSize);
    out.summon_crc = SafeRegionCRC(ADDR_SUMMON_ARRAY, kBaselineHashSummonSize);
    out.p1_entity_crc = SafeRegionCRC(ADDR_P1_ENTITY_BASE, ENTITY_SIZE);
    out.p2_entity_crc = SafeRegionCRC(ADDR_P2_ENTITY_BASE, ENTITY_SIZE);
    out.header_agreement_crc = BuildHeaderAgreementCRC();
    out.p1_agreement_crc = BuildEntityAgreementCRC(ADDR_P1_ENTITY_BASE);
    out.p2_agreement_crc = BuildEntityAgreementCRC(ADDR_P2_ENTITY_BASE);

    out.pre_match_gap_crc = SafeRegionCRC(ADDR_PRE_MATCH_GAP, PRE_MATCH_GAP_SIZE);
    // Stable tail only (charsel committed data + title word): the first 76
    // bytes are the pass-cadence live-word window (button blocks +
    // just-pressed) that the vanilla updater rewrites per side — the same
    // window the gameplay digest masks (game_snapshot.cpp F7b). The two
    // peers compute their breakdowns at different wall instants, so any
    // volatile byte here would false-fail the baseline rendezvous.
    constexpr size_t kInputSpanStableOffset = 76;
    out.p1_input_crc = SafeRegionCRC(ADDR_P1_INPUT_BUFFER + kInputSpanStableOffset,
                                     INPUT_BUFFER_SIZE - kInputSpanStableOffset);
    out.p2_input_crc = SafeRegionCRC(ADDR_P2_INPUT_BUFFER + kInputSpanStableOffset,
                                     INPUT_BUFFER_SIZE - kInputSpanStableOffset);
    out.per_frame_temp_crc = SafeRegionCRC(ADDR_MATCH_PER_FRAME_TEMP, MATCH_PER_FRAME_TEMP_SIZE);

    out.rng_seed = DetVer_GetRngSeed();
    out.sim_frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    out.display_frame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    out.game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    out.substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    out.game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    out.match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);

    // Names from sub_562550 / sub_5625E0 lineage notes.
    out.frame_simulation = ReadMemory<uint32_t>(ADDR_FRAME_SIMULATION);
    out.frame_display = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
    out.frame_write_idx = ReadMemory<uint32_t>(ADDR_FRAME_WRITE_IDX);
    out.frame_net_idx = ReadMemory<uint32_t>(ADDR_FRAME_NET_IDX);
    out.remote_frame_idx = ReadMemory<uint32_t>(ADDR_REMOTE_FRAME);

    SafeCopyBytes(out.match_header_bytes, ADDR_MATCH_BASE, sizeof(out.match_header_bytes));
    SafeCopyBytes(out.pre_match_gap_bytes, ADDR_PRE_MATCH_GAP, sizeof(out.pre_match_gap_bytes));
    return out;
}

} // namespace

namespace Net {

void BaselineSync_Reset() {
    s_haveLocalBreakdown = false;
    s_haveRemoteBreakdown = false;
    memset(&s_localBreakdown, 0, sizeof(s_localBreakdown));
    memset(&s_remoteBreakdown, 0, sizeof(s_remoteBreakdown));
    s_localBreakdownCount = 0;
    s_remoteBreakdownCount = 0;
}

void BaselineSync_LogBegin(const BaselineSyncStateView& state) {
    Rollback::NetplayLog_Write(
        "BASELINE", state.local_sim_frame,
        "Begin baseline sync: phase=%s local_ready=%d remote_ready=%d local_crc=0x%08X remote_crc=0x%08X local=%u/%u/%d remote=%u/%u/%d",
        state.phase_name ? state.phase_name : "?",
        state.local_ready ? 1 : 0,
        state.remote_ready ? 1 : 0,
        state.local_crc,
        state.remote_crc,
        state.local_mode,
        state.local_substate,
        state.local_sim_frame,
        state.remote_mode,
        state.remote_substate,
        state.remote_sim_frame);
    Rollback::NetplayLog_Write(
        "BASELINE", state.local_sim_frame,
        "Coverage map: checked main=[0x%08X,+0x%zX] header=[0x%08X,+0x%zX] context=[0x%08X,+0x%zX] effects=[0x%08X,+0x%zX] summons=[0x%08X,+0x%zX] p1=[0x%08X,+0x%X] p2=[0x%08X,+0x%X]",
        (uint32_t)ADDR_MATCH_BASE,
        kBaselineHashMainSize,
        (uint32_t)ADDR_MATCH_BASE,
        kBaselineHashHeaderSize,
        (uint32_t)(ADDR_MATCH_BASE + kBaselineHashHeaderSize),
        kBaselineHashContextSize,
        (uint32_t)ADDR_EFFECT_ARRAY,
        kBaselineHashEffectSize,
        (uint32_t)ADDR_SUMMON_ARRAY,
        kBaselineHashSummonSize,
        (uint32_t)ADDR_P1_ENTITY_BASE,
        ENTITY_SIZE,
        (uint32_t)ADDR_P2_ENTITY_BASE,
        ENTITY_SIZE);
    Rollback::NetplayLog_Write(
        "BASELINE", state.local_sim_frame,
        "Adjacent diagnostics: pre_gap=[0x%08X,+0x%X] p1_input=[0x%08X,+0x%X] p2_input=[0x%08X,+0x%X] temp=[0x%08X,+0x%X] counters(Frame_Simulation@0x%08X Frame_Display@0x%08X Frame_WriteIndex@0x%08X Frame_LastSent/NetIdx@0x%08X RemoteFrame@0x%08X from sub_562550)",
        (uint32_t)ADDR_PRE_MATCH_GAP,
        PRE_MATCH_GAP_SIZE,
        (uint32_t)ADDR_P1_INPUT_BUFFER,
        INPUT_BUFFER_SIZE,
        (uint32_t)ADDR_P2_INPUT_BUFFER,
        INPUT_BUFFER_SIZE,
        (uint32_t)ADDR_MATCH_PER_FRAME_TEMP,
        MATCH_PER_FRAME_TEMP_SIZE,
        (uint32_t)ADDR_FRAME_SIMULATION,
        (uint32_t)ADDR_FRAME_DISPLAY,
        (uint32_t)ADDR_FRAME_WRITE_IDX,
        (uint32_t)ADDR_FRAME_NET_IDX,
        (uint32_t)ADDR_REMOTE_FRAME);
}

void BaselineSync_CaptureLocalBreakdown(BaselineBreakdownPayload* out) {
    if (!out) return;
    *out = CaptureCurrentBreakdown();
}

void BaselineSync_RecordLocalBreakdown(const BaselineSyncStateView& state,
                                       const BaselineBreakdownPayload& payload,
                                       uint32_t reference_crc) {
    s_localBreakdown = payload;
    s_haveLocalBreakdown = true;
    s_localBreakdownCount++;
    LogBreakdownToNetplayLog("LocalBaselineBreakdown", state, payload, reference_crc);
}

void BaselineSync_RecordRemoteBreakdown(const BaselineSyncStateView& state,
                                        const BaselineBreakdownPayload& payload) {
    s_remoteBreakdown = payload;
    s_haveRemoteBreakdown = true;
    s_remoteBreakdownCount++;
    LogBreakdownToNetplayLog("RemoteBaselineBreakdown", state, payload, state.remote_crc);
}

void BaselineSync_LogRemoteDigest(const BaselineSyncStateView& state,
                                  uint32_t remote_crc) {
    Rollback::NetplayLog_Write(
        "BASELINE", state.remote_sim_frame,
        "Remote BaselineDigest: crc=0x%08X phase=%s remote_ready=%d remote=%u/%u/%d local_crc=0x%08X digest_sent=%d",
        remote_crc,
        state.phase_name ? state.phase_name : "?",
        state.remote_ready ? 1 : 0,
        state.remote_mode,
        state.remote_substate,
        state.remote_sim_frame,
        state.local_crc,
        state.digest_sent ? 1 : 0);
}

void BaselineSync_LogMismatchAndDump(const BaselineSyncStateView& state) {
    const BaselineBreakdownPayload current = CaptureCurrentBreakdown();
    if (!s_haveLocalBreakdown) {
        s_localBreakdown = current;
        s_haveLocalBreakdown = true;
    }

    Rollback::NetplayLog_Write(
        "BASELINE", state.local_sim_frame,
        "Baseline mismatch detected: local_crc=0x%08X remote_crc=0x%08X phase=%s local=%u/%u/%d remote=%u/%u/%d local_breakdown=%d remote_breakdown=%d",
        state.local_crc,
        state.remote_crc,
        state.phase_name ? state.phase_name : "?",
        state.local_mode,
        state.local_substate,
        state.local_sim_frame,
        state.remote_mode,
        state.remote_substate,
        state.remote_sim_frame,
        s_haveLocalBreakdown ? 1 : 0,
        s_haveRemoteBreakdown ? 1 : 0);

    LogBreakdownToNetplayLog("CurrentMismatchSnapshot", state, current, state.local_crc);

    if (s_haveRemoteBreakdown) {
        struct FieldCmp {
            const char* name;
            uint32_t local;
            uint32_t remote;
        };
        const FieldCmp fields[] = {
            {"main_crc", s_localBreakdown.main_crc, s_remoteBreakdown.main_crc},
            {"header_crc", s_localBreakdown.header_crc, s_remoteBreakdown.header_crc},
            {"context_crc", s_localBreakdown.context_crc, s_remoteBreakdown.context_crc},
            {"effect_crc", s_localBreakdown.effect_crc, s_remoteBreakdown.effect_crc},
            {"summon_crc", s_localBreakdown.summon_crc, s_remoteBreakdown.summon_crc},
            {"p1_entity_crc", s_localBreakdown.p1_entity_crc, s_remoteBreakdown.p1_entity_crc},
            {"p2_entity_crc", s_localBreakdown.p2_entity_crc, s_remoteBreakdown.p2_entity_crc},
            {"header_agreement_crc", s_localBreakdown.header_agreement_crc, s_remoteBreakdown.header_agreement_crc},
            {"p1_agreement_crc", s_localBreakdown.p1_agreement_crc, s_remoteBreakdown.p1_agreement_crc},
            {"p2_agreement_crc", s_localBreakdown.p2_agreement_crc, s_remoteBreakdown.p2_agreement_crc},
            {"pre_match_gap_crc", s_localBreakdown.pre_match_gap_crc, s_remoteBreakdown.pre_match_gap_crc},
            {"p1_input_crc", s_localBreakdown.p1_input_crc, s_remoteBreakdown.p1_input_crc},
            {"p2_input_crc", s_localBreakdown.p2_input_crc, s_remoteBreakdown.p2_input_crc},
            {"per_frame_temp_crc", s_localBreakdown.per_frame_temp_crc, s_remoteBreakdown.per_frame_temp_crc},
            {"rng_seed", s_localBreakdown.rng_seed, s_remoteBreakdown.rng_seed},
            {"sim_frame", s_localBreakdown.sim_frame, s_remoteBreakdown.sim_frame},
            {"display_frame", s_localBreakdown.display_frame, s_remoteBreakdown.display_frame},
            {"frame_simulation", s_localBreakdown.frame_simulation, s_remoteBreakdown.frame_simulation},
            {"frame_display", s_localBreakdown.frame_display, s_remoteBreakdown.frame_display},
            {"frame_write_idx", s_localBreakdown.frame_write_idx, s_remoteBreakdown.frame_write_idx},
            {"frame_net_idx(last_sent)", s_localBreakdown.frame_net_idx, s_remoteBreakdown.frame_net_idx},
            {"remote_frame_idx", s_localBreakdown.remote_frame_idx, s_remoteBreakdown.remote_frame_idx},
        };

        for (const FieldCmp& field : fields) {
            Rollback::NetplayLog_Write(
                "BASELINE", state.local_sim_frame,
                "Mismatch compare: %s local=0x%08X remote=0x%08X %s",
                field.name,
                field.local,
                field.remote,
                (field.local == field.remote) ? "MATCH" : "DIFF");
        }
    } else {
        Rollback::NetplayLog_Write(
            "BASELINE", state.local_sim_frame,
            "Mismatch compare unavailable: peer BaselineBreakdown was not received");
    }

    WriteMismatchDetailFile(
        state,
        s_localBreakdown,
        s_haveLocalBreakdown,
        s_remoteBreakdown,
        s_haveRemoteBreakdown);

    Rollback::BaselineMismatchDumpParams params{};
    params.mismatch_frame = state.local_sim_frame;
    params.local_crc = state.local_crc;
    params.remote_crc = state.remote_crc;
    params.local_mode = state.local_mode;
    params.local_substate = state.local_substate;
    params.local_sim_frame = state.local_sim_frame;
    params.remote_mode = state.remote_mode;
    params.remote_substate = state.remote_substate;
    params.remote_sim_frame = state.remote_sim_frame;
    params.frame_simulation = s_haveLocalBreakdown ? s_localBreakdown.frame_simulation : ReadMemory<uint32_t>(ADDR_FRAME_SIMULATION);
    params.frame_display = s_haveLocalBreakdown ? s_localBreakdown.frame_display : ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
    params.frame_write_idx = s_haveLocalBreakdown ? s_localBreakdown.frame_write_idx : ReadMemory<uint32_t>(ADDR_FRAME_WRITE_IDX);
    params.frame_net_idx = s_haveLocalBreakdown ? s_localBreakdown.frame_net_idx : ReadMemory<uint32_t>(ADDR_FRAME_NET_IDX);
    params.remote_frame_idx = s_haveRemoteBreakdown ? s_remoteBreakdown.remote_frame_idx : ReadMemory<uint32_t>(ADDR_REMOTE_FRAME);
    params.rng_seed = s_haveLocalBreakdown ? s_localBreakdown.rng_seed : DetVer_GetRngSeed();
    params.phase_name = state.phase_name;
    Rollback::DesyncDump_TryBaselineMismatchDump(params);
    Rollback::NetplayLog_Flush();
}

uint32_t BaselineSync_ComputeAgreementDigest(const BaselineBreakdownPayload& payload) {
    struct AgreementFields {
        uint32_t header_agreement_crc;
        uint32_t summon_crc;
        uint32_t p1_agreement_crc;
        uint32_t p2_agreement_crc;
        uint32_t rng_seed;
        uint32_t sim_frame;
        uint32_t game_mode;
        uint32_t substate;
        uint32_t game_type;
        uint32_t match_phase_timer;
        uint32_t frame_simulation;
        uint32_t frame_write_idx;
        uint32_t frame_net_idx;
        uint32_t p1_input_crc;
        uint32_t p2_input_crc;
    } fields{};

    // Use only authoritative bootstrap state here. Raw header/context/full
    // entity CRCs still remain in the breakdown payload for diagnostics, but
    // they currently include process-local loader handles and frame-local UI
    // counters that differ even when the actual match configuration agrees.
    // The 208-byte input spans ARE part of the agreement (2026-08-17 f0
    // desync fix): the engine2 sync hash covers them, so the baseline
    // rendezvous must fail loud if they differ — match_setup zeroes the
    // frontend residue in them right before capture, leaving only
    // session-synced charsel data, which must agree.
    fields.header_agreement_crc = payload.header_agreement_crc;
    fields.summon_crc = payload.summon_crc;
    fields.p1_agreement_crc = payload.p1_agreement_crc;
    fields.p2_agreement_crc = payload.p2_agreement_crc;
    fields.rng_seed = payload.rng_seed;
    fields.sim_frame = payload.sim_frame;
    fields.game_mode = payload.game_mode;
    fields.substate = payload.substate;
    fields.game_type = payload.game_type;
    fields.match_phase_timer = payload.match_phase_timer;
    fields.frame_simulation = payload.frame_simulation;
    fields.frame_write_idx = payload.frame_write_idx;
    fields.frame_net_idx = payload.frame_net_idx;
    fields.p1_input_crc = payload.p1_input_crc;
    fields.p2_input_crc = payload.p2_input_crc;

    return CalcCRC32(&fields, sizeof(fields));
}

bool BaselineSync_GetLocalBreakdown(BaselineBreakdownPayload* out) {
    if (!out || !s_haveLocalBreakdown) return false;
    *out = s_localBreakdown;
    return true;
}

bool BaselineSync_GetRemoteBreakdown(BaselineBreakdownPayload* out) {
    if (!out || !s_haveRemoteBreakdown) return false;
    *out = s_remoteBreakdown;
    return true;
}

} // namespace Net
