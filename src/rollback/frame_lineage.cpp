#include "rollback/frame_lineage.h"

namespace Rollback {

int32_t FrameLineage_GameAbsFromRb(int32_t frame_origin_abs, int32_t rb_frame) {
    return frame_origin_abs + rb_frame;
}

int32_t FrameLineage_GameAbsFromCheckpoint(int32_t frame_origin_abs, int32_t rb_frame) {
    if (rb_frame < 0) {
        return frame_origin_abs;
    }

    return FrameLineage_GameAbsFromRb(frame_origin_abs, rb_frame);
}

} // namespace Rollback