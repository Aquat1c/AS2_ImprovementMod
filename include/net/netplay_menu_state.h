/**
 * Alice Senki 2 - Netplay Menu State Types
 *
 * Shared type definitions for the netplay menu state machine.
 * Used by both the menu controller and the menu UI renderer.
 */

#pragma once

#include <stdint.h>

namespace NetMenu {

// ============================================================================
// Menu State Machine
// ============================================================================

enum class MenuState : uint32_t {
    Inactive = 0,        // Menu not displayed
    MenuRoot,            // Root: Direct Play, Settings
    DirectConnectEntry,  // Host / Join selection
    HostEntry,           // Host config (port, start)
    JoinEntry,           // Join config (endpoint, recent peers)
    SpectateEntry,       // Spectator client connect entry
    SpectatorConnecting, // Spectator sidecar connect in progress
    SpectatorConnected,  // Spectator sidecar stream active
    SettingsCategoryMenu,// Settings category selection
    SettingsEntry,       // Individual settings
    Connecting,          // Waiting for peer
    Handshake,           // Exchanging hello/ack
    ConnectedSession,    // Session established, ready for CharSel
    CharSelTransition,   // Transitioning to CharSel
    PostMatch,           // After match, rematch/disconnect
    DisconnectError,     // Disconnect or error occurred
};

inline const char* MenuStateName(MenuState state) {
    switch (state) {
        case MenuState::Inactive:             return "Inactive";
        case MenuState::MenuRoot:             return "MenuRoot";
        case MenuState::DirectConnectEntry:   return "DirectConnect";
        case MenuState::HostEntry:            return "HostEntry";
        case MenuState::JoinEntry:            return "JoinEntry";
        case MenuState::SpectateEntry:        return "SpectateEntry";
        case MenuState::SpectatorConnecting:  return "SpectatorConnecting";
        case MenuState::SpectatorConnected:   return "SpectatorConnected";
        case MenuState::SettingsCategoryMenu: return "SettingsCategory";
        case MenuState::SettingsEntry:        return "SettingsEntry";
        case MenuState::Connecting:           return "Connecting";
        case MenuState::Handshake:            return "Handshake";
        case MenuState::ConnectedSession:     return "ConnectedSession";
        case MenuState::CharSelTransition:    return "CharSelTransition";
        case MenuState::PostMatch:            return "PostMatch";
        case MenuState::DisconnectError:      return "DisconnectError";
        default:                              return "Unknown";
    }
}

// ============================================================================
// Menu Phase (visual state)
// ============================================================================

enum class MenuPhase : uint8_t {
    Hidden = 0,
    Opening,
    Active,
    Closing,
};

// ============================================================================
// Root Branch
// ============================================================================

enum class RootBranch : uint32_t {
    DirectPlay = 0,
    Settings,
    Spectate,  // Placeholder
};

// ============================================================================
// Settings Category
// ============================================================================

enum class SettingsCategory : uint32_t {
    Identity = 0,
    Appearance,
    Endpoint,
    SessionMatch,
    Diagnostics,
    GameGeneral,
    GameVoice,
    GameRoot,
    GameKeys,
    GameSystem,
};

// ============================================================================
// Text Edit Field
// ============================================================================

enum class TextEditField : uint32_t {
    None = 0,
    Nickname,
    RemoteEndpoint,
    SpectatorEndpoint,
    ListenPort,
    SpectatorPort,
    RelayEndpoint,
    StunEndpoint,
};

// ============================================================================
// Menu Snapshot (for UI and diagnostics)
// ============================================================================

struct MenuSnapshot {
    bool          initialized;
    bool          menu_active;
    bool          captures_input;
    MenuState     state;
    MenuPhase     phase;
    RootBranch    root_branch;
    SettingsCategory settings_category;
    uint32_t      selected_index;
    int           fade_frames;
    char          status[128];
    char          last_error[128];
    char          your_address[96];
    char          clipboard_flash[48];
    char          local_nickname[64];
    char          peer_nickname[64];
    float         rtt_ms;
    int           local_wins;
    int           remote_wins;
    char          hud_trail_color_label[32];
    char          hud_text_color_label[32];
    char          hud_score_color_label[32];
    char          hud_vertical_position_label[32];
    char          hud_font_size_label[32];
    char          hud_render_mode_label[32];
    int           hud_trail_length;

    // Connection config (editable by user)
    uint16_t      listen_port;
    char          remote_endpoint[96];
    char          spectator_endpoint[96];
    int           preferred_delay;
    int           connection_mode;     // Net::ConnectPreference
    bool          upnp_enabled;
    bool          stun_enabled;
    bool          hole_punch_enabled;
    bool          allow_ipv6_endpoint;
    char          relay_endpoint[96];
    char          stun_endpoint[96];
    char          nat_status[128];
    char          nat_route_status[96];
    char          nat_mapping_status[96];
    char          nat_punch_status[96];
    char          nat_stun_status[96];    // state text only, e.g. "Mapped"
    char          nat_stun_endpoint[48]; // your external IP:port (host screens only)
    char          spectator_punch_status[96];
    bool          spectators_enabled;
    uint16_t      spectator_listen_port;
    int           connected_spectators;
    bool          palette_sync_enabled;
    bool          remote_palette_preview_enabled;
    bool          debug_logging_enabled;
    char          spectator_status[128];
    char          palette_status[128];

    // Spectator client diagnostics
    bool          spectator_client_active;
    uint32_t      spectator_client_match_id;
    uint32_t      spectator_client_match_ordinal;
    char          spectator_p1_name[64];
    char          spectator_p2_name[64];
    int           spectator_p1_wins;
    int           spectator_p2_wins;
    uint32_t      spectator_client_buffered_frames;
    int32_t       spectator_client_buffer_start;
    int32_t       spectator_client_buffer_end;
    int32_t       spectator_client_confirmed_edge;
    int32_t       spectator_client_playback_frame;
    bool          spectator_client_should_fast_forward;
    bool          spectator_client_needs_hard_sync;
    bool          spectator_client_relay_active;
    uint16_t      spectator_client_relay_port;
    uint32_t      spectator_client_relay_spectators;
    char          spectator_client_status[128];
    bool          spectator_lan_discovery_active;
    uint32_t      spectator_lan_result_count;
    char          spectator_lan_discovery_status[128];
    bool          spectator_playback_active;
    int32_t       spectator_playback_frame;
    char          spectator_playback_status[128];

    // Decision prompt overlay
    bool          prompt_active;
    uint32_t      prompt_selected_index;
    uint32_t      prompt_option_count;
    char          prompt_title[96];
    char          prompt_body[160];
    char          prompt_option_labels[3][32];

    // Session display
    bool          join_spectator_probe_active;
    bool          connecting_as_host;
    bool          is_host;
    int           active_delay;
    int           rollback_budget;
    int           rollback_tolerance;
    int           gameplay_delay_mode;
    int           recommended_delay;
    int           recommended_max_rollback;
    int           stall_threshold;
    bool          stall_warning;
    int           local_frame_timing_mode;
    int           remote_frame_timing_mode;
    bool          remote_frame_timing_valid;
    bool          frame_timing_session_locked;
    int           current_rounds_to_win;
    char          current_rounds_label[32];

    // Accept state
    bool          local_accepted;
    bool          remote_accepted;

    // Text editing state
    bool          is_text_editing;
    TextEditField text_edit_field;
    char          text_edit_buffer[96];
    int           text_cursor_pos;
};

} // namespace NetMenu
