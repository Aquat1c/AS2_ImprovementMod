#pragma once

/**
 * Alice Senki 2 - Pre-command-dispatch observation hook.
 *
 * Frame advantage used to detect recovery from an action-ID edge sampled after
 * the tick finished. That is ambiguous in both directions: the engine queues a
 * terminal neutral handoff late in tick F and only applies it during tick F+1,
 * so the visible ID changes a tick after control actually returned, while a
 * post-tick route profile describes the tick that has not run yet.
 *
 * Entity_ProcessCommandMatches (0x4BEA20) is the exact input-acceptance
 * boundary. Observed on entry, the route vector is what the engine is about to
 * scan and the pending slots still hold the previous tick's handoff.
 */

#include "training/native_recovery.h"

#include <stdbool.h>
#include <stdint.h>

bool PracticeRecovery_Install();
void PracticeRecovery_Uninstall();
bool PracticeRecovery_IsInstalled();
const char* PracticeRecovery_GetInstallError();

// Latest pre-command sample and its evaluation, for the HUD and for any system
// that needs an honest "can this fighter act" answer.
const Training::NativeRecoverySample& PracticeRecovery_GetSample(int player);
const Training::NativeRecoveryResult& PracticeRecovery_GetResult(int player);
uint32_t PracticeRecovery_GetSampleFrame(int player);
bool PracticeRecovery_HasSample(int player);

void PracticeRecovery_Reset();

// Reads the vector straight out of entity memory. Used by the collision hooks,
// which run after command dispatch but within the same tick.
Training::NativeRecoverySample PracticeRecovery_ReadSample(uintptr_t entityBase);
