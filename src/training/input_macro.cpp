#include "training/input_macro.h"
#include "training/practice_tools.h"
#include "core/game_state.h"
#include "patches/memory_utils.h"
#include "input/input_system.h"
#include "as2_constants.h"
#include "log_window.h"
#include "rollback/rollback_session.h"
#include "imgui.h"

#include <string.h>
#include <stdio.h>

// ============================================================================
// Data Model
// ============================================================================

namespace {

struct MacroFrame {
    uint16_t input;     // Full 16-bit input mask
    int8_t   facing;    // +1 = right, -1 = left, 0 = unknown
};

struct MacroSlot {
    MacroFrame frames[MACRO_MAX_FRAMES];
    int  frameCount = 0;
    bool hasData    = false;
};

// ============================================================================
// State
// ============================================================================

bool       s_initialized = false;
MacroState s_state       = MACRO_IDLE;
int        s_currentSlot = 0;
MacroSlot  s_slots[MACRO_MAX_SLOTS] = {};
bool       s_debugLogging = false;

// Recording state
bool       s_preRecordSwapped = false;   // Whether WE initiated the control swap

// Playback state
int        s_playFrame = 0;

// ============================================================================
// Helpers
// ============================================================================

int8_t ReadP2Facing(void) {
    uint8_t raw = ReadMemory<uint8_t>(ADDR_P2_ENTITY_BASE + ENTITY_OFF_FACING);
    // Convention: 0 = facing right, 1 = facing left
    return (raw == 0) ? 1 : -1;
}

uint16_t FlipHorizontal(uint16_t input) {
    bool left  = (input & INPUT_LEFT)  != 0;
    bool right = (input & INPUT_RIGHT) != 0;
    uint16_t out = input & ~(INPUT_LEFT | INPUT_RIGHT);
    if (left)  out |= INPUT_RIGHT;
    if (right) out |= INPUT_LEFT;
    return out;
}

// Strip system buttons (START, SELECT, L/R) — only record gameplay inputs
uint16_t StripSystemButtons(uint16_t input) {
    return input & (INPUT_ANY_DIR | INPUT_ANY_BTN);
}

uint16_t ReadBufferedPlayerInput(int player) {
    static const uint16_t kButtonMasks[10] = {
        INPUT_UP,
        INPUT_DOWN,
        INPUT_LEFT,
        INPUT_RIGHT,
        INPUT_A,
        INPUT_B,
        INPUT_C,
        INPUT_D,
        INPUT_START,
        INPUT_SELECT,
    };

    const uintptr_t base = (player == 0) ? ADDR_P1_INPUT_BUFFER : ADDR_P2_INPUT_BUFFER;
    uint16_t result = 0;
    for (int i = 0; i < 10; ++i) {
        if (ReadMemory<uint16_t>(base + (i * 2)) != 0) {
            result |= kButtonMasks[i];
        }
    }
    return result;
}

void ClearSlot(int slot) {
    if (slot < 0 || slot >= MACRO_MAX_SLOTS) return;
    s_slots[slot].frameCount = 0;
    s_slots[slot].hasData = false;
}

void StopRecording(void) {
    MacroSlot& slot = s_slots[s_currentSlot];
    if (slot.frameCount > 0) {
        slot.hasData = true;
        LOG_INFO("[Macro] Recorded %d frames in slot %d", slot.frameCount, s_currentSlot + 1);
    }

    // Restore control if we swapped it
    if (s_preRecordSwapped) {
        PracticeTools_ApplyControlSwapState(false);
        s_preRecordSwapped = false;
    }

    s_state = MACRO_IDLE;
}

void StopPlayback(void) {
    // Inject one neutral frame to clear residual motion input
    InputSystem_SetOverride(1, 0);

    // Restore P2 to CPU control
    WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 1);
    WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);

    InputSystem_ClearOverride(1);
    LOG_INFO("[Macro] Playback stopped (frame %d, restored CPU flags p1=0 p2=1)", s_playFrame);
    s_playFrame = 0;
    s_state = MACRO_IDLE;
}

bool IsPracticeModeGated(void) {
    return PracticeTools_IsPracticeModeActive() &&
           !Rollback::RollbackSession_IsActive();
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void InputMacro_Init(void) {
    s_initialized = true;
    s_state = MACRO_IDLE;
    s_currentSlot = 0;
    s_playFrame = 0;
    s_preRecordSwapped = false;
    for (int i = 0; i < MACRO_MAX_SLOTS; i++) {
        ClearSlot(i);
    }
    LOG_INFO("[Macro] Input macro system initialized (%d slots, max %d frames)",
             MACRO_MAX_SLOTS, MACRO_MAX_FRAMES);
}

void InputMacro_Shutdown(void) {
    if (s_state == MACRO_RECORDING) StopRecording();
    if (s_state == MACRO_REPLAYING) StopPlayback();
    s_initialized = false;
}

// ============================================================================
// Per-frame tick
// ============================================================================

void InputMacro_Tick(void) {
    if (!s_initialized) return;
    if (!IsPracticeModeGated()) return;

    // Don't tick during pause (unless stepping)
    if (PracticeTools_IsPaused()) return;

    switch (s_state) {
    case MACRO_RECORDING: {
        MacroSlot& slot = s_slots[s_currentSlot];
        if (slot.frameCount >= MACRO_MAX_FRAMES) {
            LOG_INFO("[Macro] Max duration reached, stopping recording");
            StopRecording();
            return;
        }

        // Record the effective P2 gameplay input from the game buffer.
        // Control swap only affects buffer writes, not InputSystem_GetInput().
        uint16_t rawP1Input = InputSystem_GetInput(0);
        uint16_t rawP2Input = InputSystem_GetInput(1);
        uint16_t gameInput = StripSystemButtons(ReadBufferedPlayerInput(1));
        int8_t facing = ReadP2Facing();

        if (s_debugLogging && (slot.frameCount % 60 == 0 || gameInput != 0)) {
            LOG_INFO("[Macro] REC f=%d raw_p1=0x%04X raw_p2=0x%04X eff_p2=0x%04X facing=%d swap=%d",
                     slot.frameCount, rawP1Input, rawP2Input, gameInput, facing,
                     InputSystem_GetControlSwap() ? 1 : 0);
        }

        slot.frames[slot.frameCount].input = gameInput;
        slot.frames[slot.frameCount].facing = facing;
        slot.frameCount++;
        break;
    }

    case MACRO_REPLAYING: {
        const MacroSlot& slot = s_slots[s_currentSlot];
        if (!slot.hasData || s_playFrame >= slot.frameCount) {
            StopPlayback();
            return;
        }

        const MacroFrame& frame = slot.frames[s_playFrame];
        uint16_t input = frame.input;

        // Facing-aware flip: if current facing differs from recorded, swap L/R
        int8_t currentFacing = ReadP2Facing();
        if (frame.facing != 0 && currentFacing != 0 && frame.facing != currentFacing) {
            input = FlipHorizontal(input);
        }

        // CPU flags are set pre-frame by PracticeTools_FrameUpdate() so the
        // game's AI subsystem sees P2 as human during simulation.
        InputSystem_SetOverride(1, input);

        if (s_debugLogging && (s_playFrame % 60 == 0 || input != 0)) {
            uint8_t p1cpu = ReadMemory<uint8_t>(ADDR_P1_CPU_FLAG);
            uint8_t p2cpu = ReadMemory<uint8_t>(ADDR_P2_CPU_FLAG);
            LOG_INFO("[Macro] PLAY f=%d/%d input=0x%04X rec_facing=%d cur_facing=%d p1cpu=%u p2cpu=%u swap=%d",
                     s_playFrame, slot.frameCount, input,
                     frame.facing, currentFacing,
                     p1cpu, p2cpu,
                     InputSystem_GetControlSwap() ? 1 : 0);
        }

        s_playFrame++;
        break;
    }

    case MACRO_PRE_RECORD:
    case MACRO_IDLE:
    default:
        break;
    }
}

// ============================================================================
// State transitions
// ============================================================================

void InputMacro_ToggleRecord(void) {
    if (!s_initialized || !IsPracticeModeGated()) return;

    switch (s_state) {
    case MACRO_IDLE: {
        // Enter pre-record: swap controls to P2 if not already
        bool alreadySwapped = InputSystem_GetControlSwap();
        if (!alreadySwapped) {
            PracticeTools_ApplyControlSwapState(true);
            s_preRecordSwapped = true;
        } else {
            s_preRecordSwapped = false;
        }
        s_state = MACRO_PRE_RECORD;
        LOG_INFO("[Macro] Pre-record (slot %d) — press again to start recording", s_currentSlot + 1);
        break;
    }

    case MACRO_PRE_RECORD: {
        // Start recording
        ClearSlot(s_currentSlot);
        s_state = MACRO_RECORDING;
        LOG_INFO("[Macro] Recording started (slot %d)", s_currentSlot + 1);
        break;
    }

    case MACRO_RECORDING: {
        // Stop recording
        StopRecording();
        break;
    }

    case MACRO_REPLAYING: {
        // Stop playback, then enter pre-record
        StopPlayback();
        bool alreadySwapped = InputSystem_GetControlSwap();
        if (!alreadySwapped) {
            PracticeTools_ApplyControlSwapState(true);
            s_preRecordSwapped = true;
        } else {
            s_preRecordSwapped = false;
        }
        s_state = MACRO_PRE_RECORD;
        break;
    }
    }
}

void InputMacro_TogglePlay(void) {
    if (!s_initialized || !IsPracticeModeGated()) return;

    switch (s_state) {
    case MACRO_IDLE: {
        const MacroSlot& slot = s_slots[s_currentSlot];
        if (!slot.hasData || slot.frameCount == 0) {
            LOG_WARN("[Macro] Slot %d is empty, nothing to play", s_currentSlot + 1);
            return;
        }

        // Restore controls to normal (P1 human) if swapped
        if (InputSystem_GetControlSwap()) {
            PracticeTools_ApplyControlSwapState(false);
        }

        // Set P2 to human-controlled so our override takes effect
        WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);
        WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);

        s_playFrame = 0;
        s_state = MACRO_REPLAYING;
        LOG_INFO("[Macro] Playback started (slot %d, %d frames)", s_currentSlot + 1, slot.frameCount);
        break;
    }

    case MACRO_REPLAYING: {
        StopPlayback();
        break;
    }

    case MACRO_PRE_RECORD:
    case MACRO_RECORDING: {
        // Cancel recording, then play
        if (s_state == MACRO_RECORDING) {
            StopRecording();
        } else {
            // Cancel pre-record
            if (s_preRecordSwapped) {
                PracticeTools_ApplyControlSwapState(false);
                s_preRecordSwapped = false;
            }
            s_state = MACRO_IDLE;
        }

        // Now try to play
        const MacroSlot& slot = s_slots[s_currentSlot];
        if (slot.hasData && slot.frameCount > 0) {
            if (InputSystem_GetControlSwap()) {
                PracticeTools_ApplyControlSwapState(false);
            }
            WriteMemory<uint8_t>(ADDR_P2_CPU_FLAG, 0);
            WriteMemory<uint8_t>(ADDR_P1_CPU_FLAG, 0);
            s_playFrame = 0;
            s_state = MACRO_REPLAYING;
            LOG_INFO("[Macro] Playback started (slot %d, %d frames)", s_currentSlot + 1, slot.frameCount);
        }
        break;
    }
    }
}

void InputMacro_NextSlot(void) {
    if (!s_initialized) return;

    // Don't change slot while recording or playing
    if (s_state == MACRO_RECORDING || s_state == MACRO_REPLAYING) return;

    // Cancel pre-record if active
    if (s_state == MACRO_PRE_RECORD) {
        if (s_preRecordSwapped) {
            PracticeTools_ApplyControlSwapState(false);
            s_preRecordSwapped = false;
        }
        s_state = MACRO_IDLE;
    }

    s_currentSlot = (s_currentSlot + 1) % MACRO_MAX_SLOTS;
    LOG_INFO("[Macro] Selected slot %d", s_currentSlot + 1);
}

void InputMacro_Stop(void) {
    if (!s_initialized) return;

    switch (s_state) {
    case MACRO_RECORDING:
        StopRecording();
        break;
    case MACRO_REPLAYING:
        StopPlayback();
        break;
    case MACRO_PRE_RECORD:
        if (s_preRecordSwapped) {
            PracticeTools_ApplyControlSwapState(false);
            s_preRecordSwapped = false;
        }
        s_state = MACRO_IDLE;
        break;
    default:
        break;
    }
}

// ============================================================================
// Query
// ============================================================================

MacroState InputMacro_GetState(void) {
    return s_state;
}

int InputMacro_GetCurrentSlot(void) {
    return s_currentSlot;
}

int InputMacro_GetSlotFrameCount(int slot) {
    if (slot < 0 || slot >= MACRO_MAX_SLOTS) return 0;
    return s_slots[slot].frameCount;
}

bool InputMacro_SlotHasData(int slot) {
    if (slot < 0 || slot >= MACRO_MAX_SLOTS) return false;
    return s_slots[slot].hasData;
}

int InputMacro_GetPlaybackFrame(void) {
    return s_playFrame;
}

// ============================================================================
// Savestate integration
// ============================================================================

void InputMacro_OnSavestateLoad(void) {
    if (!s_initialized) return;

    // Cancel any active operation — slot data survives, state machine resets
    if (s_state == MACRO_RECORDING) {
        // Discard partial recording
        s_slots[s_currentSlot].frameCount = 0;
        s_slots[s_currentSlot].hasData = false;
        if (s_preRecordSwapped) {
            PracticeTools_ApplyControlSwapState(false);
            s_preRecordSwapped = false;
        }
    } else if (s_state == MACRO_REPLAYING) {
        InputSystem_ClearOverride(1);
    } else if (s_state == MACRO_PRE_RECORD) {
        if (s_preRecordSwapped) {
            PracticeTools_ApplyControlSwapState(false);
            s_preRecordSwapped = false;
        }
    }

    s_state = MACRO_IDLE;
    s_playFrame = 0;
}

// ============================================================================
// HUD Overlay
// ============================================================================

void InputMacro_RenderOverlay(void) {
    if (!s_initialized || s_state == MACRO_IDLE) return;
    if (!PracticeTools_IsPracticeModeActive()) return;

    ImDrawList* dl = PracticeTools_ShouldRenderHudBehindMenu()
        ? ImGui::GetBackgroundDrawList()
        : ImGui::GetForegroundDrawList();
    if (!dl) return;

    char buf[64];
    ImU32 color = IM_COL32(255, 255, 255, 255);

    switch (s_state) {
    case MACRO_PRE_RECORD:
        snprintf(buf, sizeof(buf), "MACRO PRE-REC [%d]", s_currentSlot + 1);
        color = IM_COL32(255, 255, 100, 255);
        break;
    case MACRO_RECORDING:
        snprintf(buf, sizeof(buf), "MACRO REC [%d] F:%d",
                 s_currentSlot + 1, s_slots[s_currentSlot].frameCount);
        color = IM_COL32(255, 80, 80, 255);
        break;
    case MACRO_REPLAYING:
        snprintf(buf, sizeof(buf), "MACRO PLAY [%d] F:%d/%d",
                 s_currentSlot + 1, s_playFrame, s_slots[s_currentSlot].frameCount);
        color = IM_COL32(80, 255, 120, 255);
        break;
    default:
        return;
    }

    const ImVec2 textSize = ImGui::CalcTextSize(buf);
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float x = (displaySize.x - textSize.x) * 0.5f;
    const float y = displaySize.y - 84.0f;  // Slot 2: above FA overlay

    dl->AddRectFilled(
        ImVec2(x - 6.0f, y - 2.0f),
        ImVec2(x + textSize.x + 6.0f, y + textSize.y + 2.0f),
        IM_COL32(0, 0, 0, 180), 4.0f);
    dl->AddText(ImVec2(x, y), color, buf);
}

// ============================================================================
// ImGui Panel
// ============================================================================

void InputMacro_RenderImGui(void) {
    if (!s_initialized) return;

    ImGui::Text("Current Slot: %d", s_currentSlot + 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Next Slot")) {
        InputMacro_NextSlot();
    }

    // State display
    const char* stateStr = "Idle";
    switch (s_state) {
    case MACRO_PRE_RECORD: stateStr = "Pre-Record"; break;
    case MACRO_RECORDING:  stateStr = "Recording";  break;
    case MACRO_REPLAYING:  stateStr = "Replaying";  break;
    default: break;
    }
    ImGui::Text("State: %s", stateStr);

    // Record/Play buttons
    if (s_state == MACRO_IDLE || s_state == MACRO_PRE_RECORD) {
        if (ImGui::Button("Record")) {
            InputMacro_ToggleRecord();
        }
        ImGui::SameLine();
    }

    if (s_state == MACRO_RECORDING) {
        if (ImGui::Button("Stop Recording")) {
            InputMacro_ToggleRecord();
        }
        ImGui::SameLine();
        ImGui::Text("Frame: %d", s_slots[s_currentSlot].frameCount);
    }

    if (s_state == MACRO_IDLE) {
        const MacroSlot& slot = s_slots[s_currentSlot];
        bool canPlay = slot.hasData && slot.frameCount > 0;
        if (!canPlay) ImGui::BeginDisabled();
        if (ImGui::Button("Play")) {
            InputMacro_TogglePlay();
        }
        if (!canPlay) ImGui::EndDisabled();
    } else if (s_state == MACRO_REPLAYING) {
        if (ImGui::Button("Stop Playback")) {
            InputMacro_TogglePlay();
        }
        ImGui::SameLine();
        ImGui::Text("Frame: %d / %d", s_playFrame, s_slots[s_currentSlot].frameCount);
    }

    // Slot overview
    ImGui::Separator();
    ImGui::Text("Slots:");
    for (int i = 0; i < MACRO_MAX_SLOTS; i++) {
        const MacroSlot& slot = s_slots[i];
        bool selected = (i == s_currentSlot);

        if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f));

        if (slot.hasData) {
            ImGui::Text("  [%d] %d frames (%.1fs)", i + 1, slot.frameCount, slot.frameCount / 60.0f);
        } else {
            ImGui::Text("  [%d] empty", i + 1);
        }

        if (selected) ImGui::PopStyleColor();

        // Right-click to clear
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (s_state == MACRO_IDLE) {
                ClearSlot(i);
                LOG_INFO("[Macro] Cleared slot %d", i + 1);
            }
        }

        // Click to select
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (s_state == MACRO_IDLE || s_state == MACRO_PRE_RECORD) {
                if (s_state == MACRO_PRE_RECORD) {
                    if (s_preRecordSwapped) {
                        PracticeTools_ApplyControlSwapState(false);
                        s_preRecordSwapped = false;
                    }
                    s_state = MACRO_IDLE;
                }
                s_currentSlot = i;
            }
        }
    }
    ImGui::TextDisabled("Left-click to select, right-click to clear");

    ImGui::Separator();
    ImGui::Checkbox("Macro Debug Log", &s_debugLogging);
    if (s_debugLogging) {
        uint8_t p1cpu = ReadMemory<uint8_t>(ADDR_P1_CPU_FLAG);
        uint8_t p2cpu = ReadMemory<uint8_t>(ADDR_P2_CPU_FLAG);
        bool swap = InputSystem_GetControlSwap();
        bool overrideActive = false;
        // Check if override is active by reading the override state
        uint16_t p2Input = InputSystem_GetInput(1);
        ImGui::Text("CPU: P1=%u P2=%u  Swap=%d", p1cpu, p2cpu, swap ? 1 : 0);
        ImGui::Text("P2 Input: 0x%04X", p2Input);
        if (s_state == MACRO_REPLAYING) {
            ImGui::Text("Play: %d/%d", s_playFrame, s_slots[s_currentSlot].frameCount);
        }
    }
}
