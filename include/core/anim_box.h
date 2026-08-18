#pragma once

/**
 * Alice Senki 2 - shared animation-box decode and overlap test.
 *
 * One decoder for the hitbox viewer and the practice auto-block contact
 * scanner, so the two can never drift. The arithmetic mirrors the native
 * collision routines exactly, including the truncating integer divides:
 *
 *   Entity_UpdateDamageApplication  attacker hitbox@8  vs defender ext-hurt@72
 *   Entity_UpdateGrabAlignment      attacker hitbox@8  vs defender hurt@40
 *   Entity_UpdateSummonHitDetection HitDef   hitbox@8  vs defender hurt@40
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace As2 {

struct AnimBox {
    int16_t xOff = 0;
    int16_t yOff = 0;
    int16_t halfW = 0;
    int16_t halfH = 0;
};

// Where a box set is anchored in the world. facing is the raw entity byte.
struct BoxOrigin {
    int16_t worldX = 0;
    int16_t worldY = 0;
    int8_t facing = 1;
};

inline AnimBox DecodeAnimBox(const uint8_t* frameBytes, size_t offset) {
    AnimBox box{};
    if (!frameBytes) {
        return box;
    }
    memcpy(&box.xOff, frameBytes + offset + 0, sizeof(int16_t));
    memcpy(&box.yOff, frameBytes + offset + 2, sizeof(int16_t));
    memcpy(&box.halfW, frameBytes + offset + 4, sizeof(int16_t));
    memcpy(&box.halfH, frameBytes + offset + 6, sizeof(int16_t));
    return box;
}

// The engine's own validity test: both half extents strictly positive.
inline bool AnimBoxValid(const AnimBox& box) {
    return box.halfW > 0 && box.halfH > 0;
}

inline int AnimBoxCenterX(const BoxOrigin& origin, const AnimBox& box) {
    return static_cast<int>(origin.worldX) / 10 +
           2 * static_cast<int>(box.xOff) * static_cast<int>(origin.facing);
}

inline int AnimBoxCenterY(const BoxOrigin& origin, const AnimBox& box) {
    return static_cast<int>(origin.worldY) / 10 + 2 * static_cast<int>(box.yOff);
}

inline bool AnimBoxesOverlap(const BoxOrigin& aOrigin, const AnimBox& a,
                             const BoxOrigin& bOrigin, const AnimBox& b) {
    int dx = AnimBoxCenterX(aOrigin, a) - AnimBoxCenterX(bOrigin, b);
    int dy = AnimBoxCenterY(aOrigin, a) - AnimBoxCenterY(bOrigin, b);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx / 2) <= (static_cast<int>(a.halfW) + static_cast<int>(b.halfW)) &&
           (dy / 2) <= (static_cast<int>(a.halfH) + static_cast<int>(b.halfH));
}

} // namespace As2
