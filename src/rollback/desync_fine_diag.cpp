/**
 * Alice Senki 2 - Fine-grained desync localization ring (re0.7, F7d hunt)
 *
 * See desync_fine_diag.h. Game-memory reader — lives in the adapter ring,
 * never linked into the pure test targets.
 */

#include "rollback/desync_fine_diag.h"

#include "core/as2_constants.h"
#include "patches/memory_utils.h"
#include "rollback/block_digest.h"
#include "rollback/determinism_verify.h"
#include "rollback/game_snapshot.h"

#include <string.h>
#include <windows.h>

namespace Rollback {

namespace {

constexpr uintptr_t kMainStart = ADDR_MATCH_BASE;
constexpr size_t kMainSize = GAME_SNAPSHOT_MAIN_SIZE;                 // 0x3F288
constexpr size_t kWindowCount =
    (kMainSize + FINE_DIAG_WINDOW - 1) / FINE_DIAG_WINDOW;            // 4043
// Raw image: match_header + match_context + effect_array (F7d candidates).
constexpr size_t kRawSize = ADDR_SUMMON_ARRAY - ADDR_MATCH_BASE;      // 0x3630
constexpr size_t kInputTailOff = 76;   // stable tail per F7b digest mask
constexpr size_t kInputTailSize = INPUT_BUFFER_SIZE - kInputTailOff;  // 132

// F7e follow-up: byte-exact raw capture of the entity sub-ranges the window
// CRCs flagged under combat load (render flash/tint block neighborhood and
// the combat/hit-reaction tail), so a future divergence in these windows
// names exact bytes without another build.
struct EntSpan { size_t off; size_t size; };
constexpr EntSpan kEntSpans[] = {
    { 0x180, 0x50 },   // +0x180..+0x1CF (flash/tint window neighborhood)
    { 0x4C0, 0x20 },   // +0x4C0..+0x4DF (superbg struct head, F7g)
    { 0x7A0, 0xA0 },   // +0x7A0..+0x83F (combat tail + render output block)
    { 0x1A640, 0x80 }, // +0x1A640..+0x1A6BF (voice tail block, F7h)
};
constexpr size_t kEntSpanCount = sizeof(kEntSpans) / sizeof(kEntSpans[0]);
constexpr size_t kEntRawSize = 0x50 + 0x20 + 0xA0 + 0x80;   // per entity

struct FineDiagEntry {
    bool     valid;
    uint32_t frame;          // canonical confirmed-timeline frame
    // Raw SimHeader values (exactly the scalars the gameplay digest folds).
    uint32_t rng_seed;
    uint32_t sim_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t substate_timer;
    uint32_t game_type;
    uint32_t match_phase_timer;
    uint32_t input_read_idx;
    uint32_t input_write_idx;
    uint32_t effect_index;
    uint32_t frame_display;
    uint32_t ai_learn_crc;
    uint32_t p1_tail_crc;
    uint32_t p2_tail_crc;
    uint32_t win_crc[kWindowCount];
    uint8_t  raw[kRawSize];
    uint8_t  ent_raw[2][kEntRawSize];   // [p1|p2] concatenated kEntSpans
};

FineDiagEntry s_ring[FINE_DIAG_RING_CAPACITY];   // ~1.9 MB BSS
bool s_envChecked = false;
bool s_enabled = true;

uint32_t Fold32(uint64_t h) {
    return (uint32_t)(h ^ (h >> 32));
}

bool Enabled() {
    if (!s_envChecked) {
        s_envChecked = true;
        char v[8] = {};
        const DWORD n = GetEnvironmentVariableA("AS2_FINE_DIAG", v, sizeof(v));
        if (n == 1 && v[0] == '0') {
            s_enabled = false;
        }
    }
    return s_enabled;
}

void RecordGuarded(FineDiagEntry* e, uint32_t frame) {
    e->valid = false;
    e->frame = frame;
    e->rng_seed = DetVer_GetRngSeed();
    e->sim_frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    e->game_mode = ReadMemory<uint32_t>(ADDR_GAME_MODE);
    e->substate = ReadMemory<uint32_t>(ADDR_SUB_STATE);
    e->substate_timer = ReadMemory<uint32_t>(ADDR_SUB_STATE_TIMER);
    e->game_type = ReadMemory<uint32_t>(ADDR_GAME_TYPE);
    e->match_phase_timer = ReadMemory<uint32_t>(ADDR_MATCH_PHASE_TIMER);
    e->input_read_idx = ReadMemory<uint32_t>(ADDR_INPUT_READ_IDX);
    e->input_write_idx = ReadMemory<uint32_t>(ADDR_INPUT_WRITE_IDX);
    e->effect_index = ReadMemory<uint32_t>(ADDR_EFFECT_INDEX);
    e->frame_display = ReadMemory<uint32_t>(ADDR_FRAME_DISPLAY);
    e->ai_learn_crc = Fold32(
        Block64((const void*)ADDR_AI_LEARN_STATICS, AI_LEARN_STATICS_SIZE));
    e->p1_tail_crc = Fold32(Block64(
        (const void*)(ADDR_P1_INPUT_BUFFER + kInputTailOff), kInputTailSize));
    e->p2_tail_crc = Fold32(Block64(
        (const void*)(ADDR_P2_INPUT_BUFFER + kInputTailOff), kInputTailSize));

    const uint8_t* main_bytes = (const uint8_t*)kMainStart;
    for (size_t w = 0; w < kWindowCount; ++w) {
        const size_t off = w * FINE_DIAG_WINDOW;
        const size_t len =
            (off + FINE_DIAG_WINDOW <= kMainSize) ? FINE_DIAG_WINDOW
                                                  : (kMainSize - off);
        e->win_crc[w] = Fold32(Block64(main_bytes + off, len));
    }
    memcpy(e->raw, main_bytes, kRawSize);
    const uintptr_t entBase[2] = { ADDR_P1_ENTITY_BASE, ADDR_P2_ENTITY_BASE };
    for (int p = 0; p < 2; ++p) {
        size_t pos = 0;
        for (size_t s = 0; s < kEntSpanCount; ++s) {
            memcpy(e->ent_raw[p] + pos,
                   (const void*)(entBase[p] + kEntSpans[s].off),
                   kEntSpans[s].size);
            pos += kEntSpans[s].size;
        }
    }
    e->valid = true;
}

} // namespace

void FineDiag_Reset() {
    for (uint32_t i = 0; i < FINE_DIAG_RING_CAPACITY; ++i) {
        s_ring[i].valid = false;
    }
}

void FineDiag_RecordPreTick(uint32_t canonical_frame) {
    if (!Enabled()) {
        return;
    }
    FineDiagEntry* e = &s_ring[canonical_frame % FINE_DIAG_RING_CAPACITY];
    __try {
        RecordGuarded(e, canonical_frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        e->valid = false;
    }
}

void FineDiag_WriteDump(FILE* f) {
    if (!f) {
        return;
    }
    // Collect valid entries in ascending frame order (ring is frame-keyed,
    // so a simple sort of the valid slots suffices).
    const FineDiagEntry* order[FINE_DIAG_RING_CAPACITY];
    uint32_t n = 0;
    for (uint32_t i = 0; i < FINE_DIAG_RING_CAPACITY; ++i) {
        if (s_ring[i].valid) {
            order[n++] = &s_ring[i];
        }
    }
    for (uint32_t i = 1; i < n; ++i) {
        const FineDiagEntry* key = order[i];
        uint32_t j = i;
        while (j > 0 && order[j - 1]->frame > key->frame) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    fprintf(f, "FINECOUNT n=%u win=%u base=0x%08X main=%u raw=%u\n",
            n, (unsigned)FINE_DIAG_WINDOW, (uint32_t)kMainStart,
            (unsigned)kMainSize, (unsigned)kRawSize);
    for (uint32_t i = 0; i < n; ++i) {
        const FineDiagEntry* e = order[i];
        fprintf(f,
            "FINEHDR frame=%u rng=%08x sim=%u mode=%u sub=%u subt=%u "
            "gtype=%u mpt=%u ridx=%u widx=%u effidx=%u fdisp=%u "
            "ai=%08x p1t=%08x p2t=%08x\n",
            e->frame, e->rng_seed, e->sim_frame, e->game_mode, e->substate,
            e->substate_timer, e->game_type, e->match_phase_timer,
            e->input_read_idx, e->input_write_idx, e->effect_index,
            e->frame_display, e->ai_learn_crc, e->p1_tail_crc, e->p2_tail_crc);

        fprintf(f, "FINEWIN frame=%u n=%u crcs=", e->frame,
                (unsigned)kWindowCount);
        for (size_t w = 0; w < kWindowCount; ++w) {
            fprintf(f, w ? ",%08x" : "%08x", e->win_crc[w]);
        }
        fputc('\n', f);

        static const char* kHex = "0123456789abcdef";
        char buf[256];

        fprintf(f, "FINERAW frame=%u n=%u hex=", e->frame, (unsigned)kRawSize);
        size_t bi = 0;
        for (size_t b = 0; b < kRawSize; ++b) {
            buf[bi++] = kHex[e->raw[b] >> 4];
            buf[bi++] = kHex[e->raw[b] & 0xF];
            if (bi == sizeof(buf)) {
                fwrite(buf, 1, bi, f);
                bi = 0;
            }
        }
        if (bi) {
            fwrite(buf, 1, bi, f);
        }
        fputc('\n', f);

        // Entity sub-range raw images: spans documented in the header line so
        // the comparator maps concat offsets back to entity offsets.
        fprintf(f, "FINEENT frame=%u spans=0x180:0x50,0x4c0:0x20,0x7a0:0xa0,0x1a640:0x80 p1=",
                e->frame);
        for (int p = 0; p < 2; ++p) {
            if (p == 1) {
                fprintf(f, " p2=");
            }
            for (size_t b = 0; b < kEntRawSize; ++b) {
                fputc(kHex[e->ent_raw[p][b] >> 4], f);
                fputc(kHex[e->ent_raw[p][b] & 0xF], f);
            }
        }
        fputc('\n', f);
    }
}

} // namespace Rollback
