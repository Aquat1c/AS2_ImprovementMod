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

// A sequence stays alive through this many quiet simulation frames. It is the
// blockstring-continuity window, so it decides what First Hit and After First
// Hit consider "the same pressure": too short and every gap starts a fresh
// sequence, which makes First Hit block the second hit as if it were the first.
constexpr uint32_t kSequenceQuietGrace = 30u;

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
    // Blocks whatever the attack demands, switching lane per contact.
    All,
    // Blocks only what the dummy's STANCE covers, so a low against a standing
    // dummy connects and a high against a crouching one does. This is the
    // difference between "the dummy cannot be opened up" and "the dummy is
    // holding one guard". It was called Adaptive and behaved as a second name
    // for All; All keeps switching lane per contact, which is the behaviour
    // that was already right.
    StanceOnly,
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
    // 214D. The guard-cancel offer only runs for a route whose COMMAND matched
    // this frame - Entity_ProcessCommandMatches gates it on the route byte at
    // +1675 - and command 26 is the 214D that opens route 23. The offer then
    // consumes command 26 as it queues action 49 (52 airborne), which is what
    // carries the +1940 & 0x100 that sub_4A5C20 requires. So the motion is the
    // mechanic, and the +823 stock is a second gate on top of it.
    // Category 5 only.
    AbsoluteDefence,
    // The same mechanic on the other branch: D simply held, which makes +78
    // read 0 inside blockstun and sends it to the "or 100 meter" arm. Weaker,
    // but it is what an untimed press actually does. Category 4 only.
    PushAwayMetered,
    // The other half of the dodge. Entity_UpdateAction_Attacks picks action
    // 60/62 when BACK is held and 59/61 when it is not, so D alone out of
    // blockstun rolls forward instead of back. Category 6 only.
    DodgeForward,
    // The other half of the counter: 4D rather than 6D. The resolver only reads
    // +1940 & 0x10, so which of the two the character performs is purely a
    // question of which direction is held with D. Category 1 only.
    GuardCounterBack,
    // Absolute defence taken BEFORE the first hit instead of as a cancel out of
    // blocking it. The engine has both entries and they share one gate, the
    // +823 charge: sub_522F30 queues action 49 when command 26 matches with a
    // charge in hand, and Entity_UpdateAction_Attacks queues the same action out
    // of blockstun with no command at all. So this one performs the 214D, and
    // AbsoluteDefence lets the blockstun cancel do it. Category 5 only.
    AbsoluteDefenceFirst,
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

/// True for responses whose window must be open before the hit lands.
bool ResponseArmsBeforeContact(DefensiveResponse response);

// True for the three mechanics the engine arms from a per-character routine at
// the tail of Entity_ProcessCommandMatches - just parry, repel and push away.
// Each opens a window the defender must already hold when the hit arrives, so
// the mod hooks that routine and arms through the engine's own setters instead
// of simulating the input that would have opened it. Everything else here is a
// guard CANCEL performed with live buttons, which no hook can stand in for.
bool ResponseArmedByNativeHook(DefensiveResponse response);
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
    CounterBack,     // D + BACK, the same counter on its other side
    DodgeForwardPress, // D with no direction, which rolls forward
    ArmGuardState,   // run 214D, the category-5 counter-guard motion
    DownTap,         // 2 from neutral, the low half of the category-3 parry
};

const char* DefenseInputKindLabel(DefenseInputKind kind);

struct DefenseDriveSample {
    uint8_t parryWindow = 0xFF;   // +1965, 0xFF = idle
    int32_t reactionState = -1;   // +1980, -1 = no reaction armed
    uint32_t actionId = 0;
    uint16_t meter = 0;
    bool airborne = false;
    // An attack box is live RIGHT NOW. Correct for anything that reacts to a
    // hit; useless for anything that has to be armed before one.
    bool threatArmed = false;
    // Records until the attacker's own animation reaches a frame that carries
    // an attack box, read straight out of its collision table. 0 means a box is
    // out now, -1 that none appears inside the lookahead. This is the only
    // signal available BEFORE the hit that says one is coming.
    int16_t recordsToAttack = -1;
    // The attacker's command-route vector is closed, so it is committed to
    // something. Catches a move whose very first record already carries the box.
    bool attackerCommitted = false;
    // The native arming hook is opening this mechanic's window directly, so the
    // driver must not also feed it an input - the tap would drop the guard and
    // walk the dummy forward for nothing.
    bool nativeArmActive = false;
    bool actionable = false;      // engine would accept an ordinary input now
    // +823, the category-5 stock. It is what the engine's own blockstun cancel
    // tests - NOT a "the state is already up" flag, which is how it was read.
    bool counterGuardStock = false;
    // Which lane the incoming attack demands. The category-3 parry is 6 against
    // high and mid but 2 against low, so the driver has to know which is coming.
    GroundGuardClass threatClass = GroundGuardClass::None;
};

struct DefenseDriveState {
    ParryInputState parry{};
    bool tapPhase = false;        // alternates the neutral / press frames
    bool pushPhase = false;       // alternates the D press so each one is an edge
    bool counterFired = false;    // one counter per threat, not a held direction
    bool dodgePhase = false;      // alternates the D press so each one is an edge
    // 214D is a motion, not a press, so the driver walks it a frame at a time
    // and loops: a command that matched a frame too early has been consumed.
    int8_t armStep = -1;          // -1 idle, else the step being fed
};

// An attack is on its way. Parry and repel both open a window AHEAD of the hit
// (repel's is 24 frames from a neutral tap), so keying them off threatArmed - a
// box that is already out - produced the input a frame or more too late unless
// the move happened to have long active frames. A live box, a box the
// attacker's own animation is about to reach, or a closed command route all
// mean the same thing to a mechanic that has to be armed in advance.
bool ThreatIsIncoming(const DefenseDriveSample& sample);

// Frames in the 214D motion. Matches the game's own command table entry for
// it (0x723480 cmd 26: 2, 1, 4 with the D gate).
constexpr int kGuardStateMotionFrames = 3;

DefenseInputKind EvaluateDefenseInput(DefensiveResponse response,
                                      const DefenseDriveSample& sample,
                                      DefenseDriveState& state);

// Dodge is a guard cancel: D during blockstun with the meter to pay for it.
// Entity_UpdateAction_Attacks offers it from all six blockstun states, and its
// condition reads the DERIVED D word (+78), i.e. a fresh press - so a D simply
// held from the frame blockstun began never re-arms if the first press did not
// take.
constexpr uint16_t kDodgeMeterCost = 500;
bool DodgeWindowOpen(uint32_t actionId, uint16_t meter);

// True for mechanics the engine only offers *out of blockstun*, so a dummy that
// is not blocking can never perform them however hard it presses. Selecting one
// therefore has to imply guarding.
bool ResponseRequiresBlockstun(DefensiveResponse response);

// True for mechanics that HAVE a blockstun branch, whether or not they need
// one. The driver has to keep feeding these while the dummy is in blockstun
// even with no new threat detected, because that is where their cancel is
// offered from. Absolute defence is the case that separates the two: 214D works
// from neutral as well, so it must not force a guard - but it still has to be
// fed through blockstun to reach the cancel.
bool ResponseUsesBlockstunCancel(DefensiveResponse response);

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
    // Control swap used to appear here as a refusal. It no longer does: the
    // dummy side follows the swap, so a swapped match has a dummy the mod is
    // driving exactly as before, just on the other entity.
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

// Whether this policy will cover the lane this attack demands. Only StanceOnly
// ever says no: it holds the stance's own lane instead of switching to
// whatever is incoming.
bool PolicyAllowsLane(BlockPolicy policy, uint32_t attackMask, bool preferCrouch);

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
