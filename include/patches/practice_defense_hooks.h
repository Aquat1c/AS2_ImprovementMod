#pragma once

/**
 * Alice Senki 2 - Practice-mode contact-time defense hooks.
 *
 * Why these exist: PracticeTools_FrameUpdate runs from EndScene, AFTER the tick
 * that already resolved collisions. A move that initialises and activates in one
 * tick, or a HitDef spawned already active, has therefore hit before any
 * post-render observer can see it. Reacting to that observation is exactly the
 * "takes hit 1, blocks hit 2" defect.
 *
 * The correctness authority is the shared ordinary-guard resolver
 * (ADDR_DEFENSE_ORDINARY_GUARD). It runs only after a real contact has been
 * found and after character-specific defense and guard point have declined, and
 * BEFORE the normal-hit fallback. Supplying the defender's guard lane inside
 * that call turns the would-be hit into a block on the same frame.
 *
 * The hooks never fabricate special-guard, guard-point or air capability, and
 * they only ever touch the defender's two low guard-lane bits, restoring them
 * as soon as the original resolver returns.
 */

#include "training/auto_block.h"

#include <stdbool.h>
#include <stdint.h>

struct PracticeDefenseConfig {
    bool enabled = false;               // advanced (mod-driven) dummy control selected
    Training::BlockPolicy policy = Training::BlockPolicy::Off;
    int randomPercent = 50;
    bool preferCrouch = true;           // posture chosen for masks that allow either
    int dummyPlayer = 1;
    bool controlSwapActive = false;
    bool macroOwnsDummyInput = false;
    bool paused = false;
};

struct PracticeDefenseTelemetry {
    // Current frame plan / sequence.
    Training::FrameGuardPlan plan{};
    Training::AutoBlockSequenceState sequence{};
    Training::FrameContactOutcome outcome{};

    // Last resolver call that reached the dummy.
    bool hasLastContact = false;
    uint32_t lastContactFrame = 0;
    Training::ThreatSource lastSource = Training::ThreatSource::DirectPlayer;
    uint32_t lastHitDefId = 0;
    int lastHitDefSlot = -1;
    uint32_t lastAttackMask = 0;
    Training::GroundGuardClass lastClass = Training::GroundGuardClass::None;
    Training::GuardLane lastLane = Training::GuardLane::Unset;
    uint32_t lastResult = 0;
    uint32_t lastFlagsBefore = 0;
    uint32_t lastFlagsTemp = 0;
    bool lastPrearmed = false;
    bool lastSafetyArm = false;
    bool lastNativeGuardWithoutMod = false;
    bool lastEligible = false;
    int8_t facingAtInput = 0;
    int8_t facingAtContact = 0;

    // Counters since the last reset.
    uint32_t guardHookCalls = 0;
    uint32_t laneApplications = 0;
    uint32_t conflictFrames = 0;
    uint32_t refusedNoLane = 0;
    uint32_t refusedSpecialGuard = 0;
    uint32_t lateBlockSignatures = 0;   // result 11 then 10 on the same threat
};

struct PracticeDefenseRuntimeState {
    Training::AutoBlockSequenceState sequence{};
    Training::FrameGuardPlan plan{};
    uint32_t lastProcessedSimFrame = 0;
};

// Installs the collision-phase and resolver hooks. Returns false when the
// ordinary-guard hook could not be created; auto-block must then be reported as
// unavailable rather than silently falling back to the post-render observer.
bool PracticeDefense_Install();
void PracticeDefense_Uninstall();

bool PracticeDefense_IsInstalled();
bool PracticeDefense_HasOrdinaryGuardHook();
const char* PracticeDefense_GetInstallError();

// Pushed once per frame by the practice runtime.
void PracticeDefense_SetConfig(const PracticeDefenseConfig& config);

// Semantic guard the anticipatory input path should hold this frame, plus the
// armed-threat mask that produced it (0 when nothing is armed).
Training::SemanticDirection PracticeDefense_GetAnticipatoryGuard();
uint32_t PracticeDefense_GetAnticipatoryMask();

// True when an armed threat plus the active policy means the dummy should
// already be holding guard, before geometry connects.
bool PracticeDefense_WantsAnticipatoryGuard();
void PracticeDefense_NoteAnticipatoryFacing(int8_t facing, bool prearmed);

const PracticeDefenseTelemetry& PracticeDefense_GetTelemetry();

// Contact groups resolved so far in the live sequence: what First / After First
// Hit must key off, because it has to see contacts that land while the dummy is
// already in blockstun.
bool PracticeDefense_SequenceActive();
uint32_t PracticeDefense_ResolvedContactGroups();

void PracticeDefense_Reset();
void PracticeDefense_CaptureState(PracticeDefenseRuntimeState* out);
void PracticeDefense_RestoreState(const PracticeDefenseRuntimeState* state);
