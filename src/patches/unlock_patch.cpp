/**
 * Alice Senki 2 - Unlock Patch
 * 
 * Force-unlocks all characters, boss, gallery, and stage select.
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#include "unlock_patch.h"
#include "as2_constants.h"
#include "log_window.h"
#include <stdint.h>

void UnlockAllContent() {
    // Set all 80 unlock flag bytes to 1 — this covers:
    //  - Character unlocks (byte_8163CC[0..16])
    //  - Boss unlock (byte_8163DD)
    //  - Arcade/story clear flags
    //  - Gallery entries
    volatile uint8_t* flags = (volatile uint8_t*)ADDR_UNLOCK_FLAGS_BASE;
    for (int i = 0; i < ADDR_UNLOCK_FLAGS_SIZE; i++) {
        flags[i] = 1;
    }

    // Enable stage selection by default (BYTE2 of dword_8E93EC)
    *(volatile uint8_t*)ADDR_STAGESEL_ENABLE = 1;

    LOG_INFO("All content unlocked (characters, boss, gallery, stage select enabled)");
}
