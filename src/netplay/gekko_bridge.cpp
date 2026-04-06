#include "gekko_bridge.h"

#include "log_window.h"

#ifndef GEKKONET_STATIC
#define GEKKONET_STATIC
#endif
#include <gekkonet.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace GekkoBridge {
namespace {

constexpr char kPinnedVersion[] = "v20260316133147-7f1f19e";

bool s_initialized = false;
bool s_linked = false;
char s_status[128] = "GekkoNet not initialized.";

void CopyText(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    strncpy_s(dst, cap, src, _TRUNCATE);
}

void SetStatus(const char* fmt, ...) {
    if (!fmt) {
        s_status[0] = '\0';
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, args);
    va_end(args);
}

} // namespace

bool Initialize() {
    if (s_initialized) {
        return s_linked;
    }

    s_initialized = true;
    s_linked = false;
    SetStatus("Running static-link smoke test.");

    GekkoSession* session = nullptr;
    if (!gekko_create(&session, GekkoStressSession)) {
        SetStatus("gekko_create(GekkoStressSession) failed.");
        LOG_NET_ERROR("[Gekko] Failed to create smoke-test session for pinned build %s", kPinnedVersion);
        return false;
    }

    if (!gekko_destroy(&session)) {
        SetStatus("gekko_destroy() failed after smoke test.");
        LOG_NET_ERROR("[Gekko] Failed to destroy smoke-test session for pinned build %s", kPinnedVersion);
        return false;
    }

    s_linked = true;
    SetStatus("Pinned library linked and smoke-tested.");
    LOG_NET_INFO("[Gekko] Linked pinned GekkoNet build %s", kPinnedVersion);
    return true;
}

void Shutdown() {
    if (!s_initialized) {
        return;
    }

    s_initialized = false;
    s_linked = false;
    SetStatus("GekkoNet bridge shut down.");
}

bool IsAvailable() {
    return s_linked;
}

const char* GetPinnedVersion() {
    return kPinnedVersion;
}

const char* GetStatusText() {
    return s_status;
}

bool GetSnapshot(Snapshot* out) {
    if (!out) {
        return false;
    }

    out->initialized = s_initialized;
    out->linked = s_linked;
    CopyText(out->pinned_version, sizeof(out->pinned_version), kPinnedVersion);
    CopyText(out->status, sizeof(out->status), s_status);
    return true;
}

} // namespace GekkoBridge
