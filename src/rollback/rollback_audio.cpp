#include "rollback/rollback_audio.h"

#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"

#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <windows.h>

namespace Rollback {
namespace {

static constexpr int kSoundIdCount = MATCH_PER_FRAME_TEMP_SIZE;
static constexpr int kDefaultReserve = 256;
static constexpr int kPruneMarginFrames = 12;
static constexpr int kShiftedStandInWindowFrames = 4;
static constexpr bool kEnableShiftedStandIn = true;

struct AudioEventId {
    int32_t rb_frame;
    uint16_t sound_id;
    uint16_t ordinal;
};

struct AudioEvent {
    AudioEventId id;
    int32_t game_abs_frame;
    uint32_t sequence;
    bool consumed;
    bool heard_by_standin;
};

static bool s_initialized = false;
static bool s_sessionActive = false;
static bool s_insideOriginalSEPlay = false;
static bool s_reconcileOpen = false;
static int s_rollbackBudget = 8;
static int32_t s_currentRbFrame = -1;
static int32_t s_currentGameAbsFrame = -1;
static bool s_currentRollingBack = false;
static int32_t s_lastLoadFrame = -999999;
static int32_t s_lastSummaryFrame = -999999;
static uint32_t s_sequence = 0;
static uint16_t s_frameSoundOrdinal[kSoundIdCount] = {};
static RollbackAudioSnapshot s_stats = {};

static std::vector<AudioEvent> s_committed;
static std::vector<AudioEvent> s_pending;
static std::vector<AudioEvent> s_corrected;

static bool IsValidSoundId(int sound_id) {
    return sound_id >= 0 && sound_id < kSoundIdCount;
}

static uint8_t* SoundActiveBytes() {
    return reinterpret_cast<uint8_t*>(ADDR_MATCH_PER_FRAME_TEMP);
}

static bool VanillaWouldPlayNow(int sound_id) {
    if (!IsValidSoundId(sound_id)) {
        return false;
    }

    __try {
        return SoundActiveBytes()[sound_id] == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

static void MarkVanillaSoundActive(int sound_id) {
    if (!IsValidSoundId(sound_id)) {
        return;
    }

    __try {
        SoundActiveBytes()[sound_id] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static char CallOriginalSEPlay(int sound_id) {
    if (!g_origSEPlay) {
        return 0;
    }

    s_insideOriginalSEPlay = true;
    char ret = g_origSEPlay(sound_id);
    s_insideOriginalSEPlay = false;
    return ret;
}

static AudioEvent MakeEvent(int sound_id) {
    AudioEvent ev{};
    ev.id.rb_frame = s_currentRbFrame;
    ev.id.sound_id = static_cast<uint16_t>(sound_id);
    ev.id.ordinal = s_frameSoundOrdinal[sound_id]++;
    ev.game_abs_frame = s_currentGameAbsFrame;
    ev.sequence = ++s_sequence;
    return ev;
}

static bool SameId(const AudioEvent& a, const AudioEvent& b) {
    return a.id.rb_frame == b.id.rb_frame &&
           a.id.sound_id == b.id.sound_id &&
           a.id.ordinal == b.id.ordinal;
}

static void ReserveVectors() {
    s_committed.reserve(kDefaultReserve);
    s_pending.reserve(kDefaultReserve);
    s_corrected.reserve(kDefaultReserve);
}

static void PruneOldEvents(int32_t current_rb_frame) {
    const int32_t keepAfter = current_rb_frame - s_rollbackBudget - kPruneMarginFrames;
    const size_t before = s_committed.size();

    s_committed.erase(
        std::remove_if(s_committed.begin(), s_committed.end(),
            [keepAfter](const AudioEvent& ev) {
                return ev.id.rb_frame < keepAfter;
            }),
        s_committed.end());

    const size_t pruned = before - s_committed.size();
    if (pruned > 0) {
        NetplayLog_Verbose("AUDIO", current_rb_frame,
            "PRUNE keep_after=%d pruned=%u remaining=%d",
            keepAfter,
            (unsigned)pruned,
            (int)s_committed.size());
    }
}

static AudioEvent* ConsumeExactPending(const AudioEvent& corrected) {
    for (AudioEvent& pending : s_pending) {
        if (!pending.consumed && SameId(pending, corrected)) {
            pending.consumed = true;
            return &pending;
        }
    }
    return nullptr;
}

static AudioEvent* ConsumeShiftedPending(const AudioEvent& corrected) {
    if (!kEnableShiftedStandIn) {
        return nullptr;
    }

    AudioEvent* best = nullptr;
    int bestScore = 999999;
    for (AudioEvent& pending : s_pending) {
        if (pending.consumed || pending.id.sound_id != corrected.id.sound_id) {
            continue;
        }

        const int distance = abs((int)pending.id.rb_frame - (int)corrected.id.rb_frame);
        if (distance > kShiftedStandInWindowFrames) {
            continue;
        }

        const int ordinalPenalty = pending.id.ordinal == corrected.id.ordinal ? 0 : 100;
        const int score = distance + ordinalPenalty;
        if (score < bestScore) {
            bestScore = score;
            best = &pending;
        }
    }

    if (best) {
        best->consumed = true;
    }
    return best;
}

static void CommitCorrected(const AudioEvent& corrected, bool heard_by_standin) {
    AudioEvent ev = corrected;
    ev.consumed = false;
    ev.heard_by_standin = heard_by_standin;
    s_committed.push_back(ev);
}

static void PlayLateCorrected(const AudioEvent& corrected) {
    __try {
        SoundActiveBytes()[corrected.id.sound_id] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    const char ret = CallOriginalSEPlay((int)corrected.id.sound_id);
    s_stats.total_late_played++;
    NetplayLog_Write("AUDIO", corrected.id.rb_frame,
        "LATE_PLAY sound=%u ordinal=%u rb_frame=%d game_abs_frame=%d ret=%d",
        corrected.id.sound_id,
        corrected.id.ordinal,
        corrected.id.rb_frame,
        corrected.game_abs_frame,
        (int)ret);
}

static void ReconcileNow(const char* reason) {
    if (!s_reconcileOpen) {
        return;
    }

    int exact = 0;
    int shifted = 0;
    int late = 0;
    int ghosts = 0;

    NetplayLog_Write("AUDIO", s_currentRbFrame,
        "RECONCILE_BEGIN reason=%s load_rb=%d pending=%d corrected=%d committed_before=%d",
        reason ? reason : "unknown",
        s_lastLoadFrame,
        (int)s_pending.size(),
        (int)s_corrected.size(),
        (int)s_committed.size());

    for (const AudioEvent& corrected : s_corrected) {
        if (AudioEvent* old = ConsumeExactPending(corrected)) {
            CommitCorrected(corrected, true);
            exact++;
            s_stats.total_exact_matches++;
            NetplayLog_Verbose("AUDIO", corrected.id.rb_frame,
                "MATCH_EXACT sound=%u ord=%u corrected_rb=%d old_rb=%d",
                corrected.id.sound_id,
                corrected.id.ordinal,
                corrected.id.rb_frame,
                old->id.rb_frame);
            continue;
        }

        if (AudioEvent* old = ConsumeShiftedPending(corrected)) {
            CommitCorrected(corrected, true);
            shifted++;
            s_stats.total_shifted_matches++;
            NetplayLog_Write("AUDIO", corrected.id.rb_frame,
                "MATCH_SHIFTED sound=%u corr_ord=%u old_ord=%u corrected_rb=%d old_rb=%d delta=%d",
                corrected.id.sound_id,
                corrected.id.ordinal,
                old->id.ordinal,
                corrected.id.rb_frame,
                old->id.rb_frame,
                (int)corrected.id.rb_frame - (int)old->id.rb_frame);
            continue;
        }

        PlayLateCorrected(corrected);
        CommitCorrected(corrected, false);
        late++;
    }

    for (const AudioEvent& pending : s_pending) {
        if (!pending.consumed) {
            ghosts++;
            s_stats.total_ghosts++;
            NetplayLog_Write("AUDIO", pending.id.rb_frame,
                "GHOST_PREDICTED sound=%u ordinal=%u old_rb=%d old_game_abs=%d seq=%u",
                pending.id.sound_id,
                pending.id.ordinal,
                pending.id.rb_frame,
                pending.game_abs_frame,
                pending.sequence);
        }
    }

    NetplayLog_Write("AUDIO", s_currentRbFrame,
        "RECONCILE_END reason=%s exact=%d shifted=%d late=%d ghosts=%d committed_after=%d",
        reason ? reason : "unknown",
        exact,
        shifted,
        late,
        ghosts,
        (int)s_committed.size());

    s_pending.clear();
    s_corrected.clear();
    s_reconcileOpen = false;
}

} // namespace

SEPlay_t g_origSEPlay = nullptr;

void RollbackAudio_Init() {
    if (s_initialized) {
        return;
    }

    ReserveVectors();
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_frameSoundOrdinal, 0, sizeof(s_frameSoundOrdinal));
    s_initialized = true;
    NetplayLog_Write("AUDIO", -1,
        "INIT rollback audio journal reserve=%d sound_ids=%d shifted_standin=%d shifted_window=%d",
        kDefaultReserve,
        kSoundIdCount,
        kEnableShiftedStandIn ? 1 : 0,
        kShiftedStandInWindowFrames);
}

void RollbackAudio_Shutdown() {
    RollbackAudio_OnSessionEnd("RollbackAudio_Shutdown");
    s_committed.clear();
    s_pending.clear();
    s_corrected.clear();
    s_initialized = false;
}

void RollbackAudio_OnSessionBegin(int rollback_budget) {
    s_sessionActive = true;
    s_rollbackBudget = rollback_budget > 0 ? rollback_budget : 8;
    s_currentRbFrame = -1;
    s_currentGameAbsFrame = -1;
    s_currentRollingBack = false;
    s_reconcileOpen = false;
    s_lastLoadFrame = -999999;
    s_lastSummaryFrame = -999999;
    s_sequence = 0;
    memset(s_frameSoundOrdinal, 0, sizeof(s_frameSoundOrdinal));
    memset(&s_stats, 0, sizeof(s_stats));
    s_committed.clear();
    s_pending.clear();
    s_corrected.clear();
    ReserveVectors();

    NetplayLog_Write("AUDIO", 0,
        "SESSION_BEGIN rollback_budget=%d shifted_standin=%d shifted_window=%d reserve=%d",
        s_rollbackBudget,
        kEnableShiftedStandIn ? 1 : 0,
        kShiftedStandInWindowFrames,
        kDefaultReserve);
}

void RollbackAudio_OnSessionEnd(const char* reason) {
    if (!s_sessionActive && s_committed.empty() && s_pending.empty() && s_corrected.empty()) {
        return;
    }

    if (s_reconcileOpen) {
        ReconcileNow("session_end");
    }

    NetplayLog_Write("AUDIO", s_currentRbFrame,
        "SESSION_END reason=%s committed=%d pending=%d corrected=%d normal=%d captured=%d exact=%d shifted=%d late=%d ghosts=%d invalid=%d dupes=%d",
        reason ? reason : "unknown",
        (int)s_committed.size(),
        (int)s_pending.size(),
        (int)s_corrected.size(),
        s_stats.total_normal_played,
        s_stats.total_resim_captured,
        s_stats.total_exact_matches,
        s_stats.total_shifted_matches,
        s_stats.total_late_played,
        s_stats.total_ghosts,
        s_stats.total_invalid_sound_ids,
        s_stats.total_vanilla_frame_dupes);

    s_sessionActive = false;
    s_committed.clear();
    s_pending.clear();
    s_corrected.clear();
    s_reconcileOpen = false;
}

void RollbackAudio_OnEngineLoad(int32_t load_rb_frame, int32_t load_game_abs_frame) {
    if (!s_sessionActive) {
        return;
    }

    if (s_reconcileOpen && (!s_pending.empty() || !s_corrected.empty())) {
        NetplayLog_Write("AUDIO", load_rb_frame,
            "WARN nested/overlapping audio rollback: previous_load=%d pending=%d corrected=%d; forcing reconcile before new load",
            s_lastLoadFrame,
            (int)s_pending.size(),
            (int)s_corrected.size());
        ReconcileNow("nested_load");
    }

    s_pending.clear();
    s_corrected.clear();
    s_reconcileOpen = true;
    s_lastLoadFrame = load_rb_frame;

    std::vector<AudioEvent> kept;
    kept.reserve(s_committed.size());

    int moved = 0;
    for (AudioEvent& ev : s_committed) {
        if (ev.id.rb_frame > load_rb_frame) {
            ev.consumed = false;
            s_pending.push_back(ev);
            moved++;
        } else {
            kept.push_back(ev);
        }
    }
    s_committed.swap(kept);

    NetplayLog_Write("AUDIO", load_rb_frame,
        "ROLLBACK_AUDIO_LOAD load_rb=%d load_game_abs=%d moved_pending=%d kept_committed=%d pending_total=%d",
        load_rb_frame,
        load_game_abs_frame,
        moved,
        (int)s_committed.size(),
        (int)s_pending.size());
}

void RollbackAudio_OnAdvanceBegin(int32_t rb_frame, int32_t game_abs_frame, bool rolling_back) {
    if (!rolling_back && s_reconcileOpen) {
        s_currentRbFrame = rb_frame;
        s_currentGameAbsFrame = game_abs_frame;
        ReconcileNow("before_normal_advance");
    }

    s_currentRbFrame = rb_frame;
    s_currentGameAbsFrame = game_abs_frame;
    s_currentRollingBack = rolling_back;
    memset(s_frameSoundOrdinal, 0, sizeof(s_frameSoundOrdinal));

    NetplayLog_Verbose("AUDIO", rb_frame,
        "ADVANCE_BEGIN rb_frame=%d game_abs_frame=%d rolling_back=%d committed=%d pending=%d corrected=%d",
        rb_frame,
        game_abs_frame,
        rolling_back ? 1 : 0,
        (int)s_committed.size(),
        (int)s_pending.size(),
        (int)s_corrected.size());
}

void RollbackAudio_OnEngineBatchEnd(int32_t rb_frame, int32_t game_abs_frame) {
    if (!s_sessionActive) {
        return;
    }

    s_currentRbFrame = rb_frame;
    s_currentGameAbsFrame = game_abs_frame;

    if (s_reconcileOpen) {
        ReconcileNow("batch_end");
    }

    PruneOldEvents(rb_frame);

    if (rb_frame - s_lastSummaryFrame >= 120) {
        s_lastSummaryFrame = rb_frame;
        NetplayLog_Write("AUDIO", rb_frame,
            "SUMMARY committed=%d pending=%d corrected=%d normal=%d captured=%d exact=%d shifted=%d late=%d ghosts=%d dupes=%d",
            (int)s_committed.size(),
            (int)s_pending.size(),
            (int)s_corrected.size(),
            s_stats.total_normal_played,
            s_stats.total_resim_captured,
            s_stats.total_exact_matches,
            s_stats.total_shifted_matches,
            s_stats.total_late_played,
            s_stats.total_ghosts,
            s_stats.total_vanilla_frame_dupes);
    }
}

// ── Audio_IsPlaying record/replay (qoh99 hkSoundStatus model) ─────────────
// The simulation branches on live DirectSound buffer state: Entity_UpdateAudio
// only plays a voice and updates the CAPTURED voice bookkeeping when
// Audio_IsPlaying returns false. Truth and replay ticks run at different real
// times, so the device can answer differently and the two take different
// branches — state that then differs from what the snapshot says it should be.
// Digest-masking those bytes hid the divergence; it never removed it.
//
// So: record the device's answers in call order on the truth tick, and hand
// the SAME answers back during replay ticks. The replay then reproduces the
// truth tick's branches exactly, which is the whole contract of a rollback.
namespace {

constexpr size_t kAudioStatusRingFrames = 64;   // >= max rollback depth + slack
constexpr size_t kAudioStatusCallsPerFrame = 32;

struct AudioStatusFrame {
    int32_t  rb_frame = -1;
    uint16_t count = 0;
    int      results[kAudioStatusCallsPerFrame] = {};
};

AudioStatusFrame s_statusRing[kAudioStatusRingFrames];
uint16_t         s_statusReplayCursor = 0;
int32_t          s_statusReplayFrame = -1;
uint32_t         s_statusRecorded = 0;
uint32_t         s_statusReplayed = 0;
uint32_t         s_statusOverflow = 0;
uint32_t         s_statusMisses = 0;

AudioStatusFrame& StatusSlot(int32_t rb_frame) {
    return s_statusRing[(size_t)((uint32_t)rb_frame % kAudioStatusRingFrames)];
}

} // namespace

// ── Canonical voice model (qoh99 DeterministicAudioPolicy model) ──────────
// Record/replay above fixes truth-vs-replay on ONE machine. It cannot fix the
// peers disagreeing, and they do: Entity_UpdateAudio writes audioStatePtr[1]
// and [2] at match+149184/149188 — inside the match region, BELOW P2's entity,
// so hashed and NOT masked. Two peers whose DirectSound buffers sit at
// different playback positions take different branches and write different
// values into HASHED state. Live desync f13319 has exactly that fingerprint:
// rng identical, hp identical, state hash different.
//
// qoh99's answer (DeterministicAudioPolicy.h): never consult the playback
// cursor for a status the simulation consumes. Expire the voice on a canonical
// TICK instead, so "is it still playing" is frame arithmetic and every peer
// computes the same answer — "a .wav on disk must not be able to decide how
// long the game waits".
//
// Rollback-safe without touching the snapshot: the model stores the canonical
// START frame per voice slot. A replayed play call re-stamps the same
// canonical frame (idempotent), and a start frame in the future of a restored
// frame simply reads as not-yet-started, so a restore needs no undo.
namespace {

constexpr size_t   kVoiceSlots = 4096;          // handle low 16 bits, masked
constexpr int32_t  kVoiceDurationFrames = 61;   // qoh99's fallback one-shot length

int32_t s_voiceStartFrame[kVoiceSlots];
bool    s_voiceModelReady = false;
uint32_t s_voiceModelAnswers = 0;
uint32_t s_voiceDeviceAnswers = 0;

void VoiceModelReset() {
    for (size_t i = 0; i < kVoiceSlots; ++i) s_voiceStartFrame[i] = INT32_MIN;
    s_voiceModelReady = true;
}

inline size_t VoiceSlot(int handle) {
    return (size_t)((uint32_t)handle & 0x0FFFu);
}

} // namespace

void RollbackAudio_ResetVoiceModel() {
    VoiceModelReset();
}

void RollbackAudio_GetVoiceModelStats(uint32_t* model_answers,
                                      uint32_t* device_answers) {
    if (model_answers) *model_answers = s_voiceModelAnswers;
    if (device_answers) *device_answers = s_voiceDeviceAnswers;
}

AudioPlayWrapper_t g_origAudioPlayWrapper = nullptr;

int __cdecl Hook_Audio_Play_Wrapper(int handle) {
    if (!g_origAudioPlayWrapper) return 0;
    if (RollbackSession_IsActive()) {
        if (!s_voiceModelReady) VoiceModelReset();
        // Canonical start stamp. Replays re-stamp the same value, so this is
        // idempotent under rollback.
        s_voiceStartFrame[VoiceSlot(handle)] = RollbackSession_GetCurrentFrame();
        // Suppress the actual device call during replay so a re-simulated
        // frame does not retrigger a voice the player already heard.
        if (RollbackSession_IsRollingBack()) {
            return 0;
        }
    }
    return g_origAudioPlayWrapper(handle);
}

AudioIsPlaying_t g_origAudioIsPlaying = nullptr;

int __cdecl Hook_Audio_IsPlaying(int handle) {
    if (!g_origAudioIsPlaying) return -1;
    if (!RollbackSession_IsActive()) {
        return g_origAudioIsPlaying(handle);
    }

    // Canonical answer: identical on both peers by construction, and
    // reproducible across truth and replay.
    if (s_voiceModelReady) {
        const int32_t started = s_voiceStartFrame[VoiceSlot(handle)];
        if (started != INT32_MIN) {
            const int32_t now = RollbackSession_GetCurrentFrame();
            const int32_t elapsed = now - started;
            ++s_voiceModelAnswers;
            // Not yet started (restored below the stamp) reads as silent.
            if (elapsed < 0) return 0;
            return elapsed < kVoiceDurationFrames ? 1 : 0;
        }
        // Never played in this session: silent, deterministically.
        ++s_voiceModelAnswers;
        return 0;
    }

    const int32_t frame = RollbackSession_GetCurrentFrame();

    if (RollbackSession_IsRollingBack()) {
        AudioStatusFrame& slot = StatusSlot(frame);
        if (s_statusReplayFrame != frame) {
            s_statusReplayFrame = frame;
            s_statusReplayCursor = 0;
        }
        if (slot.rb_frame == frame && s_statusReplayCursor < slot.count) {
            ++s_statusReplayed;
            return slot.results[s_statusReplayCursor++];
        }
        // No recording for this call: the replay asked more times than the
        // truth tick did. Fall through to the device rather than inventing a
        // value, and count it — a nonzero miss rate means the record window is
        // too small or the branch genuinely diverged.
        ++s_statusMisses;
        ++s_voiceDeviceAnswers;
        return g_origAudioIsPlaying(handle);
    }

    ++s_voiceDeviceAnswers;
    const int result = g_origAudioIsPlaying(handle);
    AudioStatusFrame& slot = StatusSlot(frame);
    if (slot.rb_frame != frame) {
        slot.rb_frame = frame;
        slot.count = 0;
    }
    if (slot.count < kAudioStatusCallsPerFrame) {
        slot.results[slot.count++] = result;
        ++s_statusRecorded;
    } else {
        ++s_statusOverflow;
    }
    return result;
}

void RollbackAudio_GetStatusStats(uint32_t* recorded, uint32_t* replayed,
                                  uint32_t* misses, uint32_t* overflow) {
    if (recorded) *recorded = s_statusRecorded;
    if (replayed) *replayed = s_statusReplayed;
    if (misses)   *misses = s_statusMisses;
    if (overflow) *overflow = s_statusOverflow;
}

char __cdecl Hook_SE_Play(int sound_id) {
    if (s_insideOriginalSEPlay) {
        return g_origSEPlay ? g_origSEPlay(sound_id) : 0;
    }

    if (!IsValidSoundId(sound_id)) {
        s_stats.total_invalid_sound_ids++;
        NetplayLog_Write("AUDIO", RollbackSession_GetCurrentFrame(),
            "INVALID sound_id=%d active=%d rolling_back=%d",
            sound_id,
            RollbackSession_IsActive() ? 1 : 0,
            RollbackSession_IsRollingBack() ? 1 : 0);
        return CallOriginalSEPlay(sound_id);
    }

    if (!RollbackSession_IsActive()) {
        return CallOriginalSEPlay(sound_id);
    }

    s_currentRbFrame = RollbackSession_GetCurrentFrame();
    s_currentGameAbsFrame = RollbackSession_GetCurrentGameAbsFrame();
    s_currentRollingBack = RollbackSession_IsRollingBack();

    const bool wouldPlay = VanillaWouldPlayNow(sound_id);
    if (!wouldPlay) {
        s_stats.total_vanilla_frame_dupes++;
        NetplayLog_Verbose("AUDIO", s_currentRbFrame,
            "SE_DUP_FRAME sound=%d rb_frame=%d game_abs_frame=%d rolling_back=%d",
            sound_id,
            s_currentRbFrame,
            s_currentGameAbsFrame,
            s_currentRollingBack ? 1 : 0);
        return 1;
    }

    if (s_currentRollingBack) {
        MarkVanillaSoundActive(sound_id);
        AudioEvent ev = MakeEvent(sound_id);
        s_corrected.push_back(ev);
        s_stats.total_resim_captured++;

        NetplayLog_Verbose("AUDIO", s_currentRbFrame,
            "SE_CAPTURE_RESIM sound=%u ordinal=%u rb_frame=%d game_abs_frame=%d pending=%d corrected=%d",
            ev.id.sound_id,
            ev.id.ordinal,
            ev.id.rb_frame,
            ev.game_abs_frame,
            (int)s_pending.size(),
            (int)s_corrected.size());
        return 1;
    }

    const char ret = CallOriginalSEPlay(sound_id);
    AudioEvent ev = MakeEvent(sound_id);
    s_committed.push_back(ev);
    s_stats.total_normal_played++;

    NetplayLog_Verbose("AUDIO", s_currentRbFrame,
        "SE_PLAY_NORMAL sound=%u ordinal=%u rb_frame=%d game_abs_frame=%d ret=%d committed=%d",
        ev.id.sound_id,
        ev.id.ordinal,
        ev.id.rb_frame,
        ev.game_abs_frame,
        (int)ret,
        (int)s_committed.size());

    return ret;
}

void RollbackAudio_GetSnapshot(RollbackAudioSnapshot* out) {
    if (!out) {
        return;
    }

    *out = s_stats;
    out->committed_count = (int32_t)s_committed.size();
    out->pending_count = (int32_t)s_pending.size();
    out->corrected_count = (int32_t)s_corrected.size();
}

} // namespace Rollback
