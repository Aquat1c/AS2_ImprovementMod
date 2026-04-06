/**
 * Alice Senki 2 - Visual Rollback Smoothing Implementation
 *
 * Exponential-decay offset system: after a rollback correction, the
 * visual offset is set to (last_rendered - new_sim) so the character
 * appears to stay in place. Each subsequent frame the offset decays
 * toward zero, converging on the true sim position.
 *
 * Entity memory is only modified during the render window:
 *   Hook_AdvanceFrame → ApplyBeforeRender (write visual)
 *   Hook_RenderPresent → RestoreAfterRender (write sim back)
 */

#include "visual_smoothing.h"
#include "as2_constants.h"
#include "game_state.h"
#include "log_window.h"
#include <cmath>
#include <cstring>

// ============================================================================
// Tuning constants
// ============================================================================

// Fraction of the offset that decays each frame. 0.4 = 40% convergence/frame,
// so a 10-unit offset reaches <1 unit in ~5 frames (~83ms at 60fps).
static constexpr float kLerpRate = 0.4f;

// If offset magnitude exceeds this, snap immediately (no smooth).
// 150 game units = 15 pixels. Large corrections shouldn't be smoothed
// because they indicate a significant misprediction.
static constexpr float kSnapThreshold = 150.0f;

// Below this magnitude, zero the offset (sub-pixel, not visible).
static constexpr float kZeroThreshold = 0.5f;

// ============================================================================
// Per-player visual state
// ============================================================================

struct PlayerVisualState {
    float   offset_x = 0.0f;       // Current visual offset (game coords, 1/10 pixel)
    float   offset_y = 0.0f;
    int16_t saved_sim_x = 0;       // Sim position saved before visual override
    int16_t saved_sim_y = 0;
    int16_t last_rendered_x = 0;   // Position that was actually rendered
    int16_t last_rendered_y = 0;
    bool    initialized = false;   // Have we captured last_rendered at least once?
};

static PlayerVisualState s_players[2];
static bool s_rollback_just_ended = false;
static bool s_need_restore = false;

static VisualSmoothing::Stats s_stats = {};

// ============================================================================
// Helpers
// ============================================================================

static inline int16_t ReadEntityX(int player) {
    uintptr_t base = (player == 0) ? ADDR_P1_ENTITY_BASE : ADDR_P2_ENTITY_BASE;
    return *reinterpret_cast<volatile int16_t*>(base + ENTITY_OFF_X_POS);
}

static inline int16_t ReadEntityY(int player) {
    uintptr_t base = (player == 0) ? ADDR_P1_ENTITY_BASE : ADDR_P2_ENTITY_BASE;
    return *reinterpret_cast<volatile int16_t*>(base + ENTITY_OFF_Y_POS);
}

static inline void WriteEntityX(int player, int16_t val) {
    uintptr_t base = (player == 0) ? ADDR_P1_ENTITY_BASE : ADDR_P2_ENTITY_BASE;
    *reinterpret_cast<volatile int16_t*>(base + ENTITY_OFF_X_POS) = val;
}

static inline void WriteEntityY(int player, int16_t val) {
    uintptr_t base = (player == 0) ? ADDR_P1_ENTITY_BASE : ADDR_P2_ENTITY_BASE;
    *reinterpret_cast<volatile int16_t*>(base + ENTITY_OFF_Y_POS) = val;
}

static inline float OffsetMagnitude(float ox, float oy) {
    return sqrtf(ox * ox + oy * oy);
}

static inline void ReanchorToSimulation(int player, PlayerVisualState& vs) {
    const int16_t sim_x = ReadEntityX(player);
    const int16_t sim_y = ReadEntityY(player);
    vs.offset_x = 0.0f;
    vs.offset_y = 0.0f;
    vs.saved_sim_x = sim_x;
    vs.saved_sim_y = sim_y;
    vs.last_rendered_x = sim_x;
    vs.last_rendered_y = sim_y;
    vs.initialized = true;
}

// ============================================================================
// Implementation
// ============================================================================

namespace VisualSmoothing {

void Reset() {
    for (int i = 0; i < 2; ++i) {
        s_players[i] = PlayerVisualState{};
    }
    s_rollback_just_ended = false;
    s_need_restore = false;
    s_stats = {};
    LOG_NET_INFO("[VisualSmooth] Reset");
}

void OnRollbackEnd() {
    s_rollback_just_ended = true;
}

void ApplyBeforeRender() {
    // Mode 8 Substate 3 includes non-interactive intro / transition phases.
    // We only want smoothing during the actual playable fighting window;
    // otherwise offsets can bleed into round-start / round-end presentation.
    if (!IsInPlayableMatchGameplay()) {
        for (int p = 0; p < 2; ++p) {
            ReanchorToSimulation(p, s_players[p]);
        }
        s_rollback_just_ended = false;
        s_need_restore = false;
        return;
    }

    // For each player: read sim position, compute visual, write visual
    for (int p = 0; p < 2; ++p) {
        PlayerVisualState& vs = s_players[p];

        int16_t sim_x = ReadEntityX(p);
        int16_t sim_y = ReadEntityY(p);

        // First frame: initialize tracking, no offset
        if (!vs.initialized) {
            vs.last_rendered_x = sim_x;
            vs.last_rendered_y = sim_y;
            vs.saved_sim_x = sim_x;
            vs.saved_sim_y = sim_y;
            vs.initialized = true;
            continue;
        }

        bool seeded_offset_this_frame = false;

        // On rollback end: compute offset = last_rendered - sim
        // This makes the visual position stay where it was last frame
        if (s_rollback_just_ended) {
            float new_ox = (float)(vs.last_rendered_x - sim_x);
            float new_oy = (float)(vs.last_rendered_y - sim_y);

            float mag = OffsetMagnitude(new_ox, new_oy);
            if (mag > kSnapThreshold) {
                // Too large — snap to sim position immediately
                new_ox = 0.0f;
                new_oy = 0.0f;
            }

            vs.offset_x = new_ox;
            vs.offset_y = new_oy;
            seeded_offset_this_frame = true;

            if (mag > 0.0f) {
                s_stats.rollback_events++;
                if (mag > s_stats.max_offset_seen) {
                    s_stats.max_offset_seen = mag;
                }
                LOG_NET_DEBUG("[VisualSmooth] P%d rollback offset: (%.1f, %.1f) mag=%.1f",
                              p + 1, new_ox, new_oy, mag);
            }
        }

        // Decay toward zero on subsequent frames. Do not decay on the same
        // frame a rollback correction lands, or we partially reintroduce the
        // snap we were trying to hide.
        if (!seeded_offset_this_frame) {
            vs.offset_x *= (1.0f - kLerpRate);
            vs.offset_y *= (1.0f - kLerpRate);
        }

        // Zero out sub-pixel offsets
        if (fabsf(vs.offset_x) < kZeroThreshold) vs.offset_x = 0.0f;
        if (fabsf(vs.offset_y) < kZeroThreshold) vs.offset_y = 0.0f;

        // Save sim positions for restore
        vs.saved_sim_x = sim_x;
        vs.saved_sim_y = sim_y;

        // Apply visual: write entity = sim + offset
        float visual_x = (float)sim_x + vs.offset_x;
        float visual_y = (float)sim_y + vs.offset_y;

        // Clamp to int16 range
        if (visual_x > 32767.0f) visual_x = 32767.0f;
        if (visual_x < -32768.0f) visual_x = -32768.0f;
        if (visual_y > 32767.0f) visual_y = 32767.0f;
        if (visual_y < -32768.0f) visual_y = -32768.0f;

        int16_t write_x = (int16_t)visual_x;
        int16_t write_y = (int16_t)visual_y;

        WriteEntityX(p, write_x);
        WriteEntityY(p, write_y);

        vs.last_rendered_x = write_x;
        vs.last_rendered_y = write_y;

        if (vs.offset_x != 0.0f || vs.offset_y != 0.0f) {
            s_stats.smoothing_frames++;
        }
    }

    // Clear the rollback-end flag after processing both players
    s_rollback_just_ended = false;
    s_need_restore = true;
}

void RestoreAfterRender() {
    if (!s_need_restore) return;

    for (int p = 0; p < 2; ++p) {
        PlayerVisualState& vs = s_players[p];
        if (!vs.initialized) continue;

        WriteEntityX(p, vs.saved_sim_x);
        WriteEntityY(p, vs.saved_sim_y);
    }

    s_need_restore = false;
}

void GetStats(Stats* out) {
    if (!out) return;
    *out = s_stats;
    // Compute current offset magnitudes
    out->current_offset_p1 = OffsetMagnitude(s_players[0].offset_x, s_players[0].offset_y);
    out->current_offset_p2 = OffsetMagnitude(s_players[1].offset_x, s_players[1].offset_y);
}

} // namespace VisualSmoothing
