/**
 * Alice Senki 2 - Stage Select Sync
 *
 * Confirmed shared-input buffer for the shared stage cursor (charsel sub 7).
 *
 * Both machines receive identical lockstep-confirmed P1/P2 inputs per frame.
 * We pre-merge them into a SINGLE shared input before writing to the game,
 * so both machines feed the game exactly one identical input for the shared
 * cursor.  The merge is a pure function — no internal state, no cooldown.
 *
 * Merge: OR all inputs, cancel opposing directions (Left+Right → 0, Up+Down → 0).
 * Merged input goes to P1 slot; P2 is zeroed.
 *
 * Source-of-truth: decompilation of sub_5C0B20 (stage select grid handler).
 */

#include "net/stagesel_sync.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <string.h>

namespace {

// ============================================================================
// Constants
// ============================================================================

static const int AUDIT_RING_SIZE = 128;
static const int AUDIT_RING_MASK = AUDIT_RING_SIZE - 1;

// Game-packed direction masks
static const uint16_t MASK_DOWN  = (1 << 0);
static const uint16_t MASK_UP    = (1 << 1);
static const uint16_t MASK_LEFT  = (1 << 2);
static const uint16_t MASK_RIGHT = (1 << 3);
static const uint16_t MASK_VERT  = MASK_UP | MASK_DOWN;
static const uint16_t MASK_HORZ  = MASK_LEFT | MASK_RIGHT;

// Select button — stripped during stage select to prevent roulette
// (roulette uses rand() which is not synced during charsel → guaranteed desync).
// B/D still available for cancel, Start for back.
static const uint16_t MASK_SELECT = (1 << 9);  // 0x0200

// ============================================================================
// Internal state
// ============================================================================

static bool     s_initialized = false;
static bool     s_active      = false;
static bool     s_needsEdgeReset = false;

// Confirmed frame audit ring (for desync diagnostics)
static Net::StageSelConfirmedFrame s_auditRing[AUDIT_RING_SIZE] = {};
static uint32_t s_auditWriteIdx  = 0;
static uint32_t s_confirmedCount = 0;

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace Net {

void StageSelSync_Init() {
    if (s_initialized) return;
    s_active = false;
    s_auditWriteIdx = 0;
    s_confirmedCount = 0;
    memset(s_auditRing, 0, sizeof(s_auditRing));
    s_initialized = true;
}

void StageSelSync_Shutdown() {
    if (!s_initialized) return;
    s_active = false;
    s_initialized = false;
}

void StageSelSync_Begin() {
    s_active = true;
    s_needsEdgeReset = true;
    s_auditWriteIdx = 0;
    s_confirmedCount = 0;
    memset(s_auditRing, 0, sizeof(s_auditRing));

    Rollback::NetplayLog_Write("STAGESEL", -1,
        "=== STAGE SELECT SYNC BEGIN (single merge authority at raw input injection) ===");
    LOG_NETPLAY(LOG_INFO, "[StageSelSync] Begin — stateless merge, single authority at Hook_InputProcess");
}

void StageSelSync_Abort() {
    if (!s_active) return;
    s_active = false;
    LOG_NETPLAY(LOG_INFO, "[StageSelSync] Aborted (confirmed %u frames)", s_confirmedCount);
}

bool StageSelSync_IsActive() {
    return s_active;
}

bool StageSelSync_ConsumeEdgeReset() {
    if (s_needsEdgeReset) {
        s_needsEdgeReset = false;
        return true;
    }
    return false;
}

uint16_t StageSelSync_MergeConfirmed(uint32_t frame, uint16_t p1, uint16_t p2) {
    // Pure function: OR all inputs, cancel opposing directions.
    // No internal state — identical (p1, p2) always yields identical result.
    uint16_t combined = p1 | p2;

    // Strip Select button: stage select roulette (hidden feature activated by
    // holding A/C + pressing Select during confirm) calls rand() which is NOT
    // synced during charsel — guaranteed desync if triggered.
    // B/D still available for cancel, Start for back. Select has no other
    // legitimate use in stage select (sub=7) or confirm menu (sub=8).
    combined &= ~MASK_SELECT;

    // Cancel opposing axes: both Left+Right → neither moves the cursor
    if ((combined & MASK_HORZ) == MASK_HORZ)
        combined &= ~MASK_HORZ;
    if ((combined & MASK_VERT) == MASK_VERT)
        combined &= ~MASK_VERT;

    // Record in audit ring
    uint32_t idx = s_auditWriteIdx & AUDIT_RING_MASK;
    s_auditRing[idx].frame  = frame;
    s_auditRing[idx].p1     = p1;
    s_auditRing[idx].p2     = p2;
    s_auditRing[idx].merged = combined;
    s_auditWriteIdx++;
    s_confirmedCount++;

    return combined;
}

bool StageSelSync_GetLastConfirmedFrame(StageSelConfirmedFrame* out) {
    if (s_confirmedCount == 0 || !out) return false;
    uint32_t idx = (s_auditWriteIdx - 1) & AUDIT_RING_MASK;
    *out = s_auditRing[idx];
    return true;
}

uint32_t StageSelSync_GetConfirmedFrameCount() {
    return s_confirmedCount;
}

} // namespace Net
