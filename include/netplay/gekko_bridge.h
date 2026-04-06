/**
 * Alice Senki 2 - GekkoNet bridge bootstrap
 *
 * Thin workspace-owned wrapper around the vendored GekkoNet dependency.
 * This is intentionally small for now: it proves the library is linked,
 * records the pinned version, and exposes status for UI/logging.
 */

#pragma once

#include <stdint.h>

namespace GekkoBridge {

struct Snapshot {
    bool initialized;
    bool linked;
    char pinned_version[48];
    char status[128];
};

bool Initialize();
void Shutdown();

bool IsAvailable();
const char* GetPinnedVersion();
const char* GetStatusText();
bool GetSnapshot(Snapshot* out);

} // namespace GekkoBridge
