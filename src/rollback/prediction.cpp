/**
 * Alice Senki 2 - Input Prediction Implementation
 */

#include "rollback/prediction.h"
#include "rollback/input_timeline.h"
#include "ui/log_window.h"

namespace Rollback {

// ============================================================================
// Internal State
// ============================================================================

static PredictionStrategy s_strategy            = PredictionStrategy::RepeatLast;
static uint16_t           s_lastConfirmedInput   = INPUT_NEUTRAL;
static int32_t            s_lastConfirmedFrame   = -1;
static bool               s_initialized          = false;

// ============================================================================
// Lifecycle
// ============================================================================

void Prediction_Init() {
    s_strategy = PredictionStrategy::RepeatLast;
    s_lastConfirmedInput = INPUT_NEUTRAL;
    s_lastConfirmedFrame = -1;
    s_initialized = true;
    LOG_INFO("[Prediction] Initialized (strategy=RepeatLast)");
}

void Prediction_Shutdown() {
    s_initialized = false;
}

void Prediction_Reset() {
    s_lastConfirmedInput = INPUT_NEUTRAL;
    s_lastConfirmedFrame = -1;
    LOG_INFO("[Prediction] Reset");
}

// ============================================================================
// Configuration
// ============================================================================

void Prediction_SetStrategy(PredictionStrategy strategy) {
    s_strategy = strategy;
    LOG_INFO("[Prediction] Strategy set to %s",
        strategy == PredictionStrategy::RepeatLast ? "RepeatLast" : "Neutral");
}

PredictionStrategy Prediction_GetStrategy() {
    return s_strategy;
}

// ============================================================================
// Prediction
// ============================================================================

uint16_t Prediction_PredictRemote(int32_t frame) {
    (void)frame;

    switch (s_strategy) {
        case PredictionStrategy::RepeatLast:
            return s_lastConfirmedInput;

        case PredictionStrategy::Neutral:
        default:
            return INPUT_NEUTRAL;
    }
}

void Prediction_OnRemoteConfirmed(int32_t frame, uint16_t input) {
    if (frame > s_lastConfirmedFrame) {
        s_lastConfirmedInput = input;
        s_lastConfirmedFrame = frame;
    }
}

// ============================================================================
// Diagnostics
// ============================================================================

void Prediction_GetSnapshot(PredictionSnapshot* out) {
    if (!out) return;
    out->strategy = s_strategy;
    out->last_confirmed_input = s_lastConfirmedInput;
    out->last_confirmed_frame = s_lastConfirmedFrame;
}

} // namespace Rollback
