/**
 * Alice Senki 2 - NAT Traversal (Implementation)
 *
 * UPnP IGD port mapping via miniupnpc.
 * Discovery and mapping run on a background thread.
 */

#include <winsock2.h>     // Must be before windows.h
#include <windows.h>

#include "net/nat_traversal.h"
#include "log_window.h"

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <stdio.h>
#include <string.h>

namespace Net {

// ============================================================================
// Internal state
// ============================================================================

static NatStatus   s_status    = NatStatus::Idle;
static char        s_statusText[128] = "UPnP: idle";
static char        s_externalIP[64]  = "";
static uint16_t    s_mappedPort      = 0;
static HANDLE      s_thread          = nullptr;
static volatile bool s_threadRunning = false;

// UPnP state (owned by background thread, read-only after thread completes)
static struct UPNPUrls s_urls;
static struct IGDdatas s_igdData;
static char  s_lanAddr[64]  = "";
static char  s_wanAddr[64]  = "";
static bool  s_igdValid     = false;

// ============================================================================
// Background thread
// ============================================================================

static DWORD WINAPI NatWorkerThread(LPVOID param) {
    uint16_t port = (uint16_t)(uintptr_t)param;

    LOG_INFO("[NAT] Discovery starting (port %u)...", port);

    // Discover UPnP devices (2 second timeout)
    int error = 0;
    struct UPNPDev* devList = upnpDiscover(2000, nullptr, nullptr,
                                           UPNP_LOCAL_PORT_ANY, 0, 2, &error);
    if (!devList) {
        LOG_WARN("[NAT] No UPnP devices found (error=%d)", error);
        snprintf(s_statusText, sizeof(s_statusText), "UPnP: no devices found");
        s_status = NatStatus::Unavailable;
        s_threadRunning = false;
        return 0;
    }

    // Find a valid IGD
    int igdResult = UPNP_GetValidIGD(devList, &s_urls, &s_igdData,
                                      s_lanAddr, sizeof(s_lanAddr),
                                      s_wanAddr, sizeof(s_wanAddr));
    freeUPNPDevlist(devList);

    if (igdResult == 0) {
        LOG_WARN("[NAT] No valid IGD found");
        snprintf(s_statusText, sizeof(s_statusText), "UPnP: no IGD");
        s_status = NatStatus::Unavailable;
        s_threadRunning = false;
        return 0;
    }

    s_igdValid = true;
    LOG_INFO("[NAT] IGD found (result=%d, lan=%s, wan=%s)", igdResult, s_lanAddr, s_wanAddr);

    // Get external IP
    char extIP[40] = "";
    int ipResult = UPNP_GetExternalIPAddress(s_urls.controlURL,
                                              s_igdData.first.servicetype,
                                              extIP);
    if (ipResult == UPNPCOMMAND_SUCCESS && extIP[0]) {
        strncpy(s_externalIP, extIP, sizeof(s_externalIP) - 1);
        LOG_INFO("[NAT] External IP: %s", s_externalIP);
    }

    // Add port mapping
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", port);

    int mapResult = UPNP_AddPortMapping(
        s_urls.controlURL,
        s_igdData.first.servicetype,
        portStr,            // external port
        portStr,            // internal port
        s_lanAddr,          // internal client
        "AS2 Rollback Mod", // description
        "UDP",              // protocol
        nullptr,            // remote host (wildcard)
        "0"                 // lease duration (0 = permanent until removed)
    );

    if (mapResult == UPNPCOMMAND_SUCCESS) {
        s_mappedPort = port;
        s_status = NatStatus::Mapped;
        snprintf(s_statusText, sizeof(s_statusText), "UPnP: mapped port %u (ext: %s)",
                 port, s_externalIP[0] ? s_externalIP : "?");
        LOG_INFO("[NAT] Port %u mapped successfully", port);
    } else {
        s_status = NatStatus::Error;
        snprintf(s_statusText, sizeof(s_statusText), "UPnP: mapping failed (%d)", mapResult);
        LOG_ERROR("[NAT] UPNP_AddPortMapping failed: %d (%s)", mapResult, strupnperror(mapResult));
    }

    s_threadRunning = false;
    return 0;
}

// ============================================================================
// Internal cleanup
// ============================================================================

static void RemoveMapping() {
    if (!s_igdValid || s_mappedPort == 0) return;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", s_mappedPort);

    int result = UPNP_DeletePortMapping(
        s_urls.controlURL,
        s_igdData.first.servicetype,
        portStr,
        "UDP",
        nullptr
    );

    if (result == UPNPCOMMAND_SUCCESS) {
        LOG_INFO("[NAT] Port mapping %u removed", s_mappedPort);
    } else {
        LOG_WARN("[NAT] Failed to remove port mapping %u: %d", s_mappedPort, result);
    }

    s_mappedPort = 0;
}

static void Cleanup() {
    if (s_igdValid) {
        FreeUPNPUrls(&s_urls);
        s_igdValid = false;
    }
    s_lanAddr[0] = '\0';
    s_wanAddr[0] = '\0';
    s_externalIP[0] = '\0';
}

// ============================================================================
// Public API
// ============================================================================

void Nat_AddPortMapping(uint16_t internalPort) {
    // If a thread is already running, wait for it first
    if (s_thread && s_threadRunning) {
        LOG_WARN("[NAT] Previous operation still running, waiting...");
        WaitForSingleObject(s_thread, 5000);
        CloseHandle(s_thread);
        s_thread = nullptr;
    }

    // Remove previous mapping if any
    if (s_mappedPort > 0) {
        RemoveMapping();
    }
    Cleanup();

    s_status = NatStatus::Discovering;
    snprintf(s_statusText, sizeof(s_statusText), "UPnP: discovering...");
    s_threadRunning = true;

    s_thread = CreateThread(nullptr, 0, NatWorkerThread,
                            (LPVOID)(uintptr_t)internalPort, 0, nullptr);
    if (!s_thread) {
        s_status = NatStatus::Error;
        snprintf(s_statusText, sizeof(s_statusText), "UPnP: thread creation failed");
        s_threadRunning = false;
        LOG_ERROR("[NAT] Failed to create worker thread");
    }
}

void Nat_RemovePortMapping() {
    // Wait for background thread if still running
    if (s_thread && s_threadRunning) {
        WaitForSingleObject(s_thread, 5000);
    }
    if (s_thread) {
        CloseHandle(s_thread);
        s_thread = nullptr;
    }

    RemoveMapping();
    Cleanup();

    s_status = NatStatus::Idle;
    snprintf(s_statusText, sizeof(s_statusText), "UPnP: idle");
}

NatStatus Nat_GetStatus() {
    return s_status;
}

const char* Nat_GetStatusText() {
    return s_statusText;
}

const char* Nat_GetExternalIP() {
    return s_externalIP;
}

} // namespace Net
