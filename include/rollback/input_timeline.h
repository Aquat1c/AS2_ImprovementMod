/**
 * Alice Senki 2 - Input Timeline (re0.7 M4 resurrection, master plan §2.7.3)
 *
 * Canonical-frame input ring for the custom rollback engine (engine2).
 * This is the 0.5-era mod-owned input store, resurrected and finished for
 * the §2.7 pipeline: wrap-safe u32 canonical frames (INV-15), capture-once
 * slot immutability (INV-18), and typed set results so the engine can map
 * an occupied-slot write to adopt / DuplicateIdentical / Conflict without
 * this module guessing intent.
 *
 * Pure and instantiable: no Win32, no game memory, no logging, no clock.
 * The engine owns one ring per side; the unit/soak harness owns its own.
 *
 * Input format: 16-bit bitmask matching the game's native format
 * (INPUT_UP..INPUT_R2, bits 0..13). 0xFFFF is the game's "no input yet"
 * history marker and is unrepresentable as a stored value by contract
 * (validated by the engine ingest, INV-19).
 *
 * Storage is direct-mapped (`frame % capacity`). Because every live window
 * in the engine is tiny compared to the capacity (un-acked suffix <= 32,
 * speculation <= 15, confirm lag bounded), a slot collision can only involve
 * a frame at least `capacity` frames in the past — evicting it is always
 * safe, and a write that finds a *newer* frame in its slot is by definition
 * stale and refused.
 */

#pragma once

#include <stdint.h>

#include <vector>

#include "net/frame_arithmetic.h"

namespace Rollback {

/// Neutral input (no buttons pressed).
constexpr uint16_t INPUT_NEUTRAL = 0x0000;

class InputRing {
public:
    enum class SetResult : uint8_t {
        Stored,     // slot was empty — value sealed
        Occupied,   // identical value already sealed (capture-once adopt path)
        Conflict,   // DIFFERENT value already sealed (immutability violation)
        StaleSlot,  // slot holds a newer frame — the write is ancient/stale
    };

    InputRing() = default;

    /// (Re)arm the ring. Frames have no retention floor beyond the eviction
    /// rule above; `capacity` should exceed every live window by orders of
    /// magnitude (engine default 4096).
    void Reset(uint32_t capacity, uint16_t neutral) {
        neutral_ = neutral;
        slots_.assign(capacity ? capacity : 1u, Slot{});
    }

    bool Armed() const { return !slots_.empty(); }
    uint32_t Capacity() const { return (uint32_t)slots_.size(); }

    bool Has(uint32_t frame) const {
        if (slots_.empty()) return false;
        const Slot& s = slots_[frame % slots_.size()];
        return s.present && s.frame == frame;
    }

    /// Value at `frame`, or neutral when absent.
    uint16_t Value(uint32_t frame) const {
        if (slots_.empty()) return neutral_;
        const Slot& s = slots_[frame % slots_.size()];
        return (s.present && s.frame == frame) ? s.value : neutral_;
    }

    /// Seal `frame` with `value`. Assigned slots are immutable forever
    /// (INV-18): a repeat write with the same value reports Occupied (the
    /// caller adopts), a different value reports Conflict (the caller fails
    /// closed, INV-19). A slot held by a newer frame refuses the write as
    /// StaleSlot; a slot held by an older frame is evicted (>= capacity
    /// frames in the past by construction).
    SetResult Set(uint32_t frame, uint16_t value) {
        if (slots_.empty()) return SetResult::StaleSlot;
        Slot& s = slots_[frame % slots_.size()];
        if (s.present) {
            if (s.frame == frame) {
                return (s.value == value) ? SetResult::Occupied
                                          : SetResult::Conflict;
            }
            if (Net::frameAfter(s.frame, frame)) {
                return SetResult::StaleSlot;
            }
            // Older frame: fell out of every live window — evict.
        }
        s.present = true;
        s.frame = frame;
        s.value = value;
        return SetResult::Stored;
    }

private:
    struct Slot {
        uint32_t frame = 0;
        uint16_t value = 0;
        bool     present = false;
    };

    std::vector<Slot> slots_;
    uint16_t neutral_ = INPUT_NEUTRAL;
};

} // namespace Rollback
