/**
 * Alice Senki 2 - NAT Traversal
 *
 * NAT traversal helper layer for the existing ENet/session stack.
 * Transport/gameplay ownership remains in session/network-thread systems.
 *
 * Responsibilities:
 *   - libjuice ICE/STUN/TURN candidate gathering and signaling
 *   - optional router mapping fallback (UPnP, PCP/NAT-PMP)
 *   - endpoint/path diagnostics surfaced to UI/session logs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Net {

enum class NatStatus : uint8_t {
    Idle,
    Discovering,
    Mapped,
    Unavailable,
    Error,
};

inline const char* NatStatusName(NatStatus status) {
    switch (status) {
        case NatStatus::Idle:        return "Idle";
        case NatStatus::Discovering: return "Discovering";
        case NatStatus::Mapped:      return "Mapped";
        case NatStatus::Unavailable: return "Unavailable";
        case NatStatus::Error:       return "Error";
        default:                     return "Unknown";
    }
}

enum class StunStatus : uint8_t {
    Idle,
    Probing,
    Available,
    Failed,
};

inline const char* StunStatusName(StunStatus status) {
    switch (status) {
        case StunStatus::Idle:      return "Idle";
        case StunStatus::Probing:   return "Probing";
        case StunStatus::Available: return "Available";
        case StunStatus::Failed:    return "Failed";
        default:                    return "Unknown";
    }
}

enum class NatTraversalState : uint8_t {
    Disabled,
    Idle,
    Gathering,
    MappingFallback,
    Connecting,
    Connected,
    Failed,
    TimedOut,
};

inline const char* NatTraversalStateName(NatTraversalState state) {
    switch (state) {
        case NatTraversalState::Disabled:        return "Disabled";
        case NatTraversalState::Idle:            return "Idle";
        case NatTraversalState::Gathering:       return "Gathering";
        case NatTraversalState::MappingFallback: return "MappingFallback";
        case NatTraversalState::Connecting:      return "Connecting";
        case NatTraversalState::Connected:       return "Connected";
        case NatTraversalState::Failed:          return "Failed";
        case NatTraversalState::TimedOut:        return "TimedOut";
        default:                                 return "Unknown";
    }
}

struct NatRuntimeConfig {
    bool     enable_upnp;
    bool     enable_stun;
    bool     enable_hole_punch;
    bool     enable_turn;
    bool     enable_pcp_fallback;
    bool     allow_ipv6_endpoint;
    bool     prefer_portforwarded_direct;
    char     stun_host[96];
    uint16_t stun_port;
    char     turn_host[96];
    uint16_t turn_port;
    char     turn_username[64];
    char     turn_password[64];
    uint32_t gather_timeout_ms;
    uint32_t connect_timeout_ms;
    uint32_t mapping_timeout_ms;
    uint8_t  traversal_log_verbosity;
};

inline void NatRuntimeConfig_SetDefaults(NatRuntimeConfig* cfg) {
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
    cfg->gather_timeout_ms = 5000;
    cfg->connect_timeout_ms = 8000;
    cfg->mapping_timeout_ms = 2000;
    cfg->traversal_log_verbosity = 1;
}

enum class NatSignalType : uint8_t {
    LocalDescription = 1,
    Candidate        = 2,
    GatheringDone    = 3,
};

inline const char* NatSignalTypeName(NatSignalType type) {
    switch (type) {
        case NatSignalType::LocalDescription: return "LocalDescription";
        case NatSignalType::Candidate:        return "Candidate";
        case NatSignalType::GatheringDone:    return "GatheringDone";
        default:                              return "Unknown";
    }
}

constexpr size_t NAT_SIGNAL_TEXT_MAX = 1024;

struct NatSignalMessage {
    NatSignalType type;
    char          text[NAT_SIGNAL_TEXT_MAX];
};

struct NatSnapshot {
    NatTraversalState traversal_state;
    NatStatus         upnp_status;
    NatStatus         pcp_status;
    StunStatus        stun_status;

    bool              upnp_enabled;
    bool              pcp_enabled;
    bool              stun_enabled;
    bool              hole_punch_enabled;
    bool              turn_enabled;
    bool              allow_ipv6_endpoint;
    bool              prefer_portforwarded_direct;
    bool              local_ipv6_available;

    bool              running_under_wine;
    bool              running_under_proton;

    uint16_t          mapped_port;
    uint16_t          pcp_mapped_port;
    uint16_t          stun_external_port;
    uint32_t          local_candidate_count;
    uint32_t          remote_candidate_count;

    char              status_text[192];
    char              failure_reason[128];
    char              external_ip[64];
    char              pcp_external_ip[64];
    char              stun_server[96];
    char              stun_endpoint[96];
    char              turn_server[96];
    char              local_description[1024];
    char              selected_local_candidate[256];
    char              selected_remote_candidate[256];
    char              selected_local_address[96];
    char              selected_remote_address[96];
    char              remote_hint[96];
};

void Nat_ApplyRuntimeConfig(const NatRuntimeConfig* config);
void Nat_StartServices(uint16_t internalPort);
void Nat_StopServices();
void Nat_GetSnapshot(NatSnapshot* out);

void Nat_SetRemoteHint(const char* host, uint16_t port);
void Nat_ClearRemoteHint();

bool Nat_TryPopOutboundSignal(NatSignalMessage* out);
void Nat_SubmitRemoteSignal(const NatSignalMessage* msg);

bool Nat_ShouldPreferDirect();
bool Nat_HasMappedPort();

bool Nat_IsStunBackendAvailable();
bool Nat_IsHolePunchBackendAvailable();
bool Nat_IsTurnBackendAvailable();
bool Nat_IsUpnpBackendAvailable();
bool Nat_IsPcpBackendAvailable();

// Legacy compatibility wrappers (existing call sites)
void Nat_AddPortMapping(uint16_t internalPort);
void Nat_RemovePortMapping();
NatStatus Nat_GetStatus();
const char* Nat_GetStatusText();
const char* Nat_GetExternalIP();
const char* Nat_GetStunEndpoint();

} // namespace Net
