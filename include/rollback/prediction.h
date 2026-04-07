/**
 * Alice Senki 2 - Input Prediction Policy
 *
 * Centralized prediction for missing remote inputs during rollback.
 * First-pass strategy: repeat last confirmed remote input.
 * Falls back to neutral if no confirmed input exists.
 *
 * All prediction goes through this module so the policy is explicit
 * and can be swapped or extended later.
 */

#pragma once

#include <stdint.h>

namespace Rollback {

// ============================================================================
// Prediction Strategy
// ============================================================================

enum class PredictionStrategy : uint8_t {
    RepeatLast = 0,    // Repeat last confirmed remote input
    Neutral,           // Always predict neutral (no buttons)
};

// ============================================================================
// Lifecycle
// ============================================================================

void Prediction_Init();
void Prediction_Shutdown();

/// Reset prediction state (call at session/match start).
void Prediction_Reset();

// ============================================================================
// Configuration
// ============================================================================

/// Set the active prediction strategy.
void Prediction_SetStrategy(PredictionStrategy strategy);

/// Get the active prediction strategy.
PredictionStrategy Prediction_GetStrategy();

// ============================================================================
// Prediction
// ============================================================================

/// Predict remote input for the given frame.
/// Uses the configured strategy and internal state to produce a prediction.
/// The caller is responsible for writing this to the input timeline.
uint16_t Prediction_PredictRemote(int32_t frame);

/// Notify the prediction module that a confirmed remote input has arrived.
/// This updates the internal "last known" state used by RepeatLast strategy.
void Prediction_OnRemoteConfirmed(int32_t frame, uint16_t input);

// ============================================================================
// Diagnostics
// ============================================================================

struct PredictionSnapshot {
    PredictionStrategy strategy;
    uint16_t           last_confirmed_input;  // Last known real remote input
    int32_t            last_confirmed_frame;  // Frame of last confirmed remote
};

void Prediction_GetSnapshot(PredictionSnapshot* out);

} // namespace Rollback
