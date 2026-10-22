#include "training/motion_script.h"

namespace Training {

namespace {

bool IsButtonChar(char c) {
    return c >= 'a' && c <= 'd';
}

} // namespace

uint8_t MotionExplicitButtons(const char* motion) {
    uint8_t mask = 0;
    for (const char* p = motion; p && *p; ++p) {
        if (IsButtonChar(*p)) {
            mask |= (uint8_t)(1u << (*p - 'a'));
        }
    }
    return mask;
}

bool MotionNamesButtons(const char* motion) {
    return MotionExplicitButtons(motion) != 0;
}

int ExpandMotion(const char* motion, int chargeFrames, MotionFrame* out, int capacity) {
    if (!motion || !out || capacity <= 0) {
        return 0;
    }
    if (chargeFrames < 1) {
        chargeFrames = 1;
    }

    int count = 0;
    for (const char* p = motion; *p; ++p) {
        const char c = *p;

        if (c == '[') {
            // A malformed bracket stops the expansion: playing the rest would
            // feed a direction the table never asked for.
            if (p[1] == '\0' || p[2] != ']') {
                break;
            }
            const char held = p[1];
            for (int i = 0; i < chargeFrames && count < capacity; ++i) {
                out[count].dir = held;
                out[count].buttonMask = 0;
                ++count;
            }
            p += 2;
            continue;
        }

        if (IsButtonChar(c)) {
            // The button presses with the frame already emitted - the last
            // frame of a charge, or the direction just before it.
            if (count > 0) {
                out[count - 1].buttonMask |= (uint8_t)(1u << (c - 'a'));
            }
            continue;
        }

        if (count < capacity) {
            out[count].dir = c;
            out[count].buttonMask = 0;
            ++count;
        }
    }
    return count;
}

int MotionLength(const char* motion, int chargeFrames) {
    if (!motion) {
        return 0;
    }
    if (chargeFrames < 1) {
        chargeFrames = 1;
    }

    int count = 0;
    for (const char* p = motion; *p; ++p) {
        const char c = *p;
        if (c == '[') {
            if (p[1] == '\0' || p[2] != ']') {
                break;
            }
            count += chargeFrames;
            p += 2;
            continue;
        }
        if (IsButtonChar(c)) {
            continue;
        }
        ++count;
    }
    return count;
}

} // namespace Training
