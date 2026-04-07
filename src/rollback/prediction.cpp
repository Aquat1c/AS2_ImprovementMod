/**
 * Alice Senki 2 - Input Prediction Implementation
 */

#include "rollback/prediction.h"
#include "rollback/input_timeline.h"
#include "rollback/netplay_log.h"
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
    NetplayLog_Write("PREDICT", -1, "Prediction initialized: strategy=RepeatLast");
}

void Prediction_Shutdown() {
    s_initialized = false;
}

void Prediction_Reset() {
    s_lastConfirmedInput = INPUT_NEUTRAL;
    s_lastConfirmedFrame = -1;
    LOG_INFO("[Prediction] Reset");
    NetplayLog_Write("PREDICT", -1, "Prediction reset");
}

// ============================================================================
// Configuration
// ============================================================================

void Prediction_SetStrategy(PredictionStrategy strategy) {
    s_strategy = strategy;
    LOG_INFO("[Prediction] Strategy set to %s",
        strategy == PredictionStrategy::RepeatLast ? "RepeatLast" : "Neutral");
    NetplayLog_Write("PREDICT", -1,
        "Strategy set to %s",
        strategy == PredictionStrategy::RepeatLast ? "RepeatLast" : "Neutral");
}

PredictionStrategy Prediction_GetStrategy() {
    return s_strategy;
}

// ============================================================================
// Prediction
// ============================================================================

uint16_t Prediction_PredictRemote(int32_t frame) {
    uint16_t predicted = INPUT_NEUTRAL;

    switch (s_strategy) {
        case PredictionStrategy::RepeatLast:
            predicted = s_lastConfirmedInput;
            break;

        case PredictionStrategy::Neutral:
        default:
            predicted = INPUT_NEUTRAL;
            break;
    }

    NetplayLog_Verbose("PREDICT", frame,
        "Predict remote: strategy=%s input=0x%04X last_confirmed_frame=%d",
        s_strategy == PredictionStrategy::RepeatLast ? "RepeatLast" : "Neutral",
        predicted,
        s_lastConfirmedFrame);

    return predicted;
}

void Prediction_OnRemoteConfirmed(int32_t frame, uint16_t input) {
    if (frame > s_lastConfirmedFrame) {
        s_lastConfirmedInput = input;
        s_lastConfirmedFrame = frame;
        NetplayLog_Verbose("PREDICT", frame,
            "Confirmed remote input: input=0x%04X",
            input);
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
