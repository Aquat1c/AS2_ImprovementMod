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

// What the dummy should attempt instead of, or as well as, an ordinary guard.
// Only responses whose LEGAL input path is traced end to end appear here; the
// rest would be a lie until their per-character command arming is mapped.
enum class DefensiveResponse : uint8_t {
    NormalGuard = 0,
    // Matches whatever dword_73E070 gives this character, so one setting covers
    // the whole roster instead of asking the player which mechanic they have.
    CharacterNative,
    // Re-arms the native parry window by releasing and re-pressing BACK, which
    // is the only thing Entity_CheckHitState accepts. Category 2 only.
    JustParry,
    // D out of blockstun with 500 meter. Category 6 only.
    Dodge,
    // Tap FORWARD from neutral to arm the hit-reaction window. Category 3 only.
    Repel,
    // 6D (or 4D) - the guide lists it as "6D or 4D; air OK". Category 1 only.
    GuardCounter,
    // Guard + D, timed so the press is a fresh edge inside blockstun. That is
    // the branch Entity_CheckAirTech takes for free. Category 4 only.
    PushAwayPerfect,
    // 214D - the "Warzard counter". Enables a special guard state that repels
    // attacks; +823 is that state, and action 49 is what it produces when the
    // opponent connects. Category 5 only.
    AbsoluteDefence,
    // The same mechanic on the other branch: D simply held, which makes +78
    // read 0 inside blockstun and sends it to the "or 100 meter" arm. Weaker,
    // but it is what an untimed press actually does. Category 4 only.
    PushAwayMetered,
    Count,
};

// Defence categories from dword_73E070, confirmed against the guide's own table.
constexpr int kDefenseCategoryUnique = 1;
constexpr int kDefenseCategoryRepel = 3;
// Repel arms from Entity_CheckGuardState: a FORWARD tap taken from neutral sets
// a 24-frame reaction window at +1990. +1980 == -1 means nothing is armed.
constexpr int32_t kReactionIdle = -1;
constexpr int kDefenseCategoryPushAway = 4;
constexpr int kDefenseCategoryAbsolute = 5;
constexpr int kDefenseCategoryDodge = 6;

// Which response a category can actually perform on legal input today.
DefensiveResponse ResponseForCategory(int category);
bool ResponseSupportedByCategory(DefensiveResponse response, int category);
const char* DefensiveResponseLabel(DefensiveResponse response);

// Drives the parry re-arm. The window byte is the engine's own state, so the
// cadence follows it rather than a guessed decay rate: whenever the window
// reads idle we owe a fresh edge, which means one frame without BACK.
struct ParryInputState {
    bool releasedLastFrame = false;
};

// Returns true when this frame must NOT hold BACK, so the next one lands as a
// fresh press. windowByte is the live +1965.
bool ParryWantsRelease(ParryInputState& state, uint8_t windowByte, bool threatArmed);

// What the dummy should press this frame to attempt its defensive mechanic.
// Every one of these is an input a player could make; nothing here writes state.
enum class DefenseInputKind : uint8_t {
    None = 0,        // hold the ordinary guard
    ReleaseGuard,    // no direction, so the next frame's press is a fresh edge
    ForwardTap,      // repel arm: FORWARD from neutral
    DodgePress,      // D + BACK out of blockstun
    CounterForward,  // D + FORWARD, the category-1 guard counter
    ArmGuardState,   // run 214D, the category-5 counter-guard motion
    DownTap,         // 2 from neutral, the low half of the category-3 parry
};

struct DefenseDriveSample {
    uint8_t parryWindow = 0xFF;   // +1965, 0xFF = idle
    int32_t reactionState = -1;   // +1980, -1 = no reaction armed
    uint32_t actionId = 0;
    uint16_t meter = 0;
    bool airborne = false;
    bool threatArmed = false;
    bool actionable = false;      // engine would accept an ordinary input now
    bool guardStateArmed = false; // +823, the category-5 counter-guard flag
    // Which lane the incoming attack demands. The category-3 parry is 6 against
    // high and mid but 2 against low, so the driver has to know which is coming.
    GroundGuardClass threatClass = GroundGuardClass::None;
};

struct DefenseDriveState {
    ParryInputState parry{};
    bool tapPhase = false;        // alternates the neutral / press frames
    bool pushPhase = false;       // alternates the D press so each one is an edge
    bool counterFired = false;    // one counter per threat, not a held direction
    // 214D is a motion, not a press, so the driver walks it a frame at a time.
    int8_t armStep = -1;          // -1 idle, else the step being fed
};

// Frames in the 214D motion. Matches the game's own command table entry for it
// (0x723480 cmd 26: 2, 1, 4 with the D gate).
constexpr int kGuardStateMotionFrames = 3;

DefenseInputKind EvaluateDefenseInput(DefensiveResponse response,
                                      const DefenseDriveSample& sample,
                                      DefenseDriveState& state);

// Dodge is a guard cancel: D during blockstun with the meter to pay for it.
// Entity_UpdateAction_Standard only offers it from actions 67/68 (crouch) and
// 70/71 (air) blockstun, so pressing it anywhere else is wasted.
constexpr uint16_t kDodgeMeterCost = 500;
bool DodgeWindowOpen(uint32_t actionId, uint16_t meter);

// True for mechanics the engine only offers *out of blockstun*, so a dummy that
// is not blocking can never perform them however hard it presses. Selecting one
// therefore has to imply guarding.
bool ResponseRequiresBlockstun(DefensiveResponse response);

// Push-away is armed by Entity_CheckAirTech (0x424C90), reached only from the
// four category-4 characters. It needs BACK held *and* D held; a fresh D press
// inside blockstun arms it outright, and otherwise 100 meter does. So it is not
// input-free - it is guard + D, the same shape as the dodge with a cheaper gate
// and a wider set of accepting actions.
constexpr uint16_t kPushAwayMeterCost = 100;
bool PushAwayWindowOpen(uint32_t actionId, uint16_t meter);

// The free half of that: the actions whose *fresh* D press arms push-away
// without paying. Entity_CheckAirTech takes the free branch only when +78 reads
// just-pressed inside one of these, so a D held from before contact reads 0
// there and silently drops to the 100-meter branch instead.
bool PushAwayFreeWindow(uint32_t actionId);

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

// dword_73E070[charId]: 1 unique, 2 just-parry, 3 repel, 4 push-away,
// 5 absolute defense, 6 dodge. 0 means the character has no category handler.
constexpr int kDefenseCategoryJustParry = 2;

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
