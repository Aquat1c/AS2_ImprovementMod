/**
 * Alice Senki 2 - NAT Traversal
 *
 * UPnP IGD port mapping via miniupnpc.
 * All UPnP operations run on a background thread to avoid blocking the game.
 *
 * NAT-PMP/PCP is architecturally supported but not yet implemented.
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// NAT Traversal Status
// ============================================================================

enum class NatStatus : uint8_t {
    Idle,           // Not attempted
    Discovering,    // Background thread searching for IGD
    Mapped,         // Port mapping active
    Unavailable,    // No IGD found / UPnP disabled
    Error,          // Mapping attempt failed
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

// ============================================================================
// Public API
// ============================================================================

/// Discover the IGD and add a UDP port mapping. Non-blocking.
/// Safe to call multiple times; previous mapping is removed first.
void Nat_AddPortMapping(uint16_t internalPort);

/// Remove the current mapping (if any) and clean up.
/// Briefly blocks to ensure removal completes before socket close.
void Nat_RemovePortMapping();

/// Get the current UPnP status.
NatStatus Nat_GetStatus();

/// Human-readable status line for UI display.
const char* Nat_GetStatusText();

/// External (WAN) IP obtained from the IGD, or "" if unavailable.
const char* Nat_GetExternalIP();

} // namespace Net
