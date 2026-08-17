/**
 * Alice Senki 2 - Session Types
 *
 * Shared type definitions for the session layer:
 * session states, peer info, connection stats, session metadata.
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Session State Machine
// ============================================================================

enum class SessionState : uint8_t {
    Idle,             // No session active
    Connecting,       // ENet connect in progress
    Handshaking,      // ENet connected, running the 5-step nonce handshake
    Connected,        // Handshake complete, session established
    Ready,            // Both peers signaled ready
    Disconnecting,    // Graceful disconnect in progress
    Failed,           // Terminal error state
};

inline const char* SessionStateName(SessionState state) {
    switch (state) {
        case SessionState::Idle:          return "Idle";
        case SessionState::Connecting:    return "Connecting";
        case SessionState::Handshaking:   return "Handshaking";
        case SessionState::Connected:     return "Connected";
        case SessionState::Ready:         return "Ready";
        case SessionState::Disconnecting: return "Disconnecting";
        case SessionState::Failed:        return "Failed";
        default:                          return "Unknown";
    }
}

// ============================================================================
// Session Role
// ============================================================================

enum class SessionRole : uint8_t {
    None,
    Host,
    Join,
};

inline const char* SessionRoleName(SessionRole role) {
    switch (role) {
        case SessionRole::None: return "None";
        case SessionRole::Host: return "Host";
        case SessionRole::Join: return "Join";
        default:                return "Unknown";
    }
}

// ============================================================================
// Connection Preference
// ============================================================================

enum class ConnectPreference : uint8_t {
    AutoDirectThenRelay = 0,  // Direct endpoint with NAT punch assist
    DirectOnly          = 1,  // Never fallback to relay
    RelayOnly           = 2,  // Reserved for a future traffic relay backend
};

inline const char* ConnectPreferenceName(ConnectPreference pref) {
    switch (pref) {
        case ConnectPreference::AutoDirectThenRelay: return "AutoDirectThenRelay";
        case ConnectPreference::DirectOnly:          return "DirectOnly";
        case ConnectPreference::RelayOnly:           return "RelayOnly";
        default:                                     return "Unknown";
    }
}

// ============================================================================
// Disconnect Reason
// ============================================================================

enum class DisconnectReason : uint16_t {
    Normal          = 0,
    Timeout         = 1,
    VersionMismatch = 2,
    Error           = 3,
    UserCancel      = 4,
    Busy            = 5,   // v2 (edge C-5/C-6): host already paired with a peer
};

// ============================================================================
// Connection Statistics
// ============================================================================

struct ConnectionStats {
    float    rtt_ms;              // Round-trip time in milliseconds
    float    rtt_variance_ms;     // RTT variance
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_lost;
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

// ============================================================================
// Peer Info (received via handshake)
// ============================================================================

struct PeerInfo {
    bool     valid;
    char     nickname[64];
    uint32_t build_hash;
    uint16_t protocol_version;
    uint16_t listen_port;
    bool     round_count_valid;
    uint8_t  round_count;
    bool     frame_timing_valid;
    uint8_t  frame_timing_mode;
    bool     hud_style_valid;
    uint8_t  hud_trail_r;
    uint8_t  hud_trail_g;
    uint8_t  hud_trail_b;
    uint8_t  hud_text_r;
    uint8_t  hud_text_g;
    uint8_t  hud_text_b;
    uint8_t  hud_trail_length;
    uint8_t  hud_score_r;
    uint8_t  hud_score_g;
    uint8_t  hud_score_b;
    uint8_t  hud_font_size;
    uint8_t  hud_vertical_position;
};

// ============================================================================
// NAT / Relay Settings
// ============================================================================

struct NatTraversalConfig {
    bool     enable_upnp;                   // Host-side UPnP mapping
    bool     enable_stun;                   // Run STUN probe for external endpoint
    bool     enable_hole_punch;             // Send UDP punch bursts before connect
    bool     enable_turn;                   // Enable TURN candidate gathering via libjuice
    bool     enable_pcp_fallback;           // Try PCP/NAT-PMP mapping fallback
    bool     allow_ipv6_endpoint;           // Accept IPv6 endpoint text in UI/parser
    bool     prefer_portforwarded_direct;   // Prefer direct path when mapping is available
    char     stun_host[96];                 // STUN server hostname/IP
    uint16_t stun_port;                     // STUN server UDP port
    char     turn_host[96];                 // TURN server hostname/IP
    uint16_t turn_port;                     // TURN server UDP port
    char     turn_username[64];             // TURN username
    char     turn_password[64];             // TURN password
    char     relay_host[96];                // Autopunch relay endpoint override
    uint16_t relay_port;                    // Autopunch relay endpoint port
    uint32_t gather_timeout_ms;             // Candidate gather timeout
    uint32_t connect_timeout_ms;            // ICE connect timeout after signaling
    uint32_t mapping_timeout_ms;            // UPnP/PCP mapping timeout
    uint8_t  traversal_log_verbosity;       // 0=errors,1=info,2=debug,3=verbose
};

inline void NatTraversalConfig_SetDefaults(NatTraversalConfig* cfg) {
    if (!cfg) return;
    cfg->enable_upnp = true;
    cfg->enable_stun = true;
    cfg->enable_hole_punch = true;
    cfg->enable_turn = false;
    cfg->enable_pcp_fallback = true;
    cfg->allow_ipv6_endpoint = true;
    cfg->prefer_portforwarded_direct = true;
    cfg->stun_host[0] = '\0';
    cfg->stun_port = 19302;
    cfg->turn_host[0] = '\0';
    cfg->turn_port = 3478;
    cfg->turn_username[0] = '\0';
    cfg->turn_password[0] = '\0';
    cfg->relay_host[0] = '\0';
    cfg->relay_port = 0;
    cfg->gather_timeout_ms = 5000;
    cfg->connect_timeout_ms = 8000;
    cfg->mapping_timeout_ms = 2000;
    cfg->traversal_log_verbosity = 1;
}

// ============================================================================
// Session Config (passed to session start)
// ============================================================================

struct SessionConfig {
    char     nickname[64];
    uint16_t listen_port;
    char     target_host[96];     // Join target host/IP
    uint16_t target_port;
    uint32_t build_hash;          // Exact local mod build fingerprint
    uint32_t connect_timeout_ms;  // How long to wait for connection (default 5000)
    uint32_t handshake_timeout_ms; // How long to wait for handshake (default 3000)
    ConnectPreference connect_preference;
    NatTraversalConfig nat;
};

inline void SessionConfig_SetDefaults(SessionConfig* cfg) {
    if (!cfg) return;
    cfg->nickname[0] = '\0';
    cfg->listen_port = 7500;
    cfg->target_host[0] = '\0';
    cfg->target_port = 7500;
    cfg->build_hash = 0;
    cfg->connect_timeout_ms = 5000;
    cfg->handshake_timeout_ms = 3000;
    cfg->connect_preference = ConnectPreference::AutoDirectThenRelay;
    NatTraversalConfig_SetDefaults(&cfg->nat);
}

} // namespace Net
