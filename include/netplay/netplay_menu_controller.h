/**
 * Alice Senki 2 - Custom in-game netplay menu controller
 *
 * This controller intercepts the vanilla Mode 3 -> Mode 4 netplay transition
 * and replaces it with a mod-owned in-game menu implementation. The custom
 * branch stays inside the live main-menu scene and uses the game's own
 * render/update path instead of the proxy ImGui overlay.
 */

#pragma once

#include <stdint.h>

namespace NetplayMenuController {

enum class NetplayRootBranch : uint32_t {
    DirectPlay = 0,
    Lobbies,
    Settings,
    Spectate,
};

enum class SettingsCategory : uint32_t {
    Identity = 0,
    Endpoint,
    SessionMatch,
    TransportCompatibility,
    Diagnostics,
};

enum class NetplayMenuState : uint32_t {
    Inactive = 0,
    MenuRoot,
    DirectConnectEntry,
    HostEntry,
    JoinEntry,
    SettingsCategoryMenu,
    SettingsEntry,
    Connecting,
    Handshake,
    ConnectedSession,
    CharSelTransition,
    PostMatch,
    DisconnectError,
};

struct Snapshot {
    bool initialized;
    bool hook_installed;
    bool menu_active;
    bool captures_input;
    bool intercept_enabled;
    bool last_intercept_fade;
    uint32_t intercept_count;
    uint32_t last_intercept_source_mode;
    uint32_t current_game_mode;
    uint32_t current_game_type;
    NetplayRootBranch active_root_branch;
    SettingsCategory active_settings_category;
    uint32_t state_revision;
    uint32_t selected_index;
    int fade_frames;
    bool connection_stats_visible;
    bool hud_visible;
    NetplayMenuState state;
    char status[128];
    char last_error[128];
};

bool Initialize();
void Shutdown();

void FrameUpdate();
void HandleDisconnection(const char* reason);

bool IsMenuActive();
bool ConsumesGameInput();
const char* GetStateName(NetplayMenuState state);
bool GetSnapshot(Snapshot* out);

} // namespace NetplayMenuController
