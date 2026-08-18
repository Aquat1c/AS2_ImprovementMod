#include "rollback/game_snapshot.h"

#include "patches/memory_utils.h"
#include "rollback/block_digest.h"
#include "rollback/determinism_verify.h"
#include "ui/log_window.h"

#include <string.h>
#include <windows.h>
#include <xmmintrin.h>

namespace Rollback {

namespace {

constexpr uintptr_t kMainStart = ADDR_MATCH_BASE;
constexpr size_t kMainSize = GAME_SNAPSHOT_MAIN_SIZE;
constexpr uintptr_t kPreMatchStart = ADDR_PRE_MATCH_GAP;
constexpr size_t kPreMatchSize = GAME_SNAPSHOT_PRE_MATCH_SIZE;
constexpr uintptr_t kAiLearnStart = ADDR_AI_LEARN_STATICS;
constexpr size_t kAiLearnSize = GAME_SNAPSHOT_AI_LEARN_SIZE;
constexpr uintptr_t kInputP1Start = ADDR_P1_INPUT_BUFFER;
constexpr uintptr_t kInputP2Start = ADDR_P2_INPUT_BUFFER;
constexpr size_t kInputSize = GAME_SNAPSHOT_INPUT_SIZE;

// ── Digest masks inside main_state (SAVESTATE_AUDIT F2/F4/F5) ───────────────
// Windows written at render cadence (tint, super-bg scratch) or gated by
// wall-clock audio (voice bookkeeping). They MUST stay captured+restored
// (rollback must reproduce them locally) but can never enter the cross-peer
// digest: peers whose render:sim ratios or audio timing differ hold different
// bytes there with perfectly synced gameplay. Offsets are relative to
// main_state[0] (= ADDR_MATCH_BASE), sorted ascending, non-overlapping.
struct MaskRange {
    size_t offset;
    size_t size;
};

constexpr size_t kP1EntityOff = ADDR_P1_ENTITY_BASE - ADDR_MATCH_BASE;
constexpr size_t kP2EntityOff = ADDR_P2_ENTITY_BASE - ADDR_MATCH_BASE;

constexpr MaskRange kMainDigestMasks[] = {
    // F7c: the 68-byte per-PASS sound-dedup scratch (vanilla clears it at
    // the top of each Game_Update_MatchLoop pass; entities mark bytes when
    // sounds trigger; audio-only, no sim reader — rollback_audio.cpp owns
    // rollback-safe dedup). Pre-tick hashes sample it at SIM cadence while
    // it evolves at PASS cadence: the 2nd+ tick of ANY multi-tick pass
    // (engine2 replay ticks, straight-path catch-up passes) sees the
    // previous same-pass tick's marks, and pass boundaries are per-side —
    // guaranteed cross-side hash noise the moment any sound plays
    // (2026-08-17 attempt-2 f780 desync: first attack whiff of the match,
    // low-32 hash halves equal = divergent bytes only at +4..7 of 8-byte
    // blocks = sound ids 4-7). Captured+restored as before (restore's
    // explicit clear = vanilla pass-top semantics); digest-masked only.
    // Announcer voice handles (match+712..763): same class as SE_Handles below.
    { (size_t)MATCH_ANNOUNCER_HANDLES_OFF, (size_t)MATCH_ANNOUNCER_HANDLES_SIZE },     // F9
    // SE_Handles (match+976..1791) MERGED with the F7c per-frame temp
    // (match+1792..1859) — the two spans are exactly adjacent (0x3D0 + 0x330
    // == 0x700). The handles carry a process-global allocation serial in their
    // VALUE (see as2_constants.h), so peers with different allocation
    // histories differ here with no gameplay meaning whatsoever.
    { (size_t)MATCH_SE_HANDLES_OFF,
      (size_t)(MATCH_SE_HANDLES_SIZE + MATCH_PER_FRAME_TEMP_SIZE) },                   // F9 + F7c
    // F7e (2026-08-17, first combat-load run 19-17-3x): the fine-diag ring
    // caught two transient per-side windows in the entities the moment real
    // combat inputs started flowing — the render flash/tint block (F5
    // widened to +0x1B4..+0x1BF: flash flag + pad + tint dwords, render-
    // phase draw bookkeeping) and the 4-byte hit-reaction DISPLAY block
    // (+0x7C4..+0x7C7: combo-pop shown/anim/life/keep — HUD-cadence;
    // rollback_combo_fx owns their rollback correctness and
    // netplay_hud_vanilla rewrites +0x7C5 around render). Both flickered
    // (diverged at single captures, re-agreed next frame) with rng/hp/
    // inputs identical throughout — display sampling noise, no sim reader
    // (INV-22 read-back in vivo). Captured+restored unchanged.
    // F7f: +0x1A4 / +0x7F0 / +0x7F8 — per-PASS-cadence render counters
    // inside the entities (byte-exact FINEENT evidence, run 19-25-3x:
    // cross-side offset == pass-count delta on consecutive frames). Same
    // class as the F7d sim_frame exclusion; captured/restored unchanged.
    { kP1EntityOff + ENTITY_RENDER_ANIM_TIMER_MASK_OFF,
      ENTITY_RENDER_ANIM_TIMER_MASK_SIZE },                                            // F7f
    { kP1EntityOff + ENTITY_RENDER_FLASH_TINT_MASK_OFF,
      ENTITY_RENDER_FLASH_TINT_MASK_SIZE },                                            // F5+F7e
    { kP1EntityOff + ENTITY_SUPERBG_MASK_LOW_OFF,  ENTITY_SUPERBG_MASK_LOW_SIZE },     // F2+F7g (low)
    // GAP: +0x4D4..+0x4DB is HASHED — sim pause/hitstop timers, see as2_constants.h
    { kP1EntityOff + ENTITY_SUPERBG_MASK_HIGH_OFF, ENTITY_SUPERBG_MASK_HIGH_SIZE },    // F2 (high)
    { kP1EntityOff + ENTITY_HIT_REACTION_DISPLAY_MASK_OFF,
      ENTITY_HIT_REACTION_DISPLAY_MASK_SIZE },                                         // F7e
    { kP1EntityOff + ENTITY_RENDER_OUTPUT_BLOCK_OFF,
      ENTITY_RENDER_OUTPUT_BLOCK_SIZE },                                               // F7f render output
    { kP1EntityOff + ENTITY_OFF_VOICE_BOOKKEEPING, ENTITY_VOICE_BOOKKEEPING_SIZE },    // F4
    { kP1EntityOff + ENTITY_VOICE_TAIL_MASK_OFF,   ENTITY_VOICE_TAIL_MASK_SIZE },       // F7h
    { kP2EntityOff + ENTITY_RENDER_ANIM_TIMER_MASK_OFF,
      ENTITY_RENDER_ANIM_TIMER_MASK_SIZE },                                            // F7f
    { kP2EntityOff + ENTITY_RENDER_FLASH_TINT_MASK_OFF,
      ENTITY_RENDER_FLASH_TINT_MASK_SIZE },                                            // F5+F7e
    { kP2EntityOff + ENTITY_SUPERBG_MASK_LOW_OFF,  ENTITY_SUPERBG_MASK_LOW_SIZE },     // F2+F7g (low)
    // GAP: +0x4D4..+0x4DB is HASHED — sim pause/hitstop timers
    { kP2EntityOff + ENTITY_SUPERBG_MASK_HIGH_OFF, ENTITY_SUPERBG_MASK_HIGH_SIZE },    // F2 (high)
    { kP2EntityOff + ENTITY_HIT_REACTION_DISPLAY_MASK_OFF,
      ENTITY_HIT_REACTION_DISPLAY_MASK_SIZE },                                         // F7e
    { kP2EntityOff + ENTITY_RENDER_OUTPUT_BLOCK_OFF,
      ENTITY_RENDER_OUTPUT_BLOCK_SIZE },                                               // F7f render output
    { kP2EntityOff + ENTITY_OFF_VOICE_BOOKKEEPING, ENTITY_VOICE_BOOKKEEPING_SIZE },    // F4
    { kP2EntityOff + ENTITY_VOICE_TAIL_MASK_OFF,   ENTITY_VOICE_TAIL_MASK_SIZE },       // F7h
};
constexpr size_t kMainDigestMaskCount =
    sizeof(kMainDigestMasks) / sizeof(kMainDigestMasks[0]);

constexpr bool MasksSortedDisjointInBounds() {
    size_t prev_end = 0;
    for (size_t i = 0; i < kMainDigestMaskCount; ++i) {
        if (kMainDigestMasks[i].size == 0) return false;
        if (kMainDigestMasks[i].offset < prev_end) return false;
        prev_end = kMainDigestMasks[i].offset + kMainDigestMasks[i].size;
        if (prev_end > kMainSize) return false;
    }
    return true;
}
static_assert(MasksSortedDisjointInBounds(),
              "main_state digest masks must be sorted, disjoint and in-bounds");

// Fold main_state into the digest, skipping the mask windows. Shared by the
// snapshot-copy and live-memory digest paths — identical segmentation is what
// keeps GameSnapshot_HashGameplay and GameSnapshot_HashGameplayLive
// byte-for-byte equal (the Block64 tail fold makes segmentation part of the
// hash value, so both paths MUST walk the same table).
uint64_t HashMainMasked(uint64_t h, const uint8_t* mainBytes) {
    size_t pos = 0;
    for (size_t i = 0; i < kMainDigestMaskCount; ++i) {
        h = Block64_Update(h, mainBytes + pos, kMainDigestMasks[i].offset - pos);
        pos = kMainDigestMasks[i].offset + kMainDigestMasks[i].size;
    }
    return Block64_Update(h, mainBytes + pos, kMainSize - pos);
}

// ── Input-span digest mask (F7b, 2026-08-17 live run 17-47-0x) ──────────────
// Each 208-byte global input span (word_8E9E62[28] layout) is:
//   +0..+55     current-frame button block (words 0-13) + vanilla
//               previous-frame block (words 14-27). The VANILLA input
//               updater rewrites these from its own per-side hardware view
//               (harness/SDL feed) at PASS cadence; the mod's netplay
//               overwrite fixes only words 0-9 and runs at pass cadence
//               too, while pre-tick hashing runs at SIM cadence — catch-up
//               passes make even the mod-written words stale-by-one
//               per side. Attempt-1 live evidence (17-47-0x): prev-block
//               LEFT vs RIGHT at +32/+34 diverged the f0 hash with ALL
//               confirmed inputs 0x0000 and rng/hp identical.
//   +56..+75    just-pressed state — mod-written from the same pass-cadence
//               values: identical skew hazard.
//   +76..+205   charsel committed data — session-synced, static: hashed.
//   +206..+207  (P2 span only) title-counter LOWORD overlap — zeroed at
//               baseline capture, inert during the session: hashed.
// In netplay the sim's input authority is the mod timeline (the dispatcher
// feeds it directly; vanilla Input_PackButtons packs words 0-9 only on
// non-netplay paths), and entity-consumed inputs live in the hashed entity
// blocks — so masking the live-word window [0,76) cannot hide a real
// divergence; it removes the per-side-volatile frontend mirror from the
// digest (same class as the F2/F4/F5 main-region masks). Capture/restore
// stay FULL-span (rollback must reproduce the bytes locally); only digest
// membership changes. Segmentation is part of the Block64 value →
// cross-build incompatible, and HashGameplay/HashGameplayLive MUST share
// this helper.
constexpr size_t kInputSpanVolatileEnd = 76; // button blocks + just-pressed

uint64_t HashInputSpanMasked(uint64_t h, const uint8_t* span) {
    h = Block64_Update(h, span + kInputSpanVolatileEnd,
                       GAME_SNAPSHOT_INPUT_SIZE - kInputSpanVolatileEnd);
    return h;
}

uint32_t SnapshotChecksum(const uint8_t* mainState, size_t mainSize, uint32_t effectIndex) {
    struct ChecksumParts {
        uint32_t main_crc;
        uint32_t effect_index;
    } parts{};

    // Four-lane digest, not the byte-at-a-time CRC32 this used to run: the
    // CRC was a serial chain over the full 253 KB on EVERY capture — roughly
    // 400 us, against ~18 us of actual simulation per replayed frame.
    // Must skip the same bytes GameSnapshot_Restore leaves alone, so that a
    // checksum captured here still matches one recomputed from LIVE memory
    // after a restore.
    (void)mainSize;
    parts.main_crc = ::Rollback::GameSnapshot_MainFingerprintSkippingExcluded(mainState);
    parts.effect_index = effectIndex;
    return StateFingerprint32(&parts, sizeof(parts));
}

} // namespace

void GameSnapshot_Clear(GameSnapshot* snapshot) {
    if (!snapshot) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frame = -1;
}

bool GameSnapshot_Capture(GameSnapshot* snapshot, int32_t frame) {
    if (!snapshot) {
        return false;
    }

    snapshot->valid = false;
    snapshot->frame = frame;
    snapshot->sim_frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    snapshot->display_frame = ReadMemory<uint32_t>(ADDR_FRAME_COUNTER);
    snapshot->game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    snapshot->substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    snapshot->substate_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    snapshot->game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    snapshot->match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
    snapshot->rng_seed = DetVer_GetRngSeed();
    snapshot->effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    // F3: sim-incremented, sim-read Frame_Display (0x816494) — distinct from
    // the render-loop counter captured into display_frame above.
    snapshot->frame_display = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);

    // FPU/MXCSR control words — captured per slot rather than assumed
    // (plan §2.7.7; TrialNetplay pins 0x027F, AS2 captures-and-restores).
    snapshot->fpu_cw = 0;
    snapshot->mxcsr = 0;
    __try {
        unsigned short cw;
        __asm { fnstcw cw }
        snapshot->fpu_cw = cw;
        snapshot->mxcsr = _mm_getcsr();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-fatal: zeros mean "do not restore".
    }

    __try {
        memcpy(snapshot->main_state, (const void*)kMainStart, kMainSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Capture AV at main region 0x%08X", kMainStart);
        return false;
    }

    __try {
        memcpy(snapshot->pre_match_gap, (const void*)kPreMatchStart, kPreMatchSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(snapshot->pre_match_gap, 0, kPreMatchSize);
    }

    // F1: AI pattern-learning statics (0x76C5D8..0x76C5E7). They gate rand()
    // consumption, so a fault here is capture-fatal like the main region —
    // hashing a zeroed stand-in would silently diverge from the live bytes.
    __try {
        memcpy(snapshot->ai_learn, (const void*)kAiLearnStart, kAiLearnSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Capture AV at AI-learn statics 0x%08X", kAiLearnStart);
        return false;
    }

    __try {
        memcpy(snapshot->input_p1, (const void*)kInputP1Start, kInputSize);
        memcpy(snapshot->input_p2, (const void*)kInputP2Start, kInputSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(snapshot->input_p1, 0, kInputSize);
        memset(snapshot->input_p2, 0, kInputSize);
    }

    snapshot->input_read_idx = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
    snapshot->input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
    snapshot->checksum = SnapshotChecksum(
        snapshot->main_state,
        kMainSize,
        snapshot->effect_index);
    snapshot->valid = true;
    return true;
}

// ── Render-cadence popup record: captured, but NOT rewound on restore ─────
// The SIX bytes at entity+0x1A4 are ONE ATOMIC DISPLAY RECORD. Ownership is
// split and unambiguous in the decomp:
//   SIM   writes all four fields exactly once per hit (decomp:106761-106765)
//         and READS NONE of them:
//             *(_BYTE *)(a1 + 420) = 0;    // +0x1A4 expiry clock
//             *(_BYTE *)(a1 + 421) = v15;  // +0x1A5 combo hit count
//             *(_WORD *)(a1 + 422) = v22;  // +0x1A6
//             *(_WORD *)(a1 + 424) = v21;  // +0x1A8
//   RENDER owns the lifetime: sub_4C1F90 increments +0x1A4 once per DRAWN
//         frame and retires by FF-filling the same six bytes at 90
//         (decomp:113443-113448 P1 at match+41492, 113550-113556 P2).
//
// Two bugs, one cause. Rewinding the record every restore stalled the expiry
// so the popup never retired (the stuck "HIT" and leading digit). Holding back
// ONLY the clock replaced that with a TORN record: a rollback past the hit
// restored the FF-filled combo field under a still-live counter, and the
// renderer drew "55 HIT" with a garbage damage number. The record is atomic --
// hold back all six or none.
//
// SIX, NEVER EIGHT. The attempt that excluded the whole 8-byte window (plus
// the 4-byte +0x7C4 block) desynced live at frame 58, and the reason is now
// established: +0x1AA/+0x1AB are SIMULATION state -- the defensive-state flag
// that halves damage (decomp:107062/107069) and hitstun (decomp:105871) -- and
// +0x7C4 is read and written by Entity_UpdateHitReaction. The static_assert
// below is what stops that from recurring: every byte we hold back from the
// restore must also be masked out of the digest, or the two peers' live memory
// legitimately differs and the hash reports a desync that is not one.
constexpr size_t kPopupBlockP1   = kP1EntityOff + ENTITY_POPUP_DISPLAY_BLOCK_OFF;
constexpr size_t kPopupBlockP2   = kP2EntityOff + ENTITY_POPUP_DISPLAY_BLOCK_OFF;
constexpr size_t kPopupBlockSize = ENTITY_POPUP_HOLDBACK_SIZE;
static_assert(ENTITY_POPUP_DISPLAY_BLOCK_OFF == ENTITY_RENDER_ANIM_TIMER_MASK_OFF &&
              ENTITY_POPUP_HOLDBACK_SIZE <= ENTITY_RENDER_ANIM_TIMER_MASK_SIZE,
              "every restore-held-back popup byte must also be digest-masked");
static_assert(ENTITY_POPUP_HOLDBACK_SIZE <= ENTITY_POPUP_DISPLAY_BLOCK_SIZE,
              "holdback cannot exceed the display record");
static_assert(kPopupBlockP1 + kPopupBlockSize <= kPopupBlockP2,
              "popup holdback runs must not overlap");

size_t GameSnapshot_RestoreExcludedRun(size_t mainOffset) {
    // Run-relative: answers anywhere INSIDE a held-back run, not just at its
    // first byte. While the run was one byte those were indistinguishable.
    if (mainOffset >= kPopupBlockP1 && mainOffset < kPopupBlockP1 + kPopupBlockSize) {
        return kPopupBlockP1 + kPopupBlockSize - mainOffset;
    }
    if (mainOffset >= kPopupBlockP2 && mainOffset < kPopupBlockP2 + kPopupBlockSize) {
        return kPopupBlockP2 + kPopupBlockSize - mainOffset;
    }
    return 0;
}

uint32_t GameSnapshot_MainFingerprintSkippingExcluded(const uint8_t* mainBytes) {
    if (!mainBytes) return 0;
    // Two kPopupBlockSize-byte holes; hash the three spans between them so a
    // captured buffer and live memory agree even though the restore skips
    // them. Missing this half is what produced the historical
    // "BASELINE RESTORED with CHECKSUM MISMATCH" false alarm.
    const size_t a = kPopupBlockP1 < kPopupBlockP2 ? kPopupBlockP1 : kPopupBlockP2;
    const size_t b = kPopupBlockP1 < kPopupBlockP2 ? kPopupBlockP2 : kPopupBlockP1;
    uint64_t h = Block64(mainBytes, a);
    h = Block64_Update(h, mainBytes + a + kPopupBlockSize,
                       b - (a + kPopupBlockSize));
    h = Block64_Update(h, mainBytes + b + kPopupBlockSize,
                       kMainSize - (b + kPopupBlockSize));
    return Block64_Fold32(h);
}

bool GameSnapshot_Restore(const GameSnapshot* snapshot) {
    if (!snapshot || !snapshot->valid) {
        return false;
    }

    // Preserve the renderer's whole popup record across the bulk restore.
    uint8_t livePopupP1[kPopupBlockSize] = {};
    uint8_t livePopupP2[kPopupBlockSize] = {};
    bool livePopupValid = false;
    __try {
        memcpy(livePopupP1, (const void*)(kMainStart + kPopupBlockP1), kPopupBlockSize);
        memcpy(livePopupP2, (const void*)(kMainStart + kPopupBlockP2), kPopupBlockSize);
        livePopupValid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        livePopupValid = false;
    }

    __try {
        memcpy((void*)kMainStart, snapshot->main_state, kMainSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Restore AV at main region 0x%08X", kMainStart);
        return false;
    }

    // Hand the renderer its own counter back, un-rewound. A popup the sim has
    // just (re)started reads 0 from the snapshot on both sides anyway, so a
    // fresh popup is unaffected; only an in-flight expiry keeps its progress.
    if (livePopupValid) {
        __try {
            memcpy((void*)(kMainStart + kPopupBlockP1), livePopupP1, kPopupBlockSize);
            memcpy((void*)(kMainStart + kPopupBlockP2), livePopupP2, kPopupBlockSize);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    __try {
        memcpy((void*)kPreMatchStart, snapshot->pre_match_gap, kPreMatchSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    // F1: restore the AI-learning statics with the state they gated.
    __try {
        memcpy((void*)kAiLearnStart, snapshot->ai_learn, kAiLearnSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("[GameSnapshot] Restore AV at AI-learn statics 0x%08X", kAiLearnStart);
        return false;
    }

    DetVer_SetRngSeed(snapshot->rng_seed);
    WriteMemory<uint32_t>(ADDR_EFFECT_INDEX, snapshot->effect_index);

    WriteMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER, snapshot->sim_frame);
    // ADDR_FRAME_COUNTER (0x81635C) is deliberately NOT restored. It is the
    // OUTER-PASS counter (incremented once per main-loop pass, decomp:266427),
    // not a simulation quantity, and it has zero simulation readers in the
    // whole decomp. What does read it is presentation: every "critical"
    // sidebar flash takes its alpha from `-6 - 25 * (*(game+4) % 11)`, an
    // 11-step fade ramp. Restoring it ran that ramp BACKWARDS by the rollback
    // depth on every correction, which vanilla can never do — the low-HP,
    // guard-crush and MAX-meter flashes visibly stuttered under rollback.
    // It stays captured (diagnostics/dumps read snapshot->display_frame) but
    // the live counter is left to advance monotonically, as the renderer
    // expects. Not to be confused with ADDR_FRAME_DISPLAY (0x816494), which IS
    // sim-read and IS restored below (SAVESTATE_AUDIT F3).
    // F3: rewind Frame_Display with the sim so resim re-increments from the
    // restored value instead of drifting ahead by the rollback depth.
    WriteMemory<uint32_t>(ADDR_FRAME_DISPLAY, snapshot->frame_display);
    WriteMemory<uint32_t>(ADDR_GAME_MODE, snapshot->game_mode);
    WriteMemory<uint32_t>(ADDR_SUB_STATE, snapshot->substate);
    WriteMemory<uint32_t>(ADDR_SUB_STATE_TIMER, snapshot->substate_timer);
    WriteMemory<uint32_t>(ADDR_GAME_TYPE, snapshot->game_type);
    WriteMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER, snapshot->match_phase_timer);

    __try {
        memcpy((void*)kInputP1Start, snapshot->input_p1, kInputSize);
        memcpy((void*)kInputP2Start, snapshot->input_p2, kInputSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    WriteMemory<uint32_t>(ADDR_INPUT_READ_IDX, snapshot->input_read_idx);
    WriteMemory<uint32_t>(ADDR_INPUT_WRITE_IDX, snapshot->input_write_idx);

    // Restore FPU/MXCSR control words captured with the slot.
    __try {
        if (snapshot->fpu_cw != 0) {
            unsigned short cw = snapshot->fpu_cw;
            __asm { fldcw cw }
        }
        if (snapshot->mxcsr != 0) {
            _mm_setcsr(snapshot->mxcsr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-fatal.
    }

    __try {
        memset((void*)ADDR_MATCH_PER_FRAME_TEMP, 0, MATCH_PER_FRAME_TEMP_SIZE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    return true;
}

uint64_t GameSnapshot_HashGameplay(const GameSnapshot* snapshot) {
    if (!snapshot || !snapshot->valid) {
        return 0;
    }
    // SIM-affecting membership only (INV-22, M4-6 audit + SAVESTATE_AUDIT):
    // display_frame (0x81635C), pre_match_gap (render state), and the FPU
    // control words are excluded; main_state is folded through the F2/F4/F5
    // digest masks; frame_display (F3) and the AI-learning statics (F1) are
    // hashed.
    //
    // F7d (2026-08-17, run 18-23-1x): sim_frame / input_read_idx — BOTH
    // aliases of 0x816490 `Frame_Simulation` — are EXCLUDED. The counter is
    // incremented once per OUTER PASS by Frame_AdvanceSimulation (0x562760,
    // called after the sim while-loop, unconditionally for game_type != 3;
    // DECOMP_TIMING_STUDY §1.2), while pre-tick hashing runs at SIM cadence:
    // during multi-tick catch-up passes the capture lands mid-pass and the
    // sampled value skews ±1..2 per side by pass-boundary alignment. The
    // fine-diag ring proved 22 confirmed frames of live skew with every
    // other hashed byte (all 4043 main windows, raw context image, all
    // other header scalars) byte-identical — no sim reader in mod netplay
    // (vanilla readers: Input_TryGetNextFrame gating, replaced by the
    // dispatcher; HUD lag indicator, render-only). Captured+restored as
    // before; digest-masked only. Hash-membership change — cross-build
    // incompatible (constraint 3).
    uint64_t h = BLOCK64_SEED;
    struct SimHeader {
        uint32_t rng_seed;
        uint32_t game_mode;
        uint32_t substate;
        uint32_t substate_timer;
        uint32_t game_type;
        uint32_t match_phase_timer;
        uint32_t input_write_idx;
        uint32_t effect_index;
        uint32_t frame_display;
    } header{};
    header.rng_seed = snapshot->rng_seed;
    header.game_mode = snapshot->game_mode;
    header.substate = snapshot->substate;
    header.substate_timer = snapshot->substate_timer;
    header.game_type = snapshot->game_type;
    header.match_phase_timer = snapshot->match_phase_timer;
    header.input_write_idx = snapshot->input_write_idx;
    header.effect_index = snapshot->effect_index;
    header.frame_display = snapshot->frame_display;
    h = Block64_Update(h, &header, sizeof(header));
    h = HashMainMasked(h, snapshot->main_state);
    h = Block64_Update(h, snapshot->ai_learn, sizeof(snapshot->ai_learn));
    h = HashInputSpanMasked(h, snapshot->input_p1);
    h = HashInputSpanMasked(h, snapshot->input_p2);
    return h;
}

bool GameSnapshot_HashGameplayLive(uint64_t* outHash) {
    if (!outHash) {
        return false;
    }
    *outHash = 0;

    // MUST mirror GameSnapshot_HashGameplay exactly: same SimHeader field
    // order, same region order, same digest-mask segmentation, same Block64
    // chaining — a captured snapshot of this instant hashes to the identical
    // value (M7 verification seam). F7d: sim_frame/input_read_idx (0x816490,
    // pass-cadence Frame_Simulation) excluded — see GameSnapshot_HashGameplay.
    struct SimHeader {
        uint32_t rng_seed;
        uint32_t game_mode;
        uint32_t substate;
        uint32_t substate_timer;
        uint32_t game_type;
        uint32_t match_phase_timer;
        uint32_t input_write_idx;
        uint32_t effect_index;
        uint32_t frame_display;
    } header{};

    uint64_t h = BLOCK64_SEED;
    __try {
        header.rng_seed = DetVer_GetRngSeed();
        header.game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
        header.substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
        header.substate_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
        header.game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
        header.match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
        header.input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
        header.effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
        header.frame_display = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
        h = Block64_Update(h, &header, sizeof(header));
        h = HashMainMasked(h, (const uint8_t*)kMainStart);
        h = Block64_Update(h, (const void*)kAiLearnStart, kAiLearnSize);
        h = HashInputSpanMasked(h, (const uint8_t*)kInputP1Start);
        h = HashInputSpanMasked(h, (const uint8_t*)kInputP2Start);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    *outHash = h;
    return true;
}

} // namespace Rollback
