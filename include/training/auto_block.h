#pragma once

/**
 * Alice Senki 2 - Practice auto-block core (pure logic).
 *
 * Everything in this header is free of game-memory access so it can be unit
 * tested. The hooks that read the live match and drive it live in
 * patches/practice_defense_hooks.
 *
 * Model (verified against the decomp):
 *   - A contact carries an attack mask whose low three bits are guard lanes.
 *     Bit 0 = stand lane, bit 1 = crouch lane, bit 2 = air-guardable.
 *   - The defender carries its own lanes in the low bits of +1940; the native
 *     ordinary-guard resolver matches them bit-for-bit.
 *   - Exactly ONE guard lane is latched per simulation frame. Collision
 *     callbacks are sequential but a player's input is not, so switching lanes
 *     between callbacks would produce defense no human could perform.
 */

#include <stddef.h>
#include <stdint.h>

namespace Training {

// --- Attack-mask guard lanes (entity+1740 / HitDef entry+16) ---
constexpr uint32_t kGuardLaneStand  = 0x00001u;
constexpr uint32_t kGuardLaneCrouch = 0x00002u;
constexpr uint32_t kGuardLaneAir    = 0x00004u;
constexpr uint32_t kGroundGuardMask = kGuardLaneStand | kGuardLaneCrouch;

constexpr uint32_t kAttackBypassDefender1932   = 0x00800u;
constexpr uint32_t kHitDefSuppressPlayerHit    = 0x01000u;
constexpr uint32_t kAttackSpecialGuardRequired = 0x02000u;
constexpr uint32_t kAttackContactOverride      = 0x20000u;
constexpr uint32_t kAttackBypassNativeDefense  = 0x80000u;

// Only these defender bits may ever be touched by auto-block.
constexpr uint32_t kTemporaryGuardBits = kGroundGuardMask;

// Defender capability bits (+1940) auto-block must never fabricate.
constexpr uint32_t kDefenderAirGuard     = 0x00004u;
constexpr uint32_t kDefenderGuardPoint   = 0x00400u;
constexpr uint32_t kDefenderSpecialGuard = 0x02000u;

// Sequence stays alive this many quiet simulation frames before ending.
constexpr uint32_t kSequenceQuietGrace = 2u;

enum class ThreatSource : uint8_t {
    DirectPlayer = 0,
    HitDef = 1,
};

enum class GroundGuardClass : uint8_t {
    None = 0,
    StandOnly,
    CrouchOnly,
    Either,
    SpecialGuardRequired,
};

enum class GuardLane : uint8_t {
    Unset = 0,
    Stand,
    Crouch,
    Air,
    None,
    Conflict,
};

enum class ContactResolution : uint8_t {
    None = 1,
    UniqueDefense = 3,
    JustParry = 4,
    Repel = 5,
    PushAway = 6,
    AbsoluteDefense = 7,
    Dodge = 8,
    GuardPoint = 9,
    Guard = 10,
    Hit = 11,
};

enum class BlockPolicy : uint8_t {
    Off = 0,
    All,
    Adaptive,
    FirstHit,
    AfterFirstHit,
    Random,
};

// Held as a semantic direction and resolved to physical left/right as late as
// possible, so a side change between input and contact cannot stale the guard.
enum class SemanticDirection : uint8_t {
    Neutral = 0,
    Back,
    DownBack,
    AirBack,
};

struct AttackPayloadView {
    uint32_t attackMask = 0;
    uint32_t attackLevel = 0;
    uint8_t attackState = 0;
    uint8_t hitActive = 0;
};

struct ThreatKey {
    ThreatSource source = ThreatSource::DirectPlayer;
    uint8_t owner = 0;
    uint16_t hitDefSlot = 0xFFFFu;   // 0xFFFF for a direct attack
    uint32_t objectId = 0;           // HitDef ID, or the attacker's action generation
    uintptr_t payloadAddress = 0;
};

// Identity ignores slot: HitDef compaction moves entries between frames.
bool SameThreatIdentity(const ThreatKey& a, const ThreatKey& b);
uint32_t ThreatKeyHash(const ThreatKey& key);

struct ThreatContact {
    ThreatKey key{};
    uint32_t attackMask = 0;
    uint32_t attackLevel = 0;
    uint8_t attackState = 0;
    uint8_t hitActive = 0;
    bool contactOverride = false;
    bool directMelee = false;
    bool overlapsNow = false;
    bool normalGroundGuardPossible = false;
    bool airGuardCandidate = false;
};

struct FrameGuardPlan {
    uint32_t simFrame = 0;
    bool valid = false;
    bool policyWantsBlock = false;
    bool exactContactScan = false;
    bool guardBypassPresent = false;
    bool throwOrSpecialContactPresent = false;
    bool defenderAirborne = false;
    bool laneLatched = false;
    GuardLane lane = GuardLane::Unset;
    uint32_t commonGroundLanes = 0;
    uint32_t contactOrdinal = 0;
    uint32_t threatCount = 0;
    uint32_t conflictCount = 0;
};

struct AutoBlockSequenceState {
    bool active = false;
    uint32_t generation = 0;
    uint32_t startFrame = 0;
    uint32_t lastThreatFrame = 0;
    uint32_t lastContactFrame = 0;
    uint32_t resolvedContactGroups = 0;
    uint32_t quietFrames = 0;
    bool randomBlock = false;
};

struct FrameContactOutcome {
    uint32_t simFrame = 0;
    bool anyContact = false;
    uint8_t finalResult = 0;
    uint32_t resultMask = 0;         // bit per observed result 3..11
    uint32_t directContacts = 0;
    uint32_t hitDefContacts = 0;
    bool ordinaryGuardHookCalled = false;
    bool temporaryLaneApplied = false;
    bool nativeGuardWithoutMod = false;
};

struct DefenderGuardState {
    bool practiceAdvancedModeActive = false;
    bool controlSwapActive = false;
    bool macroOwnsDummyInput = false;
    uint16_t guardGauge = 0;
    uint32_t actionId = 0;
    bool airborne = false;

    // Whether the engine would consider an ordinary attack input for this
    // fighter right now, read from the command-route vector. Action IDs cannot
    // answer this: character specials share the 34..84 range with the reaction
    // and movement states, so an ID test lets a committed special through.
    bool neutralRouteOpen = false;
    // Live attack payload (+0x06C8 == 1). Belt and braces for the active window.
    bool attackActive = false;
};

// --- Decoder -------------------------------------------------------------

GroundGuardClass DecodeGroundGuardClass(uint32_t attackMask);
const char* GroundGuardClassLabel(GroundGuardClass cls);
const char* GuardLaneLabel(GuardLane lane);
const char* ContactResolutionLabel(uint32_t result);
const char* BlockPolicyLabel(BlockPolicy policy);

// True when normal ground guard is possible in principle for this mask.
bool AttackHasGroundGuardLane(uint32_t attackMask);

// The native air-guard predicate, reproduced exactly: the defender must already
// hold both ground lanes and either the attack or the defender must supply the
// air bit. Auto-block supplies the ground lanes only; it never fabricates 0x4.
bool NativeAirGuardPredicate(uint32_t attackMask, uint32_t defenderFlags);

// The 0x2000 gate: ordinary guard is refused unless the defender already has
// the matching capability. Auto-block must not add it.
bool SpecialGuardSatisfied(uint32_t attackMask, uint32_t defenderFlags);

// --- Per-frame plan ------------------------------------------------------

void ResetPlan(FrameGuardPlan& plan, uint32_t simFrame, bool defenderAirborne);

// Exact scan: fold one expected contact into the frame's lane intersection.
void PlanAccumulateContact(FrameGuardPlan& plan, uint32_t attackMask);

// Resolve the accumulated intersection into a single lane. preferCrouch picks
// the posture for masks that permit either.
void PlanFinalize(FrameGuardPlan& plan, bool preferCrouch);

// Fallback when the exact scan produced no lane: latch from the contact the
// resolver actually presented. Returns true when a lane was latched.
bool PlanLatchFromContact(FrameGuardPlan& plan,
                          uint32_t attackMask,
                          uint32_t defenderFlags,
                          bool preferCrouch);

// Would augmenting for this attack be consistent with the frame's latched lane?
// A later incompatible contact must be reported as a conflict, never switch it.
bool PlanAcceptsAttack(const FrameGuardPlan& plan,
                       uint32_t attackMask,
                       uint32_t defenderFlags);

// --- Scoped defender-lane mutation --------------------------------------

uint32_t GuardLaneBits(GuardLane lane);
uint32_t ApplyGuardLaneBits(uint32_t oldFlags, GuardLane lane);
uint32_t RestoreGuardLaneBits(uint32_t afterFlags, uint32_t oldFlags);

// True when the defender's own lanes already satisfy this contact, so no
// augmentation is needed (telemetry: native_guard_without_mod).
bool DefenderAlreadyGuards(uint32_t attackMask, uint32_t defenderFlags, bool airborne);

// --- Eligibility ---------------------------------------------------------

bool CanAutoGuardAtContact(const DefenderGuardState& state);

// --- Policy / sequence ---------------------------------------------------

bool PolicyWantsBlock(BlockPolicy policy,
                      const AutoBlockSequenceState& sequence,
                      uint32_t contactOrdinal);

uint32_t DeterministicSequenceRoll(uint32_t startFrame,
                                   uint32_t generation,
                                   uint32_t attackerCharId,
                                   uint32_t defenderCharId,
                                   const ThreatKey& firstThreat);

void SequenceBegin(AutoBlockSequenceState& sequence,
                   uint32_t simFrame,
                   uint32_t roll,
                   int randomPercent);

void SequenceNoteThreat(AutoBlockSequenceState& sequence, uint32_t simFrame);

// Counts one contact GROUP: several objects resolving on the same simulation
// frame are one temporal hit for First/After-First purposes.
bool SequenceNoteContactGroup(AutoBlockSequenceState& sequence, uint32_t simFrame);

// Advance the quiet counter; returns true when the sequence should end.
bool SequenceTickQuiet(AutoBlockSequenceState& sequence,
                       bool threatPresent,
                       bool defenderInForcedState,
                       uint32_t quietGrace);

void SequenceEnd(AutoBlockSequenceState& sequence);

// --- Anticipatory input --------------------------------------------------

SemanticDirection SemanticGuardForLane(GuardLane lane, bool defenderAirborne);

// Resolves at injection time from live facing, never from a cached direction.
uint16_t ResolveSemanticDirection(SemanticDirection direction, bool defenderFacingRight);

} // namespace Training
