/**
 * Alice Senki 2 - Unlock Patch
 * 
 * Force-unlocks all characters, boss, gallery, and stage select.
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#pragma once

// Unlocks all 80 flag bytes at ADDR_UNLOCK_FLAGS_BASE and enables stage select.
// Must be called AFTER config.dat is loaded (config version == 258 at 0x8163BC).
void UnlockAllContent();
