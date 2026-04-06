#pragma once
// upnp_manager.h — Automatic UPnP port forwarding via miniupnpc.
// All operations run on a background thread so they never block the game.

#include <cstdint>

namespace UpnpManager {

enum class Status {
    Idle,          // Not attempted
    Discovering,   // Background thread searching for IGD
    Mapped,        // Port mapping active
    Unavailable,   // No IGD found or UPnP disabled on router
    Error          // Mapping attempt failed
};

// Discover the IGD and add a UDP port mapping.  Non-blocking — work runs on
// a background thread.  Safe to call multiple times; previous mapping is
// removed first.
void AddPortMapping(uint16_t internalPort);

// Remove the current mapping (if any) and clean up.  Blocks briefly to
// ensure the mapping is deleted before the socket closes.
void RemovePortMapping();

// Current status.
Status  GetStatus();

// Human-readable status line for the UI (e.g. "UPnP: mapped 12345").
const char* GetStatusText();

// External (WAN) IP obtained from the IGD, or "" if unavailable.
const char* GetExternalIP();

} // namespace UpnpManager
