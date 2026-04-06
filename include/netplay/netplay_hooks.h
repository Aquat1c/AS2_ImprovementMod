/**
 * Alice Senki 2 - Netplay Hooks (clean-slate)
 *
 * Intentionally small:
 * - Lightweight inline state queries for UI/debug.
 * - Small public API used by the mod.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

// Mode/substate constants + addresses
#include "core/game_state.h"
#include "core/as2_constants.h"

// ============================================================================
// NAMESPACE FOR ALL NETPLAY HOOK FUNCTIONS
// ============================================================================

namespace NetplayHooks {

// ============================================================================
// HELPER FUNCTIONS - Game State Queries
// ============================================================================

inline uint32_t GetGameMode() {
    return *reinterpret_cast<uint32_t*>(ADDR_GAME_MODE);
}

inline uint32_t GetSubstate() {
    return *reinterpret_cast<uint32_t*>(ADDR_SUB_STATE);
}

inline uint32_t GetNetplayGameType() {
    return *reinterpret_cast<uint32_t*>(ADDR_GAME_TYPE);
}

// Raw vanilla game-type query for diagnostics only.
inline bool IsNetplayGameType() {
    return GetNetplayGameType() == GAMETYPE_NETPLAY;
}

// Mod-owned netplay state. These do NOT read vanilla role/connected bytes.
uint8_t GetConnectionRole();
bool IsConnected();
bool HasModNetplayMarker();
bool IsInNetworkMode();

inline bool IsInLobbyMode() {
    return GetGameMode() == MODE_LOBBY;
}

bool IsInNetplayLobby();

inline bool IsInCharSelMode() {
    return GetGameMode() == MODE_CHARSEL;
}

bool IsInNetplayCharSel();

// CharSel substates that use vanilla input sync (sub_5625E0 loop)
bool IsInCharSelInputSync();

inline bool IsInMatchMode() {
    return GetGameMode() == MODE_MATCH;
}

bool IsInNetplayMatch();

bool IsInNetplayGameplay();

inline bool IsMatchIntroActive() {
    return ::IsMatchIntroActive();
}

inline bool IsMatchTransitionActive() {
    return ::IsMatchTransitionActive();
}

// Returns true only when netplay is in the interactive fighting window inside
// the broader Mode 8 Substate 3 loop.
inline bool IsInPlayableGameplay() {
    return IsInNetplayGameplay() &&
           !::IsMatchIntroActive() &&
           !::IsMatchTransitionActive();
}

inline uint16_t GetNetworkPort() {
    return *reinterpret_cast<uint16_t*>(ADDR_NETPLAY_PORT);
}

inline uint32_t GetNetplayFrame() {
    return *reinterpret_cast<uint32_t*>(ADDR_SIM_FRAME_COUNTER);
}

inline const char* GetSubstateDescription(uint32_t substate) {
    switch (substate) {
        case 0: return "LoadAssets";
        case 1: return "Setup";
        case 2: return "Init";
        case 3: return "GameplayLoop";
        case 4: return "Pause";
        case 5: return "Match End";
        default: return "Unknown";
    }
}

// Strict per-build signature used to quickly confirm builds match.
uint32_t GetBuildSignature();
void SetAutoHooksEnabled(bool enabled);
void SetNetplayFrameDelay(int delay);
int GetNetplayFrameDelay();

void HandleDisconnection(const char* reason);

// Called each frame from main loop
void NetplayFrameUpdate();
// Debug Save States

bool NetplayDebugSaveState(int slot);
bool NetplayDebugLoadState(int slot);
bool NetplayDebugSlotValid(int slot);
uint32_t NetplayDebugSlotFrame(int slot);
int NetplayDebugSlotCount();
void NetplayDebugClearSlots();

} // namespace NetplayHooks
