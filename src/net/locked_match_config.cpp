/**
 * Alice Senki 2 - Locked Match Configuration
 *
 * CRC32 hash computation and comparison for LockedMatchConfig.
 */

#include "net/locked_match_config.h"
#include <string.h>

namespace Net {

// Standard CRC32 (same polynomial as zlib / Ethernet)
static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1)));
        }
    }
    return ~crc;
}

uint32_t LockedMatchConfig_Hash(const LockedMatchConfig* cfg) {
    if (!cfg) return 0;
    return crc32_update(0, reinterpret_cast<const uint8_t*>(cfg), sizeof(LockedMatchConfig));
}

bool LockedMatchConfig_Equal(const LockedMatchConfig* a, const LockedMatchConfig* b) {
    if (!a || !b) return false;
    return memcmp(a, b, sizeof(LockedMatchConfig)) == 0;
}

void LockedMatchConfig_Clear(LockedMatchConfig* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(LockedMatchConfig));
}

} // namespace Net
