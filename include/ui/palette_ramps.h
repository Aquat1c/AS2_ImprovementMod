#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace PaletteRamps {
struct Group {
    int first, last, key;
};
struct Hsv {
    float h, s, v;
};
inline Hsv ToHsv(const uint8_t *c) {
    float r = c[2] / 255.f, g = c[1] / 255.f, b = c[0] / 255.f;
    float hi = fmaxf(r, fmaxf(g, b)), lo = fminf(r, fminf(g, b)), d = hi - lo;
    float h = 0;
    if (d > 0) {
        h = hi == r ? (g - b) / d : hi == g ? 2 + (b - r) / d : 4 + (r - g) / d;
        h /= 6;
        if (h < 0)
            h += 1;
    }
    return {h, hi > 0 ? d / hi : 0, hi};
}
inline int Build(const uint8_t *bank, Group *groups) {
    int last = 255;
    while (last > 0 && !(bank[last * 4] | bank[last * 4 + 1] | bank[last * 4 + 2] | bank[last * 4 + 3]))
        --last;
    int count = 0;
    for (int i = 1; i <= last;) {
        int first = i, direction = 0;
        Hsv anchor = ToHsv(bank + i * 4);
        while (i + 1 <= last && i - first < 31 && bank[(i + 1) * 4 + 3] == bank[first * 4 + 3]) {
            Hsv next = ToHsv(bank + (i + 1) * 4), previous = ToHsv(bank + i * 4);
            float hue = fabsf(next.h - anchor.h);
            hue = fminf(hue, 1 - hue);
            bool grey = anchor.s < .12f && next.s < .12f;
            if (!grey && (anchor.s < .12f || next.s < .12f || hue > .08f))
                break;
            int step = next.v > previous.v ? 1 : next.v < previous.v ? -1 : 0;
            if (step && direction && step != direction)
                break;
            if (step)
                direction = step;
            ++i;
        }
        groups[count++] = {first, i, (first + i) / 2};
        ++i;
    }
    return count;
}
inline void Recolor(const uint8_t *source, uint8_t *dest, Group group, uint8_t r, uint8_t g, uint8_t b) {
    if (group.first < 1 || group.last > 255 || group.first > group.last || group.key < group.first ||
        group.key > group.last)
        return;
    uint8_t target[4] = {b, g, r, 255};
    Hsv t = ToHsv(target), key = ToHsv(source + group.key * 4);
    for (int i = group.first; i <= group.last; ++i) {
        const uint8_t *c = source + i * 4;
        if (r == source[group.key * 4 + 2] && g == source[group.key * 4 + 1] && b == source[group.key * 4]) {
            memcpy(dest + i * 4, c, 4);
            continue;
        }
        Hsv v = ToHsv(c);
        float value = v.v <= key.v ? (key.v > 0 ? t.v * v.v / key.v : t.v)
                                   : t.v + (1 - t.v) * (v.v - key.v) / (1 - key.v);
        float sat = key.s > .001f ? fminf(1.f, v.s * t.s / key.s) : t.s;
        float h = t.h + (v.h - key.h);
        h -= floorf(h);
        float a = value * (1 - sat), f = h * 6 - floorf(h * 6), q = value * (1 - sat * f),
              z = value * (1 - sat * (1 - f));
        float rr, gg, bb;
        switch ((int)(h * 6) % 6) {
        case 0:
            rr = value;
            gg = z;
            bb = a;
            break;
        case 1:
            rr = q;
            gg = value;
            bb = a;
            break;
        case 2:
            rr = a;
            gg = value;
            bb = z;
            break;
        case 3:
            rr = a;
            gg = q;
            bb = value;
            break;
        case 4:
            rr = z;
            gg = a;
            bb = value;
            break;
        default:
            rr = value;
            gg = a;
            bb = q;
            break;
        }
        dest[i * 4] = (uint8_t)(bb * 255 + .5f);
        dest[i * 4 + 1] = (uint8_t)(gg * 255 + .5f);
        dest[i * 4 + 2] = (uint8_t)(rr * 255 + .5f);
        dest[i * 4 + 3] = c[3];
        if (memcmp(c, source + group.key * 4, 3) == 0)
            memcpy(dest + i * 4, target, 3);
    }
}
} // namespace PaletteRamps
