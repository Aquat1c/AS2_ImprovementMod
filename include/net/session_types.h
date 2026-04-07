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
    Handshaking,      // ENet connected, exchanging Hello/HelloAck
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
// Disconnect Reason
// ============================================================================

enum class DisconnectReason : uint16_t {
    Normal          = 0,
    Timeout         = 1,
    VersionMismatch = 2,
    Error           = 3,
    UserCancel      = 4,
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
    char     nickname[24];
    uint32_t build_hash;
    uint16_t protocol_version;
    uint16_t listen_port;
};

// ============================================================================
// Session Config (passed to session start)
// ============================================================================

struct SessionConfig {
    char     nickname[24];
    uint16_t listen_port;
    uint32_t target_ip;           // IPv4 in network byte order (host) or 0 (join target)
    uint16_t target_port;
    uint32_t build_hash;
    uint32_t connect_timeout_ms;  // How long to wait for connection (default 5000)
    uint32_t handshake_timeout_ms; // How long to wait for handshake (default 3000)
};

inline void SessionConfig_SetDefaults(SessionConfig* cfg) {
    if (!cfg) return;
    cfg->nickname[0] = '\0';
    cfg->listen_port = 7500;
    cfg->target_ip = 0;
    cfg->target_port = 7500;
    cfg->build_hash = 0;
    cfg->connect_timeout_ms = 5000;
    cfg->handshake_timeout_ms = 3000;
}

} // namespace Net
