/**
 * Alice Senki 2 - Background Gamepad I/O Worker
 *
 * All blocking SDL device work runs on a dedicated worker thread:
 *   - SDL_GetGamepads hotplug scans
 *   - GUID lookup, player-slot assignment, SDL_OpenGamepad / SDL_CloseGamepad
 *
 * The game thread never calls SDL on the connect/disconnect path except
 * SDL_UpdateGamepads + reads on handles it already owns. It receives
 * OpenComplete commands and attaches ready handles to player slots.
 */

#pragma once

#include <SDL3/SDL.h>

#include <stdint.h>
#include <windows.h>

namespace Input {

// Every connected pad is opened into this pool, not just the two a player can
// hold. Players are then pointed at a pool entry, so a third or fourth
// controller can be claimed by rebinding with it.
constexpr int kMaxGamepads = 8;

struct GamepadWorkerSlotSnapshot {
    bool             occupied[kMaxGamepads];
    bool             guid_valid[kMaxGamepads];
    SDL_GUID         guid[kMaxGamepads];
    SDL_JoystickID   live_instance_id[kMaxGamepads];
};

struct GamepadWorkerOpenResult {
    int              slot;
    SDL_JoystickID   instance_id;
    SDL_Gamepad*     gamepad;      // set when SDL has a standardized mapping
    SDL_Joystick*    joystick;     // set instead for a raw, unmapped stick
    SDL_GUID         guid;
    bool             guid_valid;
    DWORD            duration_ms;
};

struct GamepadWorkerResult {
    GamepadWorkerOpenResult attach{};
};

struct GamepadWorkerStats {
    bool     worker_running;
    uint32_t command_queue_depth;
    uint32_t result_queue_depth;
    uint32_t command_drop_count;
    uint32_t result_drop_count;
    uint32_t closes_completed;
    uint32_t opens_completed;
    uint32_t opens_cancelled;
    uint32_t hotplug_scans_completed;
    uint32_t hotplug_connects_queued;
    DWORD    last_close_duration_ms;
    DWORD    last_open_duration_ms;
    DWORD    last_scan_duration_ms;
};

bool GamepadWorker_Init();
void GamepadWorker_Shutdown();

void GamepadWorker_UpdateSlotSnapshot(const GamepadWorkerSlotSnapshot* snapshot);
void GamepadWorker_RequestBootstrap();

bool GamepadWorker_RequestClose(SDL_Gamepad* gamepad);
void GamepadWorker_CancelOpen(SDL_JoystickID instance_id);

bool GamepadWorker_TryPopResult(GamepadWorkerResult* out);
void GamepadWorker_GetStats(GamepadWorkerStats* out);

/// True while worker is performing or recently finished blocking device I/O.
bool GamepadWorker_IsChurnActive();

} // namespace Input
