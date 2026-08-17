/**
 * Alice Senki 2 - Desync divergence diagnostics (re0.7 post-M8)
 *
 * Pure ring + deterministic machine-line formatters. No Win32, no game
 * memory, no wall clock — unit pinned in engine2_tests.
 */

#include "rollback/desync_diag.h"

#include <string.h>

namespace Rollback {

// ============================================================================
// DesyncDiagRing
// ============================================================================

void DesyncDiagRing::Reset() {
    memset(entries_, 0, sizeof(entries_));
    next_ = 0;
    count_ = 0;
}

void DesyncDiagRing::Push(const DesyncDiagEntry& e) {
    entries_[next_] = e;
    next_ = (next_ + 1) % DESYNC_DIAG_RING_CAPACITY;
    if (count_ < DESYNC_DIAG_RING_CAPACITY) {
        ++count_;
    }
}

bool DesyncDiagRing::At(uint32_t index, DesyncDiagEntry* out) const {
    if (index >= count_) {
        return false;
    }
    // Oldest retained entry sits at next_ - count_ (mod capacity).
    const uint32_t oldest =
        (next_ + DESYNC_DIAG_RING_CAPACITY - count_) % DESYNC_DIAG_RING_CAPACITY;
    if (out) {
        *out = entries_[(oldest + index) % DESYNC_DIAG_RING_CAPACITY];
    }
    return true;
}

// ============================================================================
// Evidence
// ============================================================================

const char* DesyncEvidence_FirstDivergentField(const DesyncEvidence& ev) {
    if (!ev.valid) return "none";
    // Comparison order mirrors RollbackEngine::ReceiveSyncHash: hash is
    // authoritative and checked first; rng/hp are the narrowing diagnostics.
    if (ev.local_hash != ev.peer_hash) return "hash";
    if (ev.local_rng != ev.peer_rng)   return "rng";
    if (ev.local_hp0 != ev.peer_hp0)   return "hp0";
    if (ev.local_hp1 != ev.peer_hp1)   return "hp1";
    return "none";
}

// ============================================================================
// Formatters (deterministic; no wall clock)
// ============================================================================

int DesyncDiag_FormatRingEntry(const DesyncDiagEntry& e, char* buf, size_t bufSize) {
    return snprintf(buf, bufSize,
        "RING frame=%u epoch=%u hash=%016llx rng=%08x hp0=%u hp1=%u p1=0x%04x p2=0x%04x",
        e.frame, e.epoch,
        (unsigned long long)e.gameplay_hash,
        e.rng,
        (unsigned)e.hp0, (unsigned)e.hp1,
        (unsigned)e.p1_input, (unsigned)e.p2_input);
}

int DesyncDiag_FormatEvidence(const DesyncEvidence& ev, char* buf, size_t bufSize) {
    return snprintf(buf, bufSize,
        "EVIDENCE frame=%u epoch=%u local_hash=%016llx peer_hash=%016llx "
        "local_rng=%08x peer_rng=%08x local_hp0=%u local_hp1=%u "
        "peer_hp0=%u peer_hp1=%u first_divergent=%s",
        ev.frame, ev.epoch,
        (unsigned long long)ev.local_hash,
        (unsigned long long)ev.peer_hash,
        ev.local_rng, ev.peer_rng,
        (unsigned)ev.local_hp0, (unsigned)ev.local_hp1,
        (unsigned)ev.peer_hp0, (unsigned)ev.peer_hp1,
        DesyncEvidence_FirstDivergentField(ev));
}

// ============================================================================
// FILE* writers
// ============================================================================

void DesyncDiag_WriteRing(FILE* f, const DesyncDiagRing& ring) {
    if (!f) return;
    fprintf(f, "RINGCOUNT n=%u\n", ring.Count());
    char line[192];
    DesyncDiagEntry e{};
    for (uint32_t i = 0; ring.At(i, &e); ++i) {
        DesyncDiag_FormatRingEntry(e, line, sizeof(line));
        fprintf(f, "%s\n", line);
    }
}

void DesyncDiag_WriteEvidence(FILE* f, const DesyncEvidence& ev) {
    if (!f) return;
    if (!ev.valid) {
        fprintf(f, "EVIDENCE none\n");
        return;
    }
    char line[320];
    DesyncDiag_FormatEvidence(ev, line, sizeof(line));
    fprintf(f, "%s\n", line);
    fprintf(f, "FIRSTDIVERGENT field=%s frame=%u\n",
            DesyncEvidence_FirstDivergentField(ev), ev.frame);
}

} // namespace Rollback
