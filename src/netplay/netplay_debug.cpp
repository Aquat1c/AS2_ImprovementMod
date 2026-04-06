#include "netplay_hooks.h"

#include "as2_rollback.h"
#include "log_window.h"

namespace NetplayHooks {

bool NetplayDebugSaveState(int slot) {
    LOG_DEBUG("[NetplayHooks] NetplayDebugSaveState(slot=%d)", slot);
    return AS2_SaveState(8 + slot);
}

bool NetplayDebugLoadState(int slot) {
    LOG_DEBUG("[NetplayHooks] NetplayDebugLoadState(slot=%d)", slot);
    return AS2_LoadState(8 + slot);
}

bool NetplayDebugSlotValid(int slot) {
    StatePreview_t preview;
    return AS2_GetStatePreview(8 + slot, &preview);
}

uint32_t NetplayDebugSlotFrame(int slot) {
    StatePreview_t preview;
    if (AS2_GetStatePreview(8 + slot, &preview)) {
        return preview.frame_number;
    }
    return 0;
}

int NetplayDebugSlotCount() {
    return 8;
}

void NetplayDebugClearSlots() {
    // Savestates are stored in AS2_SaveState backing storage.
}

} // namespace NetplayHooks
