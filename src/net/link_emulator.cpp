/**
 * Alice Senki 2 - Inbound link emulator implementation
 */

#include "net/link_emulator.h"

#include "rollback/netplay_log.h"
#include "ui/log_window.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Net {

namespace {

// One InputStream packet per frame per side plus control traffic; at 60 Hz a
// 500 ms one-way delay holds ~30-60. 256 gives headroom for a pathological
// setting without unbounded growth.
constexpr size_t kMaxQueued = 256;
// Transport2Event is the only thing submitted; kept opaque here so this unit
// does not depend on the transport headers.
constexpr size_t kMaxEventSize = 2048;

struct Held {
    uint32_t release_at_ms;
    uint64_t sequence;        // FIFO tiebreak; release order is submit order
    size_t   size;
    uint8_t  bytes[kMaxEventSize];
};

std::mutex          s_mutex;
LinkEmulatorConfig  s_cfg{};
Held                s_queue[kMaxQueued];
size_t              s_head = 0;      // oldest
size_t              s_count = 0;
uint64_t            s_sequence = 0;
uint32_t            s_lastRelease = 0;
bool                s_lastReleaseValid = false;
LinkEmulatorStats   s_stats{};
uint32_t            s_rngState = 0x9E3779B9u;
bool                s_loaded = false;

uint32_t NextRandom() {
    // xorshift32 — local to this unit; the link is asymmetric by nature, so
    // this never needs to agree across peers, only to be reproducible.
    uint32_t x = s_rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rngState = x ? x : 0x9E3779B9u;
    return s_rngState;
}

bool BuildSiblingPath(HMODULE module, char* out, size_t outSize) {
    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(module, modulePath, (DWORD)sizeof(modulePath));
    if (n == 0 || n >= sizeof(modulePath)) return false;
    char* slash = strrchr(modulePath, '\\');
    if (!slash) return false;
    *slash = '\0';
    return _snprintf_s(out, outSize, _TRUNCATE, "%s\\as2_stress.cfg", modulePath) > 0;
}

bool ParseAt(const char* path, LinkEmulatorConfig* cfg) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return false;
    bool sawKey = false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (sscanf_s(line, "link_latency_ms=%d", &v) == 1 && v >= 0) {
            cfg->one_way_latency_ms = (uint32_t)(v > 5000 ? 5000 : v);
            sawKey = true;
        }
        if (sscanf_s(line, "link_jitter_ms=%d", &v) == 1 && v >= 0) {
            cfg->jitter_ms = (uint32_t)(v > 1000 ? 1000 : v);
            sawKey = true;
        }
        if (sscanf_s(line, "link_loss_pct=%d", &v) == 1 && v >= 0) {
            cfg->loss_percent = (uint32_t)(v > 100 ? 100 : v);
            sawKey = true;
        }
    }
    fclose(f);
    return sawKey;
}

} // namespace

void LinkEmulator_LoadConfig() {
    LinkEmulatorConfig cfg{};
    char candidates[3][MAX_PATH] = {};
    int count = 0;
    if (BuildSiblingPath(nullptr, candidates[count], MAX_PATH)) ++count;
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LinkEmulator_LoadConfig),
                           &self) && self) {
        if (BuildSiblingPath(self, candidates[count], MAX_PATH)) ++count;
    }
    strncpy_s(candidates[count], MAX_PATH, "as2_stress.cfg", _TRUNCATE);
    ++count;

    for (int i = 0; i < count; ++i) {
        if (i > 0 && strcmp(candidates[i], candidates[0]) == 0) continue;
        if (ParseAt(candidates[i], &cfg)) {
            LinkEmulator_Configure(cfg);
            LOG_INFO("[LinkEmu] Armed from %s: one_way=%ums jitter=%ums loss=%u%% "
                     "(expect measured RTT ~%ums when both peers match)",
                     candidates[i], cfg.one_way_latency_ms, cfg.jitter_ms,
                     cfg.loss_percent, cfg.one_way_latency_ms * 2);
            Rollback::NetplayLog_Write("LINKEMU", -1,
                "Armed: one_way=%ums jitter=%ums loss=%u%% source=%s",
                cfg.one_way_latency_ms, cfg.jitter_ms, cfg.loss_percent,
                candidates[i]);
            s_loaded = true;
            return;
        }
    }
    s_loaded = true;
    LOG_INFO("[LinkEmu] No link_* keys in as2_stress.cfg — emulator OFF");
}

void LinkEmulator_Configure(const LinkEmulatorConfig& cfg) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_cfg = cfg;
    s_rngState = cfg.seed ? cfg.seed : 0x9E3779B9u;
    s_head = 0;
    s_count = 0;
    s_sequence = 0;
    s_lastReleaseValid = false;
    memset(&s_stats, 0, sizeof(s_stats));
}

void LinkEmulator_GetConfig(LinkEmulatorConfig* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    *out = s_cfg;
}

bool LinkEmulator_IsActive() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_cfg.one_way_latency_ms > 0 || s_cfg.jitter_ms > 0 ||
           s_cfg.loss_percent > 0;
}

bool LinkEmulator_Submit(const void* event, size_t event_size,
                         bool reliable, uint32_t now_ms) {
    if (!event || event_size == 0 || event_size > kMaxEventSize) return false;
    std::lock_guard<std::mutex> lock(s_mutex);

    // Loss: unreliable only. ENet has already acknowledged a reliable packet
    // to the sender by the time it reaches us, so dropping it here is
    // permanent — nothing upstream will retransmit it.
    if (!reliable && s_cfg.loss_percent > 0 &&
        (NextRandom() % 100u) < s_cfg.loss_percent) {
        ++s_stats.total_dropped;
        return false;
    }

    uint32_t delay = s_cfg.one_way_latency_ms;
    if (s_cfg.jitter_ms > 0) {
        delay += NextRandom() % (s_cfg.jitter_ms + 1u);
    }
    uint32_t release = now_ms + delay;

    // Monotonic release: jitter adds latency, it must never reorder. We are
    // above ENet's reliability layer and the session layer is entitled to
    // assume a reliable channel arrives in order.
    if (s_lastReleaseValid && (int32_t)(release - s_lastRelease) < 0) {
        release = s_lastRelease;
    }

    if (s_count >= kMaxQueued) {
        // Pathological configuration (huge latency, tiny frame budget).
        // Release the oldest immediately rather than growing without bound;
        // counted so a run that hit this is never mistaken for a clean one.
        ++s_stats.overflow_forced;
        s_queue[s_head].release_at_ms = now_ms;
    }

    const size_t slot = (s_head + s_count) % kMaxQueued;
    if (s_count < kMaxQueued) {
        ++s_count;
    }
    Held& h = s_queue[slot];
    h.release_at_ms = release;
    h.sequence = s_sequence++;
    h.size = event_size;
    memcpy(h.bytes, event, event_size);

    s_lastRelease = release;
    s_lastReleaseValid = true;
    ++s_stats.total_delayed;
    if (s_count > s_stats.peak_queued) s_stats.peak_queued = (uint32_t)s_count;
    s_stats.queued = (uint32_t)s_count;
    return true;
}

bool LinkEmulator_PopDue(void* out_event, size_t event_size, uint32_t now_ms) {
    if (!out_event) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_count == 0) return false;

    Held& h = s_queue[s_head];
    if ((int32_t)(now_ms - h.release_at_ms) < 0) {
        return false;   // FIFO: nothing behind it can be due either
    }
    const size_t n = h.size < event_size ? h.size : event_size;
    memcpy(out_event, h.bytes, n);
    s_head = (s_head + 1) % kMaxQueued;
    --s_count;
    ++s_stats.total_released;
    s_stats.queued = (uint32_t)s_count;
    return true;
}

void LinkEmulator_Reset() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_head = 0;
    s_count = 0;
    s_lastReleaseValid = false;
    s_stats.queued = 0;
}

void LinkEmulator_GetStats(LinkEmulatorStats* out) {
    if (!out) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    *out = s_stats;
}

} // namespace Net
