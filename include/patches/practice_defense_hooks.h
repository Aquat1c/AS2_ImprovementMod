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
#include "core/as2_constants.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct PracticeDefenseConfig {
    bool enabled = false;               // advanced (mod-driven) dummy control selected
    Training::BlockPolicy policy = Training::BlockPolicy::Off;
    int randomPercent = 50;
    bool preferCrouch = true;           // posture chosen for masks that allow either
    Training::DefensiveResponse response = Training::DefensiveResponse::NormalGuard;
    // 0 or 1: which entity the mod is driving. Follows the control swap.
    int dummyPlayer = 1;
    bool macroOwnsDummyInput = false;
    bool paused = false;
};

// Live state of the native arming hooks. Everything here is written to the log
// on change, so a run can be read back without having had the game in front of
// you: what the dummy was set to, whether anything was driving it, and whether
// the window was actually open when the hit arrived.
struct PracticeDefenseArmState {
    bool installed = false;
    uint32_t arms = 0;            // windows opened since the last reset
    uint32_t parryArms = 0;
    uint32_t repelArms = 0;
    uint32_t pushAwayArms = 0;
    uint32_t defenceAllowedForced = 0;   // +1949 had to be written
    uint32_t absoluteStockForced = 0;    // +823 had to be granted
    uint32_t guardCancelsOffered = 0;    // the offer queued a cancel action
    uint32_t counterGuardRouteOpened = 0; // route 0 stated as matched
    uint32_t lastRouteLogCount = 0;       // so the outcome logs on change only
    uint32_t contactsPrepared = 0;       // window pointed at a specific contact
    uint32_t resolverCalls = 0;          // category resolver reached the dummy
    uint32_t resolverDeclines = 0;       // ...and returned 1
    uint32_t lastArmFrame = 0;

    // Live window bytes, sampled once per collision tick.
    uint8_t parryWindow = PARRY_WINDOW_IDLE;
    int32_t repelReaction = REPEL_REACTION_IDLE;
    uint8_t repelTimer = 0;
    uint8_t pushAwayTimer = PUSH_AWAY_TIMER_IDLE;
    bool pushAwayFree = false;
    uint8_t guardStock = 0;       // +823, the category-5 counter-guard stock

    // What the dummy is set to do, and whether anything is driving it.
    int category = 0;
    Training::DefensiveResponse configured = Training::DefensiveResponse::NormalGuard;
    Training::DefensiveResponse effective = Training::DefensiveResponse::NormalGuard;
    bool gateOpen = false;
    bool hookDriven = false;      // the mechanic is armed by hook, not by input
    Training::DefenseInputKind input = Training::DefenseInputKind::None;

    // Prediction, so "nothing is coming" reads differently from "it is coming
    // and we still did nothing".
    int16_t recordsToAttack = -1;
    bool attackerCommitted = false;
    bool threatArmed = false;
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

    // One counter per contact resolution code (3..11), so the menu can show
    // what the dummy actually produced rather than only what it last produced.
    uint32_t resultCounts[12] = {};
    uint32_t contactTicks = 0;
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

// True when this frame must drop BACK so the next press re-arms the native
// parry window. Only ever true under DefensiveResponse::JustParry.
// What the dummy should press this frame to attempt its defensive mechanic.
// DefenseInputKind::None means "just hold the ordinary guard".
Training::DefenseInputKind PracticeDefense_DefenseInput();

// Which frame of the 214D motion is being fed, for the injector to turn into a
// direction. -1 when no motion is running.
int PracticeDefense_ArmStep();

// True when the selected mechanic is a guard cancel and the block policy is Off,
// so the mod is guarding on its behalf. The Auto-Block row says so rather than
// reporting an "Off" that is not what is happening.
bool PracticeDefense_BlockForcedByResponse();
// The dummy's defence category from dword_73E070, so the menu can say when the
// selected character simply has no parry.
int PracticeDefense_DummyDefenseCategory();

const PracticeDefenseTelemetry& PracticeDefense_GetTelemetry();
const PracticeDefenseArmState& PracticeDefense_GetArmState();

// True when the three window mechanics are being armed through the engine's own
// setters. False means the hooks did not install and the driver is back to
// simulating the input, which is late by construction.
bool PracticeDefense_ArmHooksActive();

// One-shot: true once for each frame a category resolver awarded the dummy its
// defensive mechanic, with the contact resolution code that was awarded. This
// is the seam a follow-up hangs off - the counter connecting, the absolute
// defence firing - because it is the only place that knows the mechanic
// actually resolved rather than merely being armed.
bool PracticeDefense_ConsumeDefensiveSuccess(uint32_t* outFrame, uint32_t* outResult);

// Entity_ProcessCommandMatches seam, called by whoever owns that hook.
//
// MinHook allows one hook per target and the frame-advantage recovery observer
// already owns 0x4BEA20, so creating a second one there does not fail quietly -
// it takes the address and leaves the other hook uninstalled. The preemptive
// counter guard therefore chains off the existing detour instead: Entry runs
// before the original, Exit after it.
void PracticeDefense_OnCommandMatchesEntry(uintptr_t entity);
void PracticeDefense_OnCommandMatchesExit(uintptr_t entity);

// Told by the owner of that hook whether the seam is live at all. Without it
// the preemptive counter guard has to fall back to feeding the 214D motion.
void PracticeDefense_SetCounterGuardRouteAvailable(bool available);


// Contact groups resolved so far in the live sequence: what First / After First
// Hit must key off, because it has to see contacts that land while the dummy is
// already in blockstun.
bool PracticeDefense_SequenceActive();
uint32_t PracticeDefense_ResolvedContactGroups();

void PracticeDefense_Reset();
void PracticeDefense_CaptureState(PracticeDefenseRuntimeState* out);
void PracticeDefense_RestoreState(const PracticeDefenseRuntimeState* state);
