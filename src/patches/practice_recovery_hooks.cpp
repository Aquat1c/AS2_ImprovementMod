#include "patches/practice_recovery_hooks.h"

#include "core/game_state.h"
#include "core/mod_main.h"
#include "patches/memory_utils.h"
#include "training/frame_advantage.h"
#include "training/practice_tools.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

#include <stdio.h>
#include <string.h>

using namespace Training;

namespace {

typedef int (__cdecl* ProcessCommandMatches_t)(uint32_t*);

ProcessCommandMatches_t g_origProcessCommandMatches = nullptr;
bool g_installed = false;
char g_installError[128] = {};

NativeRecoverySample g_sample[2] = {};
NativeRecoveryResult g_result[2] = {};
uint32_t g_sampleFrame[2] = {};
bool g_hasSample[2] = {};

int ResolvePlayerIndex(uintptr_t entityBase) {
    if (entityBase == GetEntityBase(0)) return 0;
    if (entityBase == GetEntityBase(1)) return 1;
    return -1;
}

// charData + 172; charData is the pointer at entity+0, whose +176 is the
// character id used to index dword_73E070.
uint8_t ReadCpuFlag(uintptr_t entityBase) {
    const uint32_t block = ReadMemory<uint32_t>(entityBase);
    if (!block) {
        return 0;
    }
    return ReadMemory<uint8_t>((uintptr_t)block + 172);
}

} // namespace

NativeRecoverySample PracticeRecovery_ReadSample(uintptr_t entityBase) {
    NativeRecoverySample s{};
    if (!entityBase) {
        return s;
    }

    s.currentAction = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_ACTION_ID);
    s.pendingAction1 = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_PENDING_ACTION_1);
    s.pendingAction2 = ReadMemory<uint32_t>(entityBase + ENTITY_OFF_PENDING_ACTION_2);
    s.airborne = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_AIRBORNE);
    s.downHeld = ReadMemory<uint16_t>(entityBase + ENTITY_OFF_INPUT_DOWN);
    s.cpuControlled = ReadCpuFlag(entityBase);
    s.trainingRestoreGate = ReadMemory<uint8_t>(entityBase + ENTITY_OFF_TRAINING_RESTORE_GATE);

    CopyMemorySafe(s.routes,
                   reinterpret_cast<const void*>(entityBase + ENTITY_OFF_COMMAND_ROUTE_TIMERS),
                   ENTITY_COMMAND_ROUTE_COUNT);
    return s;
}

namespace {

int __cdecl Hook_ProcessCommandMatches(uint32_t* fighter) {
    const uintptr_t entityBase = reinterpret_cast<uintptr_t>(fighter);
    const int player = ResolvePlayerIndex(entityBase);

    if (player >= 0 && PracticeTools_IsPracticeModeActive()) {
        const uint32_t frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        g_sample[player] = PracticeRecovery_ReadSample(entityBase);
        g_result[player] = EvaluateNativeRecovery(g_sample[player]);
        g_sampleFrame[player] = frame;
        g_hasSample[player] = true;
        FrameAdvantage_OnPreCommandDispatch(player, frame, g_sample[player], g_result[player]);
    }

    const int result = g_origProcessCommandMatches
        ? g_origProcessCommandMatches(fighter)
        : 0;

    if (player >= 0 && PracticeTools_IsPracticeModeActive()) {
        // Audit only: did a real input replace the terminal handoff this tick?
        FrameAdvantage_OnPostCommandDispatch(
            player,
            g_sampleFrame[player],
            ReadMemory<uint32_t>(entityBase + ENTITY_OFF_PENDING_ACTION_1),
            ReadMemory<uint32_t>(entityBase + ENTITY_OFF_PENDING_ACTION_2));
    }

    return result;
}

} // namespace

bool PracticeRecovery_Install() {
    if (g_installed) {
        return true;
    }

    g_installError[0] = '\0';
    PracticeRecovery_Reset();

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_ENTITY_PROCESS_COMMAND_MATCHES),
        reinterpret_cast<void*>(&Hook_ProcessCommandMatches),
        reinterpret_cast<void**>(&g_origProcessCommandMatches));
    if (status != MH_OK) {
        snprintf(g_installError, sizeof(g_installError), "create failed (%d)", (int)status);
        LOG_ERROR("[FARecovery] %s", g_installError);
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(ADDR_ENTITY_PROCESS_COMMAND_MATCHES));
    if (status != MH_OK) {
        snprintf(g_installError, sizeof(g_installError), "enable failed (%d)", (int)status);
        LOG_ERROR("[FARecovery] %s", g_installError);
        return false;
    }

    LOG_INFO("[FARecovery] hooked Entity_ProcessCommandMatches @ 0x%08X",
             (unsigned)ADDR_ENTITY_PROCESS_COMMAND_MATCHES);
    g_installed = true;
    return true;
}

void PracticeRecovery_Uninstall() {
    if (!g_installed) {
        return;
    }
    MH_DisableHook(reinterpret_cast<void*>(ADDR_ENTITY_PROCESS_COMMAND_MATCHES));
    g_installed = false;
}

bool PracticeRecovery_IsInstalled() {
    return g_installed;
}

const char* PracticeRecovery_GetInstallError() {
    return g_installError;
}

const NativeRecoverySample& PracticeRecovery_GetSample(int player) {
    static const NativeRecoverySample empty{};
    if (player < 0 || player > 1) {
        return empty;
    }
    return g_sample[player];
}

const NativeRecoveryResult& PracticeRecovery_GetResult(int player) {
    static const NativeRecoveryResult empty{};
    if (player < 0 || player > 1) {
        return empty;
    }
    return g_result[player];
}

uint32_t PracticeRecovery_GetSampleFrame(int player) {
    if (player < 0 || player > 1) {
        return 0;
    }
    return g_sampleFrame[player];
}

bool PracticeRecovery_HasSample(int player) {
    if (player < 0 || player > 1) {
        return false;
    }
    return g_hasSample[player];
}

void PracticeRecovery_Reset() {
    memset(g_sample, 0, sizeof(g_sample));
    memset(g_result, 0, sizeof(g_result));
    memset(g_sampleFrame, 0, sizeof(g_sampleFrame));
    g_hasSample[0] = false;
    g_hasSample[1] = false;
}
