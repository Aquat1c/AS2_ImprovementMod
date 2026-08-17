/**
 * Alice Senki 2 - Desync divergence diagnostics (re0.7 post-M8)
 *
 * Pure, game-memory-free, clock-free building blocks for localizing a
 * ConfirmedDesync after the fact:
 *
 *   - DesyncDiagRing: a rolling ring of the last N confirmed frames'
 *     {frame, gameplay_hash, rng, hp0, hp1, p1_input, p2_input}, fed by the
 *     adapter from the confirm seam (always on during netplay, cheap).
 *   - DesyncEvidence: the failing SyncHash pair (local vs peer, ALL fields)
 *     captured by the engine at the moment the ConfirmedDesync terminal is
 *     set.
 *   - Deterministic machine-readable line formatters, shared by the dump
 *     writer and tools/compare_desync_dumps.py. No wall clock appears in
 *     any formatted line (timestamps live only in dump file headers).
 *
 * This TU links into engine2_tests (formatting + ring semantics are unit
 * pinned there) as well as the mod DLL.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>

namespace Rollback {

/// Ring capacity: the last 64 confirmed frames — more than two SyncHash
/// cadence intervals (30), so the window always spans the failing exchange.
constexpr uint32_t DESYNC_DIAG_RING_CAPACITY = 64;

/// One confirmed frame's diagnostic record (confirm-seam fields only —
/// everything here is final by construction, never predicted).
struct DesyncDiagEntry {
    uint32_t frame = 0;          // canonical confirmed frame
    uint32_t epoch = 0;
    uint64_t gameplay_hash = 0;  // pre-tick sim-affecting digest (Block64)
    uint32_t rng = 0;            // rng seed diagnostic (pre-tick)
    uint16_t hp0 = 0;
    uint16_t hp1 = 0;
    uint16_t p1_input = 0;       // canonical (actual) inputs for the frame
    uint16_t p2_input = 0;
};

/// Fixed-capacity rolling ring; overwrites oldest. No allocation, no clock.
class DesyncDiagRing {
public:
    void Reset();
    void Push(const DesyncDiagEntry& e);
    /// Number of retained entries (<= capacity).
    uint32_t Count() const { return count_; }
    /// index 0 = oldest retained entry, Count()-1 = newest.
    bool At(uint32_t index, DesyncDiagEntry* out) const;

private:
    DesyncDiagEntry entries_[DESYNC_DIAG_RING_CAPACITY] = {};
    uint32_t next_ = 0;    // next write slot
    uint32_t count_ = 0;   // retained entries
};

/// The failing SyncHash pair — every wire field, both sides. Captured by
/// RollbackEngine::ReceiveSyncHash when it sets the ConfirmedDesync
/// terminal; read back by the adapter for the evidence dump.
struct DesyncEvidence {
    bool     valid = false;
    uint32_t frame = 0;          // canonical confirmed frame that failed
    uint32_t epoch = 0;
    uint64_t local_hash = 0;
    uint64_t peer_hash = 0;
    uint32_t local_rng = 0;
    uint32_t peer_rng = 0;
    uint16_t local_hp0 = 0;
    uint16_t local_hp1 = 0;
    uint16_t peer_hp0 = 0;
    uint16_t peer_hp1 = 0;
};

/// First divergent field in comparison order (hash is authoritative and is
/// compared first, then rng, hp0, hp1): returns "hash", "rng", "hp0",
/// "hp1", or "none" when every field matches (or evidence is invalid).
const char* DesyncEvidence_FirstDivergentField(const DesyncEvidence& ev);

/// One machine-greppable line per ring entry (no trailing newline):
///   RING frame=<u> epoch=<u> hash=<016llx> rng=<08x> hp0=<u> hp1=<u>
///        p1=0x<04x> p2=0x<04x>
/// Returns snprintf-style length. Deterministic: same entry, same bytes.
int DesyncDiag_FormatRingEntry(const DesyncDiagEntry& e, char* buf, size_t bufSize);

/// One machine-greppable evidence line (no trailing newline):
///   EVIDENCE frame=<u> epoch=<u> local_hash=<016llx> peer_hash=<016llx>
///            local_rng=<08x> peer_rng=<08x> local_hp0=.. local_hp1=..
///            peer_hp0=.. peer_hp1=.. first_divergent=<field>
int DesyncDiag_FormatEvidence(const DesyncEvidence& ev, char* buf, size_t bufSize);

/// FILE* writers over the formatters above (oldest ring entry first).
void DesyncDiag_WriteRing(FILE* f, const DesyncDiagRing& ring);
void DesyncDiag_WriteEvidence(FILE* f, const DesyncEvidence& ev);

} // namespace Rollback
