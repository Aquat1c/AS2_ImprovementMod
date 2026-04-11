#pragma once

#include <stdint.h>

namespace Rollback {

int32_t FrameLineage_GameAbsFromRb(int32_t frame_origin_abs, int32_t rb_frame);
int32_t FrameLineage_GameAbsFromCheckpoint(int32_t frame_origin_abs, int32_t rb_frame);

} // namespace Rollback