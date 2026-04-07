#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Input Sync Hooks
// ============================================================================
// Hooks for vanilla netplay functions (send/recv/sync/advanceFrame/initialSync)
// and load barrier freeze mechanism.
//
// When a mod session owns synchronization (charsel lockstep, load barrier,
// or active session), these hooks suppress the vanilla netplay code path
// to prevent interference or blocking.

// Install all vanilla netplay suppression hooks.
// Must be called AFTER MH_Initialize and after primary input hooks are installed.
bool InputSyncHooks_Install();

// Load barrier freeze: pauses gameplay while keeping game loop alive
// for packet exchange, rendering, and SessionManager updates.
void InputSyncHooks_SetLoadBarrierFreeze(bool freeze);
bool InputSyncHooks_IsLoadBarrierFrozen();

// Runtime freeze: used by gameplay timesync when the local simulator is too
// far ahead of confirmed remote input. Keeps the main loop alive while
// suppressing match advancement.
void InputSyncHooks_SetTimesyncFreeze(bool freeze);
bool InputSyncHooks_IsTimesyncFrozen();
bool InputSyncHooks_IsGameplayFreezeActive();

// Query: is the mod currently owning synchronization?
// (charsel lockstep active, load barrier frozen, or session connected)
bool InputSyncHooks_IsModOwnedSync();
