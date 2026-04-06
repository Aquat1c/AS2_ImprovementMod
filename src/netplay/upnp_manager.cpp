// upnp_manager.cpp — UPnP port forwarding via miniupnpc.
// Discovery and mapping run on a background thread so they never stall the
// game loop.  The public API is lock-free for status queries; only
// AddPortMapping / RemovePortMapping touch the mutex.

#include "netplay/upnp_manager.h"
#include "log_window.h"

#if HAS_UPNP

#include <miniupnpc.h>
#include <upnpcommands.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace UpnpManager {

// ── internal state ──────────────────────────────────────────────────────────
static std::mutex          s_mtx;
static std::thread         s_worker;

// Atomics so GetStatus / GetStatusText can be polled lock-free.
static std::atomic<Status> s_status{Status::Idle};
static char                s_statusText[128]  = "UPnP: idle";
static char                s_externalIP[48]   = "";

// Current mapping params (for removal).
static UPNPUrls  s_urls;
static IGDdatas  s_igd;
static bool      s_igdValid   = false;
static uint16_t  s_mappedPort = 0;

static constexpr int  DISCOVER_TIMEOUT_MS = 2000;
static constexpr const char* LEASE_DURATION = "3600"; // 1 hour
static constexpr const char* DESCRIPTION    = "AliceSenki2";

// ── helpers ─────────────────────────────────────────────────────────────────
static void CleanupIGD() {
    if (s_igdValid) {
        FreeUPNPUrls(&s_urls);
        s_igdValid = false;
    }
    s_mappedPort = 0;
}

static void SetStatusText(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_statusText, sizeof(s_statusText), fmt, ap);
    va_end(ap);
}

// ── background worker ───────────────────────────────────────────────────────
static void WorkerThread(uint16_t port) {
    s_status.store(Status::Discovering, std::memory_order_release);
    SetStatusText("UPnP: discovering...");
    LOG_NET_INFO("[UPnP] Discovering IGD (timeout %d ms)...", DISCOVER_TIMEOUT_MS);

    int error = 0;
    UPNPDev* devList = upnpDiscover(
        DISCOVER_TIMEOUT_MS, nullptr, nullptr,
        UPNP_LOCAL_PORT_ANY, 0, 2, &error);

    if (!devList) {
        LOG_NET_INFO("[UPnP] No devices found (error %d).", error);
        s_status.store(Status::Unavailable, std::memory_order_release);
        SetStatusText("UPnP: no IGD found");
        return;
    }

    char lanAddr[64] = {};
    char wanAddr[64] = {};
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        CleanupIGD();

        int igdResult = UPNP_GetValidIGD(
            devList, &s_urls, &s_igd,
            lanAddr, sizeof(lanAddr),
            wanAddr, sizeof(wanAddr));

        freeUPNPDevlist(devList);

        if (igdResult == 0) {
            LOG_NET_INFO("[UPnP] No valid IGD found.");
            s_status.store(Status::Unavailable, std::memory_order_release);
            SetStatusText("UPnP: no IGD found");
            return;
        }
        s_igdValid = true;
        LOG_NET_INFO("[UPnP] IGD found (result %d): LAN=%s  WAN=%s",
                   igdResult, lanAddr, wanAddr);
    }

    // Fetch external IP.
    {
        char extIP[16] = {};
        int r = UPNP_GetExternalIPAddress(
            s_urls.controlURL, s_igd.first.servicetype, extIP);
        if (r == UPNPCOMMAND_SUCCESS && extIP[0]) {
            strncpy(s_externalIP, extIP, sizeof(s_externalIP) - 1);
            s_externalIP[sizeof(s_externalIP) - 1] = '\0';
            LOG_NET_INFO("[UPnP] External IP: %s", s_externalIP);
        } else {
            LOG_NET_INFO("[UPnP] Could not get external IP (error %d).", r);
        }
    }

    // Add port mapping.
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

    int r = UPNP_AddPortMapping(
        s_urls.controlURL, s_igd.first.servicetype,
        portStr,       // external port
        portStr,       // internal port
        lanAddr,       // internal client
        DESCRIPTION,   // description
        "UDP",         // protocol
        nullptr,       // remoteHost (wildcard)
        LEASE_DURATION);

    if (r == UPNPCOMMAND_SUCCESS) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_mappedPort = port;
        s_status.store(Status::Mapped, std::memory_order_release);
        SetStatusText("UPnP: mapped port %u", (unsigned)port);
        LOG_NET_INFO("[UPnP] Port %u mapped successfully.", (unsigned)port);
    } else {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_status.store(Status::Error, std::memory_order_release);
        SetStatusText("UPnP: mapping failed (%d)", r);
        LOG_NET_INFO("[UPnP] AddPortMapping failed: error %d.", r);
    }
}

// ── public API ──────────────────────────────────────────────────────────────

void AddPortMapping(uint16_t internalPort) {
    std::lock_guard<std::mutex> lk(s_mtx);

    // Join previous worker if still hanging around.
    if (s_worker.joinable()) {
        // If a mapping is currently active, remove it first.
        if (s_mappedPort && s_igdValid) {
            char portStr[8];
            snprintf(portStr, sizeof(portStr), "%u", (unsigned)s_mappedPort);
            UPNP_DeletePortMapping(
                s_urls.controlURL, s_igd.first.servicetype,
                portStr, "UDP", nullptr);
            LOG_NET_INFO("[UPnP] Removed previous mapping (port %u).", (unsigned)s_mappedPort);
            s_mappedPort = 0;
        }
        s_worker.join();
    }

    s_worker = std::thread(WorkerThread, internalPort);
}

void RemovePortMapping() {
    std::lock_guard<std::mutex> lk(s_mtx);

    if (s_worker.joinable())
        s_worker.join();

    if (s_mappedPort && s_igdValid) {
        char portStr[8];
        snprintf(portStr, sizeof(portStr), "%u", (unsigned)s_mappedPort);
        int r = UPNP_DeletePortMapping(
            s_urls.controlURL, s_igd.first.servicetype,
            portStr, "UDP", nullptr);
        LOG_NET_INFO("[UPnP] Removed port mapping %u (result %d).", (unsigned)s_mappedPort, r);
    }

    CleanupIGD();

    s_status.store(Status::Idle, std::memory_order_release);
    SetStatusText("UPnP: idle");
    s_externalIP[0] = '\0';
}

Status GetStatus() {
    return s_status.load(std::memory_order_acquire);
}

const char* GetStatusText() {
    return s_statusText;
}

const char* GetExternalIP() {
    return s_externalIP;
}

} // namespace UpnpManager

#else // !HAS_UPNP — stub when miniupnpc is not available

namespace UpnpManager {

void        AddPortMapping(uint16_t)    {}
void        RemovePortMapping()         {}
Status      GetStatus()                 { return Status::Unavailable; }
const char* GetStatusText()             { return "UPnP: not compiled"; }
const char* GetExternalIP()             { return ""; }

} // namespace UpnpManager

#endif // HAS_UPNP
