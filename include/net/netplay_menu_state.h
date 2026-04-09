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
    Endpoint,
    SessionMatch,
    Diagnostics,
};

// ============================================================================
// Text Edit Field
// ============================================================================

enum class TextEditField : uint32_t {
    None = 0,
    Nickname,
    RemoteEndpoint,
    ListenPort,
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
    char          local_nickname[32];
    char          peer_nickname[32];
    float         rtt_ms;
    int           local_wins;
    int           remote_wins;

    // Connection config (editable by user)
    uint16_t      listen_port;
    char          remote_endpoint[96];
    int           preferred_delay;
    int           connection_mode;     // Net::ConnectPreference
    bool          upnp_enabled;
    bool          stun_enabled;
    bool          hole_punch_enabled;
    bool          allow_ipv6_endpoint;
    char          relay_endpoint[96];
    char          stun_endpoint[96];
    char          nat_status[128];

    // Session display
    bool          is_host;
    int           active_delay;
    int           rollback_budget;
    int           rollback_delay;       // Input pipeline delay (CCCaster-style)
    int           recommended_delay;    // Auto-computed from RTT

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
