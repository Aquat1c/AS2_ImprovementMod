/**
 * Alice Senki 2 - Input Prediction (re0.7 M4 resurrection, master plan §2.7.3-R)
 *
 * Hold-last-actual prediction for missing remote inputs. Resurrected from the
 * pre-Gekko module as a pure, instantiable policy object for engine2.
 *
 * The predictor itself is trivial by design (QOH99 §2.2): predict the last
 * *actual* remote input. Earliest-mismatch tracking lives in the engine —
 * the predictor only answers "what do we play when the wire is silent".
 *
 * Pure: no Win32, no game memory, no logging, no clock.
 */

#pragma once

#include <stdint.h>

#include "net/frame_arithmetic.h"

namespace Rollback {

class HoldLastPredictor {
public:
    void Reset(uint16_t neutral) {
        neutral_ = neutral;
        last_value_ = neutral;
        has_actual_ = false;
        last_frame_ = 0;
        total_predictions_ = 0;
    }

    /// Feed a confirmed (actual) remote input. Only the newest frame wins;
    /// out-of-order actuals for older frames do not regress the holding value.
    void OnActual(uint32_t frame, uint16_t value) {
        if (!has_actual_ || Net::frameAtOrAfter(frame, last_frame_)) {
            has_actual_ = true;
            last_frame_ = frame;
            last_value_ = value;
        }
    }

    /// Predict the remote input for a frame with no actual yet.
    uint16_t Predict() {
        ++total_predictions_;
        return has_actual_ ? last_value_ : neutral_;
    }

    /// Non-counting peek (diagnostics).
    uint16_t Held() const { return has_actual_ ? last_value_ : neutral_; }
    bool     HasActual() const { return has_actual_; }
    uint32_t LastActualFrame() const { return last_frame_; }
    uint32_t TotalPredictions() const { return total_predictions_; }

private:
    uint16_t neutral_ = 0;
    uint16_t last_value_ = 0;
    uint32_t last_frame_ = 0;
    uint32_t total_predictions_ = 0;
    bool     has_actual_ = false;
};

} // namespace Rollback
