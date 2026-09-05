#pragma once

/**
 * Alice Senki 2 - practice motion strings (pure logic).
 *
 * A scripted action is written as a numpad string and played one frame per
 * character, which keeps a new command a table entry rather than another case
 * in the input builder. The syntax carries three things:
 *
 *   digits      one frame each, the numpad direction ('5' is neutral)
 *   [x]         hold direction x for `chargeFrames` frames
 *   a b c d     press that button together with the frame just emitted
 *
 * A string that names no button keeps the original rule - the row's configured
 * button lands on the last frame, which is what makes "236" a move rather than
 * a walk. A string that names its own buttons is a whole route: "21[4]d6" is
 * 21[4]D cancelled into 6X, and the row's button lands on the 6.
 *
 * Nothing here touches game memory, so the parser is unit tested rather than
 * inferred from behaviour.
 */

#include <stddef.h>
#include <stdint.h>

namespace Training {

struct MotionFrame {
    char dir = '5';          // numpad direction for this frame
    uint8_t buttonMask = 0;  // bit per button index: 1<<0 = A .. 1<<3 = D
};

/// Buttons named inside the string, as a bit per button index.
uint8_t MotionExplicitButtons(const char* motion);

/// True when the string names at least one button of its own.
bool MotionNamesButtons(const char* motion);

/// Expands `motion` into the frames it plays. Returns the frame count, which is
/// never more than `capacity`. A malformed bracket ends the expansion rather
/// than emitting a frame nobody wrote.
int ExpandMotion(const char* motion, int chargeFrames, MotionFrame* out, int capacity);

/// Frame count alone, without needing a buffer.
int MotionLength(const char* motion, int chargeFrames);

} // namespace Training
