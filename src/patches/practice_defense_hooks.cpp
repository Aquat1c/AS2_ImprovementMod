#include "patches/practice_defense_hooks.h"

#include "core/anim_box.h"
#include "core/game_state.h"
#include "core/mod_main.h"
#include "patches/memory_utils.h"
#include "patches/practice_recovery_hooks.h"
#include "training/action_state_classifier.h"
#include "training/native_recovery.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace Training;

namespace {

// --- Native signatures ---------------------------------------------------
// (attackerCtx, sourceObject, contactPos, attackerFacing, payloadAddress).
// The normal-hit fallback takes a sixth argument the guard resolver does not:
// the "other player also aligned" flag from the direct/grab phases, 0 from the
// summon phase. Getting that arity wrong would feed it a garbage sixth value.
typedef int  (__cdecl* DefenseResolve_t)(int, int, int16_t*, char, int);
typedef int  (__cdecl* NormalHit_t)(int, int, int16_t*, char, int, char);
typedef int  (__cdecl* GrabPhase_t)(int);
typedef char (__cdecl* DamagePhase_t)(int);
typedef int  (__cdecl* SummonPhase_t)(int);

DefenseResolve_t g_origOrdinaryGuard = nullptr;
NormalHit_t      g_origNormalHit = nullptr;
GrabPhase_t      g_origGrabPhase = nullptr;
DamagePhase_t    g_origDamagePhase = nullptr;
SummonPhase_t    g_origSummonPhase = nullptr;

bool g_installed = false;
bool g_ordinaryGuardHooked = false;
char g_installError[128] = {};

PracticeDefenseConfig g_config{};
PracticeDefenseTelemetry g_telemetry{};

FrameGuardPlan g_plan{};
AutoBlockSequenceState g_sequence{};
FrameContactOutcome g_outcome{};

// Epoch, not the sim frame counter: the plan is rebuilt on the first collision
// phase of a tick regardless of whether the counter moved.
uint32_t g_phaseEpoch = 1;
uint32_t g_planEpoch = 0;
uint32_t g_lastProcessedSimFrame = 0;

DefenseDriveState g_defenseDrive{};
DefenseInputKind g_defenseInput = DefenseInputKind::None;

SemanticDirection g_anticipatoryGuard = SemanticDirection::Neutral;
uint32_t g_anticipatoryMask = 0;
int8_t g_facingAtInput = 0;
bool g_inputPrearmed = false;

// Detects the "hit on active frame 1, block on active frame 2" signature. The
// epoch, not the sim-frame counter, is what reliably advances once per
// collision tick.
uint32_t g_lastHitThreatId = 0;
uint32_t g_lastHitFrame = 0xFFFFFFFFu;
uint32_t g_lastHitEpoch = 0;
bool g_lastHitWantedBlock = false;

constexpr int kMaxScannedThreats = 24;

struct ScannedThreat {
    ThreatKey key{};
    uint32_t attackMask = 0;
    bool overlapsNow = false;
    int distance = 0;        // |x| to the dummy, for anticipatory selection
};

ScannedThreat g_scanned[kMaxScannedThreats];
int g_scannedCount = 0;
int g_overlappingCount = 0;

bool g_anticipatoryWantsGuard = false;

// +1944 carries the character-defense / guard-point / normal-hit result. It is
// compared across the tick so a stale value cannot be counted twice.
uint32_t g_defenderResultBefore = 0;

// --- Small helpers -------------------------------------------------------

uintptr_t DummyEntity() {
    const int player = (g_config.dummyPlayer == 0) ? 0 : 1;
    return GetEntityBase(player);
}

uintptr_t AttackerEntity() {
    const int player = (g_config.dummyPlayer == 0) ? 1 : 0;
    return GetEntityBase(player);
}

// The mechanic the dummy would actually attempt, after mapping Native onto its
// category and refusing anything the category cannot perform.
DefensiveResponse EffectiveResponse(int category) {
    DefensiveResponse response = g_config.response;
    if (response == DefensiveResponse::CharacterNative) {
        response = ResponseForCategory(category);
    }
    if (!ResponseSupportedByCategory(response, category)) {
        response = DefensiveResponse::NormalGuard;
    }
    return response;
}

bool DefensiveResponseSelected() {
    if (g_config.response == DefensiveResponse::NormalGuard) {
        return false;
    }
    return EffectiveResponse(PracticeDefense_DummyDefenseCategory()) !=
           DefensiveResponse::NormalGuard;
}

// Dodge and push-away are guard cancels: the engine only offers them out of
// blockstun, so a dummy that never blocks can never perform them no matter what
// it presses. Selecting one implies guarding, which is why the policy is
// promoted rather than left Off - otherwise the setting deadlocks against
// itself, waiting for a blockstun state that nothing will ever produce.
BlockPolicy EffectiveBlockPolicy() {
    if (g_config.policy != BlockPolicy::Off) {
        return g_config.policy;
    }
    return ResponseRequiresBlockstun(EffectiveResponse(PracticeDefense_DummyDefenseCategory()))
               ? BlockPolicy::All
               : BlockPolicy::Off;
}

bool PracticeGateOpen() {
    if (!g_config.enabled) return false;
    // The defensive-response driver is built in BuildFramePlan, so bailing here
    // on the block policy alone meant the whole Defense row did nothing unless
    // auto-block happened to be on as well. They are separate settings: a
    // selected mechanic opens the gate on its own. Forcing a block stays gated
    // on the policy further down (g_plan.policyWantsBlock), so nothing starts
    // blocking that the player did not ask for.
    if (g_config.policy == BlockPolicy::Off && !DefensiveResponseSelected()) return false;
    if (GetGameType() != GAMETYPE_TRAINING) return false;
    if (GetGameMode() != MODE_MATCH) return false;
    return true;
}

uint32_t ReadSimFrame() {
    return ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
}

bool ReadAnimFrameBytes(uintptr_t entityBase, uint32_t animIdx, uint8_t* out) {
    if (!entityBase || !out) return false;
    const uintptr_t base = entityBase + ENTITY_OFF_ANIM_DATA +
                           (uintptr_t)ANIM_DATA_STRIDE * animIdx;
    return CopyMemorySafe(out, reinterpret_cast<const void*>(base), ANIM_DATA_STRIDE);
}

// dword_73E070 is indexed by the id at *(entity+0) + 176, not by entity+176.
uint32_t ReadCharacterId(uintptr_t entityBase) {
    const uint32_t block = ReadMemory<uint32_t>(entityBase);
    if (!block) {
        return 0;
    }
    return ReadMemory<uint32_t>((uintptr_t)block + 176);
}

As2::BoxOrigin EntityOrigin(uintptr_t entityBase) {
    As2::BoxOrigin origin{};
    origin.worldX = ReadMemory<int16_t>(entityBase + ENTITY_OFF_X_POS);
    origin.worldY = ReadMemory<int16_t>(entityBase + ENTITY_OFF_Y_POS);
    origin.facing = ReadMemory<int8_t>(entityBase + ENTITY_OFF_FACING);
    return origin;
}

// Mirrors the native "any attack box overlaps any target box" scan, including
// the contact-override shortcut that bypasses box geometry entirely.
bool BoxSetsOverlap(const uint8_t* attackFrame, int attackOffset, const As2::BoxOrigin& attackOrigin,
                    const uint8_t* targetFrame, int targetOffset, const As2::BoxOrigin& targetOrigin,
                    bool contactOverride) {
    for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; ++i) {
        const As2::AnimBox atk = As2::DecodeAnimBox(attackFrame, attackOffset + i * HURTBOX_ENTRY_SIZE);
        if (!As2::AnimBoxValid(atk) && !contactOverride) {
            continue;
        }
        for (int j = 0; j < HURTBOX_COUNT_PER_FRAME; ++j) {
            const As2::AnimBox tgt = As2::DecodeAnimBox(targetFrame, targetOffset + j * HURTBOX_ENTRY_SIZE);
            if (!As2::AnimBoxValid(tgt) && !contactOverride) {
                continue;
            }
            if (contactOverride) {
                return true;
            }
            if (As2::AnimBoxesOverlap(attackOrigin, atk, targetOrigin, tgt)) {
                return true;
            }
        }
    }
    return false;
}

bool HitActiveArmed(uint8_t hitActive) {
    return hitActive != 0 && hitActive != 0xFF;
}

void RecordScannedThreat(const ThreatKey& key, uint32_t attackMask, bool overlapsNow, int distance) {
    for (int i = 0; i < g_scannedCount; ++i) {
        if (g_scanned[i].key.payloadAddress == key.payloadAddress) {
            // Same payload reached through two box sets.
            g_scanned[i].overlapsNow = g_scanned[i].overlapsNow || overlapsNow;
            return;
        }
    }
    if (g_scannedCount >= kMaxScannedThreats) {
        return;
    }
    g_scanned[g_scannedCount].key = key;
    g_scanned[g_scannedCount].attackMask = attackMask;
    g_scanned[g_scannedCount].overlapsNow = overlapsNow;
    g_scanned[g_scannedCount].distance = distance;
    g_scannedCount++;
}

int AbsDistanceX(uintptr_t dummy, int16_t worldX) {
    const int d = (int)ReadMemory<int16_t>(dummy + ENTITY_OFF_X_POS) - (int)worldX;
    return d < 0 ? -d : d;
}

// --- Exact current-frame contact scan ------------------------------------

// An armed threat is recorded whether or not it overlaps yet: the plan uses only
// the overlapping ones, while the anticipatory input path uses the nearest armed
// one so the dummy is already holding guard before geometry connects.
void ScanDirectThreat(uintptr_t attacker, uintptr_t dummy) {
    const uint8_t attackState = ReadMemory<uint8_t>(attacker + ENTITY_OFF_ATTACK_STATE);
    if (attackState != 1) {
        return;
    }
    const uint8_t hitActive = ReadMemory<uint8_t>(attacker + ENTITY_OFF_HIT_ACTIVE);
    if (!HitActiveArmed(hitActive)) {
        return;
    }

    const uint32_t attackMask = ReadMemory<uint32_t>(attacker + ENTITY_OFF_ATTACK_TYPE);
    const bool contactOverride = (attackMask & ATTACK_FLAG_CONTACT_OVERRIDE) != 0;

    // The +1932 gate refuses contact unless the attack carries the bypass bit.
    const bool gated = ReadMemory<uint16_t>(dummy + ENTITY_OFF_MAX_HIT_RAW_A) != 0 &&
                       (attackMask & ATTACK_FLAG_BYPASS_DEF_1932) == 0;

    uint8_t attackerFrame[ANIM_DATA_STRIDE];
    uint8_t dummyFrame[ANIM_DATA_STRIDE];
    const uint32_t attackerAnim = ReadMemory<uint32_t>(attacker + ENTITY_OFF_ANIM_INDEX);
    const uint32_t dummyAnim = ReadMemory<uint32_t>(dummy + ENTITY_OFF_ANIM_INDEX);
    if (!ReadAnimFrameBytes(attacker, attackerAnim, attackerFrame) ||
        !ReadAnimFrameBytes(dummy, dummyAnim, dummyFrame)) {
        return;
    }

    const As2::BoxOrigin attackerOrigin = EntityOrigin(attacker);
    const As2::BoxOrigin dummyOrigin = EntityOrigin(dummy);

    // The damage phase tests the extended hurtboxes, the grab phase the primary
    // set; either one reaching the dummy is one contact for this payload.
    const bool overlaps = !gated &&
        (BoxSetsOverlap(attackerFrame, ANIM_HITBOX_OFFSET, attackerOrigin,
                        dummyFrame, ANIM_EXT_HURTBOX_OFFSET, dummyOrigin, contactOverride) ||
         BoxSetsOverlap(attackerFrame, ANIM_HITBOX_OFFSET, attackerOrigin,
                        dummyFrame, ANIM_HURTBOX_OFFSET, dummyOrigin, contactOverride));

    ThreatKey key{};
    key.source = ThreatSource::DirectPlayer;
    key.owner = (uint8_t)((g_config.dummyPlayer == 0) ? 1 : 0);
    key.hitDefSlot = 0xFFFFu;
    key.objectId = ReadMemory<uint32_t>(attacker + ENTITY_OFF_ACTION_ID);
    key.payloadAddress = attacker + ENTITY_OFF_ATTACK_STATE;
    RecordScannedThreat(key, attackMask, overlaps,
                        AbsDistanceX(dummy, ReadMemory<int16_t>(attacker + ENTITY_OFF_X_POS)));
}

void ScanHitDefThreats(uintptr_t dummy) {
    const uint8_t dummyOwner = (uint8_t)((g_config.dummyPlayer == 0) ? 0 : 1);

    // The summon path has no bypass bit, so the gate refuses every HitDef.
    const bool gated = ReadMemory<uint16_t>(dummy + ENTITY_OFF_MAX_HIT_RAW_A) != 0;

    uint8_t dummyFrame[ANIM_DATA_STRIDE];
    const uint32_t dummyAnim = ReadMemory<uint32_t>(dummy + ENTITY_OFF_ANIM_INDEX);
    if (!ReadAnimFrameBytes(dummy, dummyAnim, dummyFrame)) {
        return;
    }
    const As2::BoxOrigin dummyOrigin = EntityOrigin(dummy);

    for (int slot = 0; slot < SUMMON_MAX_SLOTS; ++slot) {
        const uintptr_t entry = ADDR_SUMMON_ARRAY + (uintptr_t)slot * SUMMON_ENTRY_SIZE;
        const uint32_t id = ReadMemory<uint32_t>(entry + HITDEF_OFF_ID);
        if (id == 0) {
            break;  // the native scan stops at the first free slot
        }
        const uint8_t owner = ReadMemory<uint8_t>(entry + HITDEF_OFF_OWNER);
        if (owner == 0xFF || owner == dummyOwner) {
            continue;
        }
        if (ReadMemory<uint8_t>(entry + HITDEF_OFF_ATTACK_STATE) != 1) {
            continue;
        }
        const uint32_t attackMask = ReadMemory<uint32_t>(entry + HITDEF_OFF_ATTACK_MASK);
        if ((attackMask & ATTACK_FLAG_HITDEF_NO_PLAYER) != 0) {
            continue;
        }
        if (!HitActiveArmed(ReadMemory<uint8_t>(entry + HITDEF_OFF_HIT_ACTIVE))) {
            continue;
        }

        const uint16_t animIdx = ReadMemory<uint16_t>(entry + HITDEF_OFF_ANIM_FRAME_IDX);
        if (animIdx == 0xFFFFu) {
            continue;
        }
        const uint8_t animOwner = ReadMemory<uint8_t>(entry + HITDEF_OFF_ANIM_OWNER);

        uint8_t hitDefFrame[ANIM_DATA_STRIDE];
        if (!ReadAnimFrameBytes(GetEntityBase(animOwner == 0 ? 0 : 1), animIdx, hitDefFrame)) {
            continue;
        }

        // The HitDef's own world position, facing and frame index, never the
        // owner's: the owner may be airborne, recovered, or across the screen.
        As2::BoxOrigin origin{};
        origin.worldX = ReadMemory<int16_t>(entry + HITDEF_OFF_X);
        origin.worldY = ReadMemory<int16_t>(entry + HITDEF_OFF_Y);
        origin.facing = ReadMemory<int8_t>(entry + HITDEF_OFF_FACING);

        const bool contactOverride = (attackMask & ATTACK_FLAG_CONTACT_OVERRIDE) != 0;
        const bool overlaps = !gated &&
            BoxSetsOverlap(hitDefFrame, ANIM_HITBOX_OFFSET, origin,
                           dummyFrame, ANIM_HURTBOX_OFFSET, dummyOrigin, contactOverride);

        ThreatKey key{};
        key.source = ThreatSource::HitDef;
        key.owner = owner;
        key.hitDefSlot = (uint16_t)slot;
        key.objectId = id;
        key.payloadAddress = entry + HITDEF_OFF_PAYLOAD;
        RecordScannedThreat(key, attackMask, overlaps, AbsDistanceX(dummy, origin.worldX));
    }
}

// Nearest armed threat, used only for anticipatory posture.
const ScannedThreat* NearestArmedThreat() {
    const ScannedThreat* best = nullptr;
    for (int i = 0; i < g_scannedCount; ++i) {
        if (g_scanned[i].overlapsNow) {
            return &g_scanned[i];
        }
        if (!best || g_scanned[i].distance < best->distance) {
            best = &g_scanned[i];
        }
    }
    return best;
}

// --- Attack prediction ---------------------------------------------------

// The entity carries its character's whole collision table inline at
// +ENTITY_OFF_ANIM_DATA, and the records of one action are consecutive, so the
// frames the attacker has not reached yet are readable right now. That is the
// only signal available BEFORE a hit that says one is coming - and every
// mechanic that has to be armed ahead of contact needs exactly that.
constexpr int kAttackLookaheadRecords = 8;

// The array ends where the voice bookkeeping begins; reading past that would be
// another struct's bytes reinterpreted as boxes.
constexpr uint32_t kAnimRecordLimit =
    (uint32_t)((ENTITY_OFF_VOICE_BOOKKEEPING - ENTITY_OFF_ANIM_DATA) / ANIM_DATA_STRIDE);

bool FrameHasAttackBox(const uint8_t* frame) {
    for (int i = 0; i < HURTBOX_COUNT_PER_FRAME; ++i) {
        const As2::AnimBox box =
            As2::DecodeAnimBox(frame, ANIM_HITBOX_OFFSET + i * HURTBOX_ENTRY_SIZE);
        if (As2::AnimBoxValid(box)) {
            return true;
        }
    }
    return false;
}

// True when the attacker's current action carries an attack payload at all.
//
// +1740 is the attack mask, and Entity_ResetHitData zeroes it on entry to every
// ordinary action - stand and both walks all call it - so a non-zero mask means
// this action is an attack. It is written when the action begins, well before
// the box goes live, which is exactly what a mechanic that has to be armed
// early needs. The command-route vector is NOT a substitute: walking closes it
// too, which is what made the dummy answer a walk as though it were a hit.
bool AttackerCommitted(uintptr_t attacker) {
    if (!attacker) {
        return false;
    }
    if (ReadMemory<uint32_t>(attacker + ENTITY_OFF_ATTACK_TYPE) != 0) {
        return true;
    }
    if (ReadMemory<uint8_t>(attacker + ENTITY_OFF_ATTACK_STATE) != 0) {
        return true;
    }
    return Training::IsSharedAttackAction(
        ReadMemory<uint32_t>(attacker + ENTITY_OFF_ACTION_ID));
}

// Records until the attacker's animation reaches one that carries an attack
// box. 0 = a box is out on the current record, -1 = none inside the lookahead.
// Records are phases, not ticks, so a positive answer is a lower bound on the
// frames left - which is the safe direction to be wrong in.
//
// The scan walks the character's collision table forward, and that table is
// shared by every action, so records past the end of the current one belong to
// whatever happens to sit next in the file. Scanning from a walk therefore
// found an attack box that was never coming. It only runs once the attacker is
// committed to an attack, which is what keeps the answer about THIS action.
int RecordsUntilAttackBox(uintptr_t attacker) {
    if (!attacker || !AttackerCommitted(attacker)) {
        return -1;
    }
    const uint32_t animIdx = ReadMemory<uint32_t>(attacker + ENTITY_OFF_ANIM_INDEX);
    if (animIdx >= kAnimRecordLimit) {
        return -1;
    }
    uint8_t frame[ANIM_DATA_STRIDE];
    for (int k = 0; k <= kAttackLookaheadRecords; ++k) {
        const uint32_t index = animIdx + (uint32_t)k;
        if (index >= kAnimRecordLimit) {
            break;
        }
        if (!ReadAnimFrameBytes(attacker, index, frame)) {
            break;
        }
        if (FrameHasAttackBox(frame)) {
            return k;
        }
    }
    return -1;
}

// --- Native defensive-window arming --------------------------------------
//
// Just parry, repel and push away are not reactions to a hit - they are windows
// the defender opens BEFORE one, from a per-character routine at the tail of
// Entity_ProcessCommandMatches. A dummy driven by simulated input can only ever
// open them after it has seen a hitbox, which is already too late: repel wants a
// forward tap out of neutral, and by the time a box exists the tap had to have
// happened. Hooking those three routines and arming through the engine's own
// setters puts the dummy in exactly the state a perfectly timed human input
// would have produced - no input to simulate, no guard dropped, no drift.

typedef int      (__cdecl* CheckArm_t)(int);
typedef int      (__cdecl* SetHitState1965_t)(int, char, int);
typedef int      (__cdecl* SetHitState1973_t)(int, char, char);
typedef int      (__cdecl* SetHitReaction_t)(int, int);
typedef int      (__cdecl* UpdateHitReaction_t)(int);
typedef uint8_t* (__cdecl* SetHitFlag1993_t)(uint8_t*, char);
typedef char     (__cdecl* SetHitByte1949_t)(int, char);
typedef int      (__cdecl* SetHitFlags1996_t)(int, char, char);
typedef void     (__cdecl* UpdateActionAttacks_t)(int, uint32_t*);

UpdateActionAttacks_t g_origUpdateActionAttacks = nullptr;
// Set by whoever owns the Entity_ProcessCommandMatches hook.
bool g_counterGuardRouteAvailable = false;
uint32_t g_pendingBeforeCommandMatches = 0;
CheckArm_t g_origCheckHitState = nullptr;
CheckArm_t g_origCheckGuardState = nullptr;
CheckArm_t g_origCheckAirTech = nullptr;
bool g_armHooksInstalled = false;

const SetHitState1965_t   Native_SetParryWindow =
    reinterpret_cast<SetHitState1965_t>(ADDR_ENTITY_SET_HIT_STATE_1965);
const SetHitReaction_t    Native_SetHitReaction =
    reinterpret_cast<SetHitReaction_t>(ADDR_ENTITY_SET_HIT_REACTION);
const UpdateHitReaction_t Native_UpdateHitReaction =
    reinterpret_cast<UpdateHitReaction_t>(ADDR_ENTITY_UPDATE_HIT_REACTION);
const SetHitFlag1993_t    Native_SetPushAwayFlag =
    reinterpret_cast<SetHitFlag1993_t>(ADDR_ENTITY_SET_HIT_FLAG_1993);
const SetHitState1973_t   Native_SetGuardPosture =
    reinterpret_cast<SetHitState1973_t>(ADDR_ENTITY_SET_HIT_STATE_1973);
const SetHitByte1949_t    Native_SetDefenceAllowed =
    reinterpret_cast<SetHitByte1949_t>(ADDR_ENTITY_SET_HIT_BYTE_1949);
const SetHitFlags1996_t   Native_SetPushAwayPosture =
    reinterpret_cast<SetHitFlags1996_t>(ADDR_ENTITY_SET_HIT_FLAGS_1996);

// Last decoded lane of the incoming attack, so the repel arm can pick its low
// branch. One tick stale - the arming routines run before the collision phases.
GroundGuardClass g_lastThreatClass = GroundGuardClass::None;

// Last sample built by the frame plan, so the command-time hooks can ask the
// same prediction the driver uses instead of recomputing it.
DefenseDriveSample g_lastDrive{};

// Drained by the practice runtime once per success.
bool g_defensiveSuccessPending = false;
uint32_t g_defensiveSuccessFrame = 0;
uint32_t g_defensiveSuccessResult = 0;

PracticeDefenseArmState g_arm{};

// The mechanic this frame, or NormalGuard when the dummy is not the mod's.
DefensiveResponse ArmTargetResponse(uintptr_t entity) {
    if (!g_armHooksInstalled || !PracticeGateOpen()) {
        return DefensiveResponse::NormalGuard;
    }
    if (!entity || entity != DummyEntity()) {
        return DefensiveResponse::NormalGuard;
    }
    return EffectiveResponse(PracticeDefense_DummyDefenseCategory());
}

// Re-applied every frame, so the counter and the log line follow real state
// transitions rather than the reapplication itself.
void NoteArm(const char* mechanic, uintptr_t entity, int detail) {
    g_arm.lastArmFrame = ReadSimFrame();
    g_arm.arms++;
    // Every gate term the contact-time resolver will test is on the line, so an
    // arm that later fails to resolve can be compared against the resolve trace
    // without replaying anything.
    LOG_INFO("[Defence] armed %s frame=%u detail=%d action=%u | allow1949=%u "
             "kind1989=%u react1980=%d flags1940=0x%04X parry1965=0x%02X "
             "down1973=%u back1974=%u push1993=0x%02X air=%u",
             mechanic, g_arm.lastArmFrame, detail,
             (unsigned)ReadMemory<uint32_t>(entity + ENTITY_OFF_ACTION_ID),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_DEFENCE_ALLOWED),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_HIT_REACTION_KIND),
             (int)ReadMemory<int32_t>(entity + ENTITY_OFF_HIT_REACTION_STATE),
             (unsigned)ReadMemory<uint32_t>(entity + ENTITY_OFF_MAX_HIT_FLAGS),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_PARRY_WINDOW),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_PARRY_STANCE),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_PARRY_GUARD_HELD),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_PUSH_AWAY_ARMED),
             (unsigned)ReadMemory<uint8_t>(entity + ENTITY_OFF_AIRBORNE));
}

// Every category's window path is gated on +1949 - the action the defender is
// in has to permit its defensive mechanic at all. Action handlers set it after
// Entity_ResetHitData and NOTHING in the game ever writes 0, so an action that
// simply does not set it leaves the mechanic switched off with the window
// perfectly armed. That is exactly what the 02-32-31 run showed: the repel
// window re-armed every 24 frames and every contact still resolved as a hit.
//
// It is not forced during hitstun or a knockdown, where the engine would never
// have offered the defence in the first place. Blockstun is left in: parry and
// push away are both meant to work from there.
bool ForceDefenceAllowed(uintptr_t entity, const char* mechanic) {
    const uint32_t action = ReadMemory<uint32_t>(entity + ENTITY_OFF_ACTION_ID);
    if (Training::IsHitstun(action) || Training::IsKnockdownOrLaunch(action)) {
        return false;
    }
    // Committed to its own move - startup included, and character specials
    // included, which only the route vector can see. Blockstun and proximity
    // guard are explicitly still defensible: parry and push away are both meant
    // to work from there.
    if (!Training::IsBlockstun(action) && !Training::IsProximityGuard(action) &&
        !NeutralRouteOpen(PracticeRecovery_ReadSample(entity))) {
        return false;
    }
    if (ReadMemory<uint8_t>(entity + ENTITY_OFF_ATTACK_STATE) == 1) {
        return false;
    }
    if (ReadMemory<uint8_t>(entity + ENTITY_OFF_DEFENCE_ALLOWED) == 1) {
        return true;
    }
    Native_SetDefenceAllowed((int)entity, 1);
    g_arm.defenceAllowedForced++;
    LOG_INFO("[Defence] +1949 was 0 for %s, forced (action=%u) - the action does not "
             "permit the mechanic on its own", mechanic, (unsigned)action);
    return true;
}

// True when the attack demands the crouch lane, so the low branch of a mechanic
// that has one is the branch that will fire.
bool ThreatIsLow() {
    return g_lastThreatClass == GroundGuardClass::CrouchOnly;
}

// Entity_CheckHitState's own kind/class split, applied without the BACK edge.
//
// The window alone is not enough: sub_4A50A0 resolves the stand branch only
// when +1974 (BACK held) is 1 with +1973 (DOWN held) clear, and the low branch
// only when both are 1. Entity_CheckHitState writes that pair from the live
// input every frame, so a dummy that is not holding guard has a perfectly armed
// window and no branch to take. Writing the posture the incoming attack needs,
// through the engine's own setter, is the other half of the arm.
void ForceParryArm(uintptr_t entity) {
    if (ArmTargetResponse(entity) != DefensiveResponse::JustParry) {
        return;
    }
    if (!ForceDefenceAllowed(entity, "just_parry")) {
        return;
    }
    const bool airborne = ReadMemory<uint8_t>(entity + ENTITY_OFF_AIRBORNE) == 1;
    const bool low = !airborne && ThreatIsLow();
    Native_SetGuardPosture((int)entity, low ? 1 : 0, 1);

    const uint32_t action = ReadMemory<uint32_t>(entity + ENTITY_OFF_ACTION_ID);
    char kind = PARRY_KIND_STAND;
    int cls = 1;
    if (action == 64 || action == 65 || action == 67 ||
        action == 68 || action == 70 || action == 71) {
        kind = PARRY_KIND_BLOCKSTUN;
        cls = 3;
    } else if (action >= 34 && action <= 39) {
        kind = PARRY_KIND_CROUCH;
        cls = 2;
    }
    // Applied every frame rather than only when the window reads idle. The
    // engine clears the window between command processing and collision, so an
    // arm that happened once and then trusted its own 24-frame timer was gone
    // by the time the resolver looked at it.
    const bool wasIdle =
        ReadMemory<uint8_t>(entity + ENTITY_OFF_PARRY_WINDOW) == PARRY_WINDOW_IDLE;
    Native_SetParryWindow((int)entity, kind, cls);
    if (wasIdle) {
        g_arm.parryArms++;
        NoteArm("just_parry", entity, kind);
    }
}

// Repel. sub_4A5570 matches the attack's guard lane against the LOW BITS of the
// reaction at +1980 - 5 is 0b101 (stand lane), 6 is 0b110 (crouch lane), 7 is
// 0b111 - so the reaction the engine would have produced depends on which tap
// the player made. It is chosen here from the lane the incoming attack demands.
// Re-arming when that lane changes is free and keeps the window pointed at what
// is actually coming.
void ForceRepelArm(uintptr_t entity) {
    if (ArmTargetResponse(entity) != DefensiveResponse::Repel) {
        return;
    }
    if (!ForceDefenceAllowed(entity, "repel")) {
        return;
    }
    const bool airborne = ReadMemory<uint8_t>(entity + ENTITY_OFF_AIRBORNE) == 1;
    const int desired = airborne ? REPEL_REACTION_AIR
                                 : (ThreatIsLow() ? REPEL_REACTION_LOW : REPEL_REACTION_HIGH);

    // Re-applied every frame, not only when +1980 changes. The 02-44-14 trace
    // showed why: the reaction survived at +1980 but the KIND byte +1989 was
    // back to 0 by the time the resolver ran, and sub_4A5570's first gate reads
    // +1989, so every one of those contacts declined to a plain hit. The one
    // that did repel got there through the other arm of that gate, the 0x43
    // capability flags a crouching defender happens to carry. Arming once and
    // trusting the engine's own 24-frame timer was the whole bug.
    const bool wasArmed =
        (int)ReadMemory<int32_t>(entity + ENTITY_OFF_HIT_REACTION_STATE) == desired &&
        ReadMemory<uint8_t>(entity + ENTITY_OFF_HIT_REACTION_KIND) != 0;
    Native_SetHitReaction((int)entity, desired);
    // The engine only reaches this while the defender stands neutral, and it is
    // what turns the raw reaction into the code +1989 the resolver tests.
    // Skipping it would leave the window armed but half-built.
    Native_UpdateHitReaction((int)entity);
    if (!wasArmed) {
        g_arm.repelArms++;
        NoteArm("repel", entity, desired);
    }
}

// Push away. Entity_CheckAirTech only ever sets flags, so the flag IS the
// mechanic: +1994 picks the free variant over the 100-meter one. The free
// branch is only offered from the actions the engine lists, and the paid branch
// still has to be paid for, so neither is granted where the engine refuses it.
void ForcePushAwayArm(uintptr_t entity) {
    const DefensiveResponse response = ArmTargetResponse(entity);
    const bool perfect = response == DefensiveResponse::PushAwayPerfect;
    if (!perfect && response != DefensiveResponse::PushAwayMetered) {
        return;
    }
    const uint32_t action = ReadMemory<uint32_t>(entity + ENTITY_OFF_ACTION_ID);
    if (perfect) {
        if (!PushAwayFreeWindow(action)) {
            return;
        }
    } else if (!PushAwayFreeWindow(action) &&
               ReadMemory<uint16_t>(entity + ENTITY_OFF_METER) < kPushAwayMeterCost) {
        return;
    }
    if (!ForceDefenceAllowed(entity, perfect ? "push_away_free" : "push_away_metered")) {
        return;
    }
    const bool wasArmed =
        ReadMemory<uint8_t>(entity + ENTITY_OFF_PUSH_AWAY_TIMER) != PUSH_AWAY_TIMER_IDLE &&
        ReadMemory<uint8_t>(entity + ENTITY_OFF_PUSH_AWAY_FREE) == (perfect ? 1 : 0);
    Native_SetPushAwayFlag(reinterpret_cast<uint8_t*>(entity), perfect ? 1 : 0);
    if (!wasArmed) {
        g_arm.pushAwayArms++;
        NoteArm(perfect ? "push_away_free" : "push_away_metered", entity, (int)action);
    }
}

const char* DefenseCategoryName(int category) {
    switch (category) {
        case kDefenseCategoryUnique:    return "guard counter";
        case kDefenseCategoryJustParry: return "just parry";
        case kDefenseCategoryRepel:     return "repel";
        case kDefenseCategoryPushAway:  return "push away";
        case kDefenseCategoryAbsolute:  return "absolute defence";
        case kDefenseCategoryDodge:     return "dodge";
        default:                        return "none";
    }
}

// Everything the menu needs to say what the dummy is set to, whether anything
// is driving it, and whether the window is actually open right now.
void PublishArmState(uintptr_t dummy, int category, DefensiveResponse effective,
                     const DefenseDriveSample& drive) {
    g_arm.installed = g_armHooksInstalled;
    g_arm.category = category;
    g_arm.configured = g_config.response;
    g_arm.effective = effective;
    g_arm.gateOpen = true;   // only reached with the gate open
    g_arm.hookDriven = g_armHooksInstalled && ResponseArmedByNativeHook(effective);
    g_arm.input = g_defenseInput;
    g_arm.parryWindow = drive.parryWindow;
    g_arm.repelReaction = drive.reactionState;
    g_arm.repelTimer = ReadMemory<uint8_t>(dummy + ENTITY_OFF_HIT_REACTION_TIMER);
    g_arm.pushAwayTimer = ReadMemory<uint8_t>(dummy + ENTITY_OFF_PUSH_AWAY_TIMER);
    g_arm.pushAwayFree = ReadMemory<uint8_t>(dummy + ENTITY_OFF_PUSH_AWAY_FREE) != 0;
    g_arm.guardStock = ReadMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE);
    g_arm.recordsToAttack = drive.recordsToAttack;
    g_arm.attackerCommitted = drive.attackerCommitted;
    g_arm.threatArmed = drive.threatArmed;

    // One line whenever what the dummy is set to do actually changes. Without
    // it a log shows only the contacts, and "the menu never applied the
    // setting" reads the same as "the setting applied and did nothing".
    static int s_lastCategory = -1;
    static DefensiveResponse s_lastEffective = DefensiveResponse::Count;
    static DefensiveResponse s_lastConfigured = DefensiveResponse::Count;
    static bool s_lastHookDriven = false;
    if (category != s_lastCategory || effective != s_lastEffective ||
        g_config.response != s_lastConfigured || g_arm.hookDriven != s_lastHookDriven) {
        s_lastCategory = category;
        s_lastEffective = effective;
        s_lastConfigured = g_config.response;
        s_lastHookDriven = g_arm.hookDriven;
        LOG_INFO("[Defence] dummy category=%d(%s) configured=%s effective=%s driver=%s "
                 "policy=%s prefer_crouch=%d",
                 category, DefenseCategoryName(category),
                 DefensiveResponseLabel(g_config.response),
                 DefensiveResponseLabel(effective),
                 g_arm.hookDriven ? "native hook" : "input",
                 BlockPolicyLabel(g_config.policy),
                 g_config.preferCrouch ? 1 : 0);
    }

    // And one whenever the mechanic's own window opens or closes, so the gap
    // between "armed" and "fired" is visible in the log rather than only in the
    // outcome. Sampled once per collision tick, which is where it matters.
    static uint8_t s_lastParry = 0;
    static int32_t s_lastReaction = 0;
    static uint8_t s_lastPush = 0;
    const bool parryOpen = g_arm.parryWindow != PARRY_WINDOW_IDLE;
    const bool repelOpen = g_arm.repelReaction != REPEL_REACTION_IDLE;
    const bool pushOpen = g_arm.pushAwayTimer != PUSH_AWAY_TIMER_IDLE;
    if (parryOpen != (s_lastParry != PARRY_WINDOW_IDLE) ||
        repelOpen != (s_lastReaction != REPEL_REACTION_IDLE) ||
        pushOpen != (s_lastPush != PUSH_AWAY_TIMER_IDLE)) {
        LOG_INFO("[Defence] window frame=%u parry=%s(0x%02X) repel=%s(%d,%uf) "
                 "push=%s(%s) allow1949=%u action=%u threat=%s",
                 g_plan.simFrame,
                 parryOpen ? "open" : "idle", (unsigned)g_arm.parryWindow,
                 repelOpen ? "open" : "idle", (int)g_arm.repelReaction,
                 (unsigned)g_arm.repelTimer,
                 pushOpen ? "open" : "idle", g_arm.pushAwayFree ? "free" : "metered",
                 (unsigned)ReadMemory<uint8_t>(dummy + ENTITY_OFF_DEFENCE_ALLOWED),
                 (unsigned)drive.actionId,
                 GroundGuardClassLabel(drive.threatClass));
    }
    s_lastParry = g_arm.parryWindow;
    s_lastReaction = (int32_t)g_arm.repelReaction;
    s_lastPush = g_arm.pushAwayTimer;
}

// True for the six blockstun states the guard-cancel offer is keyed on.
bool IsBlockstunAction(uint32_t action) {
    return action == 64 || action == 65 || action == 67 ||
           action == 68 || action == 70 || action == 71;
}

// The preemptive counter-guard.
//
// Its engine entry is sub_522F30 / sub_5D99C0 on route slot 0, reached when
// Entity_ProcessCommandMatches finds that route's command matched. Writing that
// route byte is the same kind of arming the parry gets: the mod states that the
// command happened and then touches nothing else - the handler still checks its
// own gates, still consumes command 26, and the 57-frame state that follows is
// entirely the engine's. Feeding 2-1-4-D instead meant the match had to land on
// exactly the right frame and got consumed whether or not a hit came, which is
// what made it wonky.
void CommandMatchesEntry(uintptr_t dummy) {
    if (g_counterGuardRouteAvailable && PracticeGateOpen() && dummy && dummy == DummyEntity() &&
        EffectiveResponse(PracticeDefense_DummyDefenseCategory()) ==
            DefensiveResponse::AbsoluteDefenceFirst) {
        const uint32_t action = ReadMemory<uint32_t>(dummy + ENTITY_OFF_ACTION_ID);
        // Only where a player could have input it: not out of hitstun, not out
        // of the dummy's own move. Blockstun is allowed - the route is lower
        // priority than the blockstun cancel, so whichever the engine prefers
        // wins on its own terms.
        const bool couldInput =
            !Training::IsHitstun(action) && !Training::IsKnockdownOrLaunch(action) &&
            ReadMemory<uint8_t>(dummy + ENTITY_OFF_ATTACK_STATE) != 1 &&
            (Training::IsBlockstun(action) || Training::IsProximityGuard(action) ||
             NeutralRouteOpen(PracticeRecovery_ReadSample(dummy)));

        // Read the prediction fresh. g_lastDrive is built in the collision
        // phase, which is LATER in the tick than command matching, so relying
        // on it here would always be a tick behind.
        const uintptr_t attacker = AttackerEntity();
        const bool incoming = AttackerCommitted(attacker);

        if (couldInput && incoming) {
            // The charge has to be there when sub_522F30 reads it, and it reads
            // it inside the call below. Something clears +823 every frame - the
            // 13-16-19 log grants it on every single collision phase and finds
            // it back at 0 on the next - so checking it first and bailing meant
            // the route byte was never written at all. Grant, then state the
            // command matched, in that order.
            if (ReadMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE) == 0) {
                WriteMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE, 1);
                g_arm.absoluteStockForced++;
            }
            const uintptr_t slot = dummy + ENTITY_OFF_COMMAND_ROUTE_TIMERS +
                                   ENTITY_ROUTE_COUNTER_GUARD;
            if (ReadMemory<uint8_t>(slot) == 0) {
                WriteMemory<uint8_t>(slot, 1);
                g_arm.counterGuardRouteOpened++;
            }
        }
    }

    g_pendingBeforeCommandMatches =
        dummy ? ReadMemory<uint32_t>(dummy + ENTITY_OFF_PENDING_ACTION_1) : 0;
}

// Whether stating the match actually produced the counter guard. Logged on
// change only, so a working run is a couple of lines and a broken one says
// which term the handler refused on.
void CommandMatchesExit(uintptr_t dummy) {
    if (!dummy || g_arm.counterGuardRouteOpened == g_arm.lastRouteLogCount) {
        return;
    }
    g_arm.lastRouteLogCount = g_arm.counterGuardRouteOpened;
    const uint32_t after = ReadMemory<uint32_t>(dummy + ENTITY_OFF_PENDING_ACTION_1);
    LOG_INFO("[Defence] counter-guard route stated frame=%u action=%u pending %u -> %u "
             "stock823=%u (%s)",
             ReadSimFrame(),
             (unsigned)ReadMemory<uint32_t>(dummy + ENTITY_OFF_ACTION_ID),
             (unsigned)g_pendingBeforeCommandMatches, (unsigned)after,
             (unsigned)ReadMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE),
             (after == ACTION_ABSOLUTE_DEFENCE_GROUND ||
              after == ACTION_ABSOLUTE_DEFENCE_AIR)
                 ? "queued the counter guard"
                 : "handler declined");
}

// The guard-cancel offer.
//
// Absolute defence needs no input - the offer's condition is the character's
// category and the stock byte at +823, nothing else - so the only thing the mod
// can supply is the stock, and it has to be there BEFORE the offer reads it.
// Writing it from the collision phase was a whole phase too late in the tick,
// which is why the dummy blocked and never countered.
void __cdecl Hook_UpdateActionAttacks(int entity, uint32_t* pendingAction) {
    const uintptr_t defender = (uintptr_t)entity;
    const uint32_t action = defender ? ReadMemory<uint32_t>(defender + ENTITY_OFF_ACTION_ID) : 0;
    const bool blockstun = IsBlockstunAction(action);
    const bool isDummy = PracticeGateOpen() && defender != 0 && defender == DummyEntity();
    const DefensiveResponse response =
        isDummy ? EffectiveResponse(PracticeDefense_DummyDefenseCategory())
                : DefensiveResponse::NormalGuard;

    if (isDummy && blockstun &&
        (response == DefensiveResponse::AbsoluteDefence ||
         response == DefensiveResponse::AbsoluteDefenceFirst) &&
        ReadMemory<uint8_t>(defender + ENTITY_OFF_DEFENSE_RESOURCE) == 0) {
        WriteMemory<uint8_t>(defender + ENTITY_OFF_DEFENSE_RESOURCE, 1);
        g_arm.absoluteStockForced++;
        LOG_INFO("[Defence] absolute-defence stock granted at the offer (action=%u frame=%u)",
                 (unsigned)action, ReadSimFrame());
    }

    const uint32_t before = pendingAction ? pendingAction[0] : 0;
    if (g_origUpdateActionAttacks) {
        g_origUpdateActionAttacks(entity, pendingAction);
    }

    // Only blockstun frames say anything, and only for the mod's own dummy, so
    // this is a handful of lines per string rather than one per frame.
    if (isDummy && blockstun) {
        const uint32_t after = pendingAction ? pendingAction[0] : 0;
        if (after != before) {
            g_arm.guardCancelsOffered++;
            LOG_INFO("[Defence] guard cancel offered: action %u -> %u (from %u) frame=%u",
                     (unsigned)action, (unsigned)after, (unsigned)before, ReadSimFrame());
        } else {
            static uint32_t s_lastRefusalFrame = 0xFFFFFFFFu;
            const uint32_t frame = ReadSimFrame();
            if (frame != s_lastRefusalFrame) {
                s_lastRefusalFrame = frame;
                LOG_INFO("[Defence] guard cancel NOT offered frame=%u action=%u response=%s "
                         "category=%d stock823=%u meter=%u d_edge=%u back=%u",
                         frame, (unsigned)action,
                         DefensiveResponseLabel(response),
                         PracticeDefense_DummyDefenseCategory(),
                         (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_DEFENSE_RESOURCE),
                         (unsigned)ReadMemory<uint16_t>(defender + ENTITY_OFF_METER),
                         (unsigned)ReadMemory<uint16_t>(defender + 78),
                         (unsigned)ReadMemory<uint16_t>(defender + ENTITY_OFF_INPUT_BACK));
            }
        }
    }
}

int __cdecl Hook_CheckHitState(int entity) {
    const int result = g_origCheckHitState ? g_origCheckHitState(entity) : entity;
    ForceParryArm((uintptr_t)entity);
    return result;
}

int __cdecl Hook_CheckGuardState(int entity) {
    const int result = g_origCheckGuardState ? g_origCheckGuardState(entity) : entity;
    ForceRepelArm((uintptr_t)entity);
    return result;
}

int __cdecl Hook_CheckAirTech(int entity) {
    const int result = g_origCheckAirTech ? g_origCheckAirTech(entity) : entity;
    ForcePushAwayArm((uintptr_t)entity);
    return result;
}

// --- Contact-time resolver tracing ---------------------------------------
//
// The collision pass calls exactly one of these per contact, chosen by the
// defender's category, BEFORE guard point and ordinary guard. Returning 1
// declines and the contact falls through to the normal-hit handler - which is
// what a "the parry did not fire" report looks like from the outside, with no
// way to tell an unarmed window from an armed one the resolver refused.
//
// So every call that reaches the dummy is logged with all of the resolver's own
// gate terms next to the result it returned. There is nothing left to infer.

typedef int (__cdecl* DefenceResolve5_t)(int, int, int16_t*, char, int);
typedef int (__cdecl* DefenceResolve2_t)(int, int);

DefenceResolve5_t g_origResolveCounter = nullptr;
DefenceResolve5_t g_origResolveParry = nullptr;
DefenceResolve5_t g_origResolveRepel = nullptr;
DefenceResolve5_t g_origResolvePushAway = nullptr;
DefenceResolve5_t g_origResolveAbsolute = nullptr;
DefenceResolve2_t g_origResolveDodge = nullptr;

// Points the dummy's window at the contact that is about to be judged.
//
// Called from inside the category resolver hook, BEFORE the original runs. This
// is the only place that has the exact attack mask of this contact, and nothing
// can run between it and the gate that reads the state. It also covers
// projectiles for free: a summon reaches the same six resolvers from the summon
// collision phase, with the HitDef entry as the payload instead of the
// attacker's own, so the mask is read the same way and the defender is still
// reached through attackerCtx+4.
//
// Every resolver tests the STAND lane first, so a mask carrying both lanes is a
// stand contact and only a crouch-only mask takes the low branch.
// What a preparation temporarily changed, so it can be put back the moment the
// resolver has read it - the same discipline ScopedGuardLane uses for the guard
// lanes it lends the ordinary-guard resolver.
struct DefencePreparation {
    uintptr_t defender = 0;
    bool flagsChanged = false;
    uint32_t oldFlags = 0;
};

DefencePreparation PrepareDefenceForContact(int attackerCtx, uint32_t attackMask) {
    DefencePreparation prep{};
    const uintptr_t defender = (uintptr_t)ReadMemory<uint32_t>(
        (uintptr_t)attackerCtx + ENTITY_OFF_OPPONENT);
    prep.defender = defender;
    const DefensiveResponse response = ArmTargetResponse(defender);
    if (!ResponseArmedByNativeHook(response)) {
        return prep;
    }
    if (!ForceDefenceAllowed(defender, DefensiveResponseLabel(response))) {
        return prep;
    }

    const bool airborne = ReadMemory<uint8_t>(defender + ENTITY_OFF_AIRBORNE) == 1;
    const bool low = (attackMask & ATTACK_GUARD_STAND) == 0 &&
                     (attackMask & ATTACK_GUARD_CROUCH) != 0;

    switch (response) {
        case DefensiveResponse::JustParry: {
            // sub_4A50A0 resolves the stand branch on (BACK held, DOWN clear)
            // and the low branch on (BACK held, DOWN held).
            Native_SetGuardPosture((int)defender, low ? 1 : 0, 1);
            const uint32_t action = ReadMemory<uint32_t>(defender + ENTITY_OFF_ACTION_ID);
            char kind = PARRY_KIND_STAND;
            int cls = 1;
            if (action == 64 || action == 65 || action == 67 ||
                action == 68 || action == 70 || action == 71) {
                kind = PARRY_KIND_BLOCKSTUN;
                cls = 3;
            } else if (action >= 34 && action <= 39) {
                kind = PARRY_KIND_CROUCH;
                cls = 2;
            }
            Native_SetParryWindow((int)defender, kind, cls);
            break;
        }
        case DefensiveResponse::Repel: {
            // The low bits of the reaction ARE the guard lanes it answers:
            // 5 is 0b101, 6 is 0b110, 7 is 0b111.
            const int desired = airborne ? REPEL_REACTION_AIR
                                         : (low ? REPEL_REACTION_LOW : REPEL_REACTION_HIGH);
            Native_SetHitReaction((int)defender, desired);
            Native_UpdateHitReaction((int)defender);
            break;
        }
        case DefensiveResponse::PushAwayPerfect:
        case DefensiveResponse::PushAwayMetered: {
            const bool perfect = response == DefensiveResponse::PushAwayPerfect;
            Native_SetPushAwayFlag(reinterpret_cast<uint8_t*>(defender), perfect ? 1 : 0);
            // +1996 is DOWN held, +1997 BACK held - the same pair of lanes the
            // parry branch uses, under different names.
            Native_SetPushAwayPosture((int)defender, low ? 1 : 0, 1);
            break;
        }
        default:
            return prep;
    }
    g_arm.contactsPrepared++;
    return prep;
}

// Puts back anything the preparation lent the resolver.
void FinishDefenceForContact(const DefencePreparation& prep) {
    if (!prep.flagsChanged || !prep.defender) {
        return;
    }
    // Re-read rather than restoring blind: the resolver may legitimately have
    // written other bits of +1940 while it ran.
    const uint32_t after = ReadMemory<uint32_t>(prep.defender + ENTITY_OFF_MAX_HIT_FLAGS);
    const uint32_t restored = (after & ~(uint32_t)DEFENDER_FLAG_ABSOLUTE_DEFENCE) |
                              (prep.oldFlags & DEFENDER_FLAG_ABSOLUTE_DEFENCE);
    WriteMemory<uint32_t>(prep.defender + ENTITY_OFF_MAX_HIT_FLAGS, restored);
}

void TraceResolve(const char* mechanic, int attackerCtx, uint32_t attackMask, int result) {
    const uintptr_t defender = (uintptr_t)ReadMemory<uint32_t>(
        (uintptr_t)attackerCtx + ENTITY_OFF_OPPONENT);
    if (!PracticeGateOpen() || defender == 0 || defender != DummyEntity()) {
        return;
    }
    g_arm.resolverCalls++;
    if (result == CONTACT_RESULT_NONE) {
        g_arm.resolverDeclines++;
    } else {
        // Anything other than "declined" means the mechanic was awarded.
        g_defensiveSuccessPending = true;
        g_defensiveSuccessFrame = ReadSimFrame();
        g_defensiveSuccessResult = (uint32_t)result;
    }
    LOG_INFO("[Defence] resolve %s mask=0x%05X class=%s -> %s(%d) | "
             "allow1949=%u kind1989=%u react1980=%d flags1940=0x%04X "
             "parry1965=0x%02X down1973=%u back1974=%u push1993=0x%02X free1994=%u "
             "air=%u action=%u meter=%u",
             mechanic,
             (unsigned)attackMask,
             GroundGuardClassLabel(DecodeGroundGuardClass(attackMask)),
             ContactResolutionLabel((uint32_t)result), result,
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_DEFENCE_ALLOWED),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_HIT_REACTION_KIND),
             (int)ReadMemory<int32_t>(defender + ENTITY_OFF_HIT_REACTION_STATE),
             (unsigned)ReadMemory<uint32_t>(defender + ENTITY_OFF_MAX_HIT_FLAGS),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_PARRY_WINDOW),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_PARRY_STANCE),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_PARRY_GUARD_HELD),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_PUSH_AWAY_ARMED),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_PUSH_AWAY_FREE),
             (unsigned)ReadMemory<uint8_t>(defender + ENTITY_OFF_AIRBORNE),
             (unsigned)ReadMemory<uint32_t>(defender + ENTITY_OFF_ACTION_ID),
             (unsigned)ReadMemory<uint16_t>(defender + ENTITY_OFF_METER));
}

#define DEFENCE_RESOLVE_HOOK(name, orig, label)                                      \
    int __cdecl name(int attackerCtx, int sourceObject, int16_t* contactPos,         \
                     char attackerFacing, int payloadAddress) {                      \
        const uint32_t mask = ReadMemory<uint32_t>((uintptr_t)payloadAddress + 4);   \
        const DefencePreparation prep = PrepareDefenceForContact(attackerCtx, mask); \
        const int result = orig                                                      \
            ? orig(attackerCtx, sourceObject, contactPos, attackerFacing,            \
                   payloadAddress)                                                   \
            : CONTACT_RESULT_NONE;                                                   \
        FinishDefenceForContact(prep);                                               \
        TraceResolve(label, attackerCtx, mask, result);                              \
        return result;                                                               \
    }

DEFENCE_RESOLVE_HOOK(Hook_ResolveCounter, g_origResolveCounter, "guard_counter")
DEFENCE_RESOLVE_HOOK(Hook_ResolveParry, g_origResolveParry, "just_parry")
DEFENCE_RESOLVE_HOOK(Hook_ResolveRepel, g_origResolveRepel, "repel")
DEFENCE_RESOLVE_HOOK(Hook_ResolvePushAway, g_origResolvePushAway, "push_away")
DEFENCE_RESOLVE_HOOK(Hook_ResolveAbsolute, g_origResolveAbsolute, "absolute_defence")

#undef DEFENCE_RESOLVE_HOOK

// The dodge takes the payload as its second argument instead of its fifth.
int __cdecl Hook_ResolveDodge(int attackerCtx, int payloadAddress) {
    const uint32_t mask = ReadMemory<uint32_t>((uintptr_t)payloadAddress + 4);
    // The dodge is deliberately NOT prepared. sub_4A5D30 only stamps the result
    // code - the roll itself comes from the action the dummy is already in - so
    // lending it its flag would report a dodge the dummy never performed.
    const int result = g_origResolveDodge
        ? g_origResolveDodge(attackerCtx, payloadAddress)
        : CONTACT_RESULT_NONE;
    TraceResolve("dodge", attackerCtx, mask, result);
    return result;
}

// --- Sequence bookkeeping ------------------------------------------------

bool DefenderInForcedState(uintptr_t dummy) {
    const uint32_t actionId = ReadMemory<uint32_t>(dummy + ENTITY_OFF_ACTION_ID);
    return Training::IsForcedDefenderLock(actionId);
}

void BeginSequenceIfNeeded(uint32_t simFrame, const ThreatKey& firstThreat) {
    if (g_sequence.active) {
        SequenceNoteThreat(g_sequence, simFrame);
        return;
    }

    const uintptr_t attacker = AttackerEntity();
    const uintptr_t dummy = DummyEntity();
    const uint32_t roll = DeterministicSequenceRoll(
        simFrame,
        g_sequence.generation + 1,
        ReadCharacterId(attacker),
        ReadCharacterId(dummy),
        firstThreat);
    SequenceBegin(g_sequence, simFrame, roll, g_config.randomPercent);

    LOG_INFO("[AutoBlock] sequence start frame=%u gen=%u policy=%s random_block=%d",
             simFrame,
             g_sequence.generation,
             BlockPolicyLabel(g_config.policy),
             g_sequence.randomBlock ? 1 : 0);
}

// --- Frame plan ----------------------------------------------------------

// Snapshot of the dummy's guard eligibility, shared by the plan and the hook.
DefenderGuardState ReadDefenderGuardState(uintptr_t dummy) {
    DefenderGuardState state{};
    state.practiceAdvancedModeActive = g_config.enabled;
    state.macroOwnsDummyInput = g_config.macroOwnsDummyInput;
    state.guardGauge = ReadMemory<uint16_t>(dummy + ENTITY_OFF_GUARD_GAUGE);
    state.actionId = ReadMemory<uint32_t>(dummy + ENTITY_OFF_ACTION_ID);
    state.airborne = ReadMemory<uint8_t>(dummy + ENTITY_OFF_AIRBORNE) == 1;
    state.attackActive = ReadMemory<uint8_t>(dummy + ENTITY_OFF_ATTACK_STATE) == 1;

    // The collision phases run after command dispatch but inside the same tick,
    // so the route vector still describes this tick's permissions.
    const NativeRecoverySample routes = PracticeRecovery_ReadSample(dummy);
    state.neutralRouteOpen = NeutralRouteOpen(routes);
    return state;
}

void BuildFramePlan() {
    const uint32_t simFrame = ReadSimFrame();
    g_lastProcessedSimFrame = simFrame;

    const uintptr_t dummy = DummyEntity();
    const bool airborne = ReadMemory<uint8_t>(dummy + ENTITY_OFF_AIRBORNE) == 1;

    ResetPlan(g_plan, simFrame, airborne);
    memset(&g_outcome, 0, sizeof(g_outcome));
    g_outcome.simFrame = simFrame;
    g_scannedCount = 0;
    g_overlappingCount = 0;
    g_anticipatoryWantsGuard = false;
    g_anticipatoryGuard = SemanticDirection::Neutral;
    g_anticipatoryMask = 0;
    g_defenderResultBefore = ReadMemory<uint32_t>(dummy + ENTITY_OFF_MAX_HIT_ACTIVE);

    if (!PracticeGateOpen()) {
        if (g_sequence.active) {
            SequenceEnd(g_sequence);
        }
        // Keep the counters, drop the live half: a stale "armed" reading in the
        // menu would say the opposite of what is happening.
        g_arm.installed = g_armHooksInstalled;
        g_arm.gateOpen = false;
        g_arm.hookDriven = false;
        g_arm.category = PracticeDefense_DummyDefenseCategory();
        g_arm.configured = g_config.response;
        g_arm.effective = DefensiveResponse::NormalGuard;
        g_arm.input = DefenseInputKind::None;
        g_arm.recordsToAttack = -1;
        g_arm.attackerCommitted = false;
        g_arm.threatArmed = false;
        g_telemetry.plan = g_plan;
        g_telemetry.sequence = g_sequence;
        return;
    }

    ScanDirectThreat(AttackerEntity(), dummy);
    ScanHitDefThreats(dummy);

    // Only contacts that actually resolve this tick constrain the lane.
    const ScannedThreat* firstOverlap = nullptr;
    for (int i = 0; i < g_scannedCount; ++i) {
        if (!g_scanned[i].overlapsNow) {
            continue;
        }
        if (!firstOverlap) {
            firstOverlap = &g_scanned[i];
        }
        g_overlappingCount++;
        PlanAccumulateContact(g_plan, g_scanned[i].attackMask);
    }

    if (firstOverlap) {
        BeginSequenceIfNeeded(simFrame, firstOverlap->key);
    } else if (g_scannedCount > 0 && g_sequence.active) {
        // Still armed: a multi-hit action's inactive gap must not end it.
        SequenceNoteThreat(g_sequence, simFrame);
    } else if (g_sequence.active) {
        const bool armed = ReadMemory<uint8_t>(AttackerEntity() + ENTITY_OFF_ATTACK_STATE) != 0;
        if (SequenceTickQuiet(g_sequence, armed, DefenderInForcedState(dummy), kSequenceQuietGrace)) {
            LOG_INFO("[AutoBlock] sequence end frame=%u gen=%u groups=%u",
                     simFrame, g_sequence.generation, g_sequence.resolvedContactGroups);
            // Running totals at the natural boundary, so a session can be read
            // back without counting lines by hand.
            LOG_INFO("[Defence] totals contacts=%u guard=%u parry=%u repel=%u push=%u "
                     "dodge=%u absolute=%u counter=%u hit=%u | arms=%u (parry %u repel %u "
                     "push %u) forced1949=%u resolver_calls=%u declines=%u",
                     g_telemetry.contactTicks,
                     g_telemetry.resultCounts[CONTACT_RESULT_GUARD],
                     g_telemetry.resultCounts[CONTACT_RESULT_JUST_PARRY],
                     g_telemetry.resultCounts[CONTACT_RESULT_REPEL],
                     g_telemetry.resultCounts[CONTACT_RESULT_PUSH_AWAY],
                     g_telemetry.resultCounts[CONTACT_RESULT_DODGE],
                     g_telemetry.resultCounts[CONTACT_RESULT_ABSOLUTE_DEFENSE],
                     g_telemetry.resultCounts[CONTACT_RESULT_UNIQUE_DEFENSE],
                     g_telemetry.resultCounts[CONTACT_RESULT_HIT],
                     g_arm.arms, g_arm.parryArms, g_arm.repelArms, g_arm.pushAwayArms,
                     g_arm.defenceAllowedForced,
                     g_arm.resolverCalls, g_arm.resolverDeclines);
            LOG_INFO("[Defence] totals route_stated=%u stock_granted=%u cancels_offered=%u",
                     g_arm.counterGuardRouteOpened, g_arm.absoluteStockForced,
                     g_arm.guardCancelsOffered);
            LOG_INFO("[Defence] totals contacts_prepared=%u (the window was pointed at a "
                     "specific contact this many times, projectiles included)",
                     g_arm.contactsPrepared);
            SequenceEnd(g_sequence);
        }
    }

    PlanFinalize(g_plan, g_config.preferCrouch);
    g_plan.contactOrdinal = g_sequence.resolvedContactGroups;
    g_plan.policyWantsBlock =
        g_sequence.active && PolicyWantsBlock(EffectiveBlockPolicy(), g_sequence, g_plan.contactOrdinal);

    if (g_plan.lane == GuardLane::Conflict) {
        g_telemetry.conflictFrames++;
        LOG_WARN("[AutoBlock] high/low conflict frame=%u threats=%u", simFrame, g_plan.threatCount);
        for (int i = 0; i < g_scannedCount; ++i) {
            if (!g_scanned[i].overlapsNow) {
                continue;
            }
            LOG_WARN("[AutoBlock]   threat src=%s owner=%u id=%u slot=%d mask=0x%05X class=%s",
                     g_scanned[i].key.source == ThreatSource::HitDef ? "hitdef" : "direct",
                     (unsigned)g_scanned[i].key.owner,
                     (unsigned)g_scanned[i].key.objectId,
                     (int)(int16_t)g_scanned[i].key.hitDefSlot,
                     (unsigned)g_scanned[i].attackMask,
                     GroundGuardClassLabel(DecodeGroundGuardClass(g_scanned[i].attackMask)));
        }
    }

    // Anticipatory posture for the next input pass. The contact-time hook is what
    // guarantees the block; this only gives the dummy a natural direction and
    // makes the input display truthful.
    const DefenderGuardState defenderState = ReadDefenderGuardState(dummy);
    const uint32_t anticipatedOrdinal = g_sequence.active ? g_sequence.resolvedContactGroups : 0;
    const BlockPolicy effectivePolicy = EffectiveBlockPolicy();
    const bool policyWould = effectivePolicy != BlockPolicy::Off &&
                             PolicyWantsBlock(effectivePolicy, g_sequence, anticipatedOrdinal);

    const int dummyCategory = PracticeDefense_DummyDefenseCategory();
    const DefensiveResponse response = EffectiveResponse(dummyCategory);

    // Absolute defence has no input and no window: Entity_UpdateAction_Attacks
    // queues action 49 (52 airborne) out of any blockstun state whenever the
    // character is category 5 and the stock byte at +823 is non-zero, and
    // action 49 is what carries the +1940 & 0x100 the resolver wants. Nothing
    // in the decompiled C ever writes +823 - it comes from a move script - so a
    // dummy told to use the mechanic gets the stock, the same way +1949 is
    // supplied for the window mechanics.
    if (dummyCategory == kDefenseCategoryAbsolute) {
        static int s_lastStock = -1;
        const int stock = ReadMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE);
        if (stock != s_lastStock) {
            s_lastStock = stock;
            LOG_INFO("[Defence] absolute-defence stock +823 = %d (action=%u)",
                     stock, (unsigned)defenderState.actionId);
        }
        // +823 gates BOTH entries: the blockstun cancel in
        // Entity_UpdateAction_Attacks, and the neutral 214D command itself
        // (sub_5045a0 tests it before it will even queue the move). Nothing in
        // the executable ever writes it, so it is a resource handed out
        // elsewhere - and a dummy told to use the mechanic is a dummy that has
        // its charge. Everything past this point stays the engine's.
        if (stock == 0 && (response == DefensiveResponse::AbsoluteDefence ||
                           response == DefensiveResponse::AbsoluteDefenceFirst)) {
            WriteMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE, 1);
            g_arm.absoluteStockForced++;
            LOG_INFO("[Defence] absolute-defence charge granted (+823 was 0)");
        }
    }

    const uintptr_t attacker = AttackerEntity();
    DefenseDriveSample drive{};
    drive.parryWindow = ReadMemory<uint8_t>(dummy + ENTITY_OFF_PARRY_WINDOW);
    drive.reactionState = (int32_t)ReadMemory<uint32_t>(dummy + ENTITY_OFF_HIT_REACTION_STATE);
    drive.actionId = defenderState.actionId;
    drive.meter = ReadMemory<uint16_t>(dummy + ENTITY_OFF_METER);
    drive.airborne = airborne;
    drive.threatArmed = g_scannedCount > 0;
    // Prediction, not observation: the attacker's own collision records say a
    // box is coming several phases before one exists, and a closed command
    // route says it is committed even when the very first record already
    // carries the box. Waiting for +1736 to flip is the same instant as waiting
    // for the box, which is why keying off it changed nothing.
    drive.recordsToAttack = (int16_t)RecordsUntilAttackBox(attacker);
    drive.attackerCommitted = AttackerCommitted(attacker);
    // The preemptive counter guard is armed through the command-matches seam,
    // which belongs to another hook - so it is only "hook driven" when that
    // seam is actually live.
    drive.nativeArmActive =
        g_armHooksInstalled &&
        (response != DefensiveResponse::AbsoluteDefenceFirst || g_counterGuardRouteAvailable);
    drive.actionable = CanAutoGuardAtContact(defenderState);
    drive.counterGuardStock =
        ReadMemory<uint8_t>(dummy + ENTITY_OFF_DEFENSE_RESOURCE) != 0;
    // The parry direction depends on what is coming, so decode the threat the
    // dummy would be reacting to. The latched lane is the fallback for a plan
    // that resolved before the scan found an overlapping box.
    if (const ScannedThreat* armed = NearestArmedThreat()) {
        drive.threatClass = DecodeGroundGuardClass(armed->attackMask);
    } else if (g_plan.laneLatched) {
        drive.threatClass = (g_plan.lane == GuardLane::Crouch)
                                ? GroundGuardClass::CrouchOnly
                                : GroundGuardClass::Either;
    }
    g_defenseInput = EvaluateDefenseInput(response, drive, g_defenseDrive);
    g_lastThreatClass = drive.threatClass;
    g_lastDrive = drive;
    PublishArmState(dummy, dummyCategory, response, drive);

    const bool anticipatedLaneAllowed =
        !firstOverlap ||
        PolicyAllowsLane(effectivePolicy, firstOverlap->attackMask, g_config.preferCrouch);
    if (policyWould && anticipatedLaneAllowed && CanAutoGuardAtContact(defenderState)) {
        if (g_plan.laneLatched) {
            g_anticipatoryGuard = SemanticGuardForLane(g_plan.lane, g_plan.defenderAirborne);
            g_anticipatoryMask = firstOverlap ? firstOverlap->attackMask : 0;
            g_anticipatoryWantsGuard = true;
        } else if (const ScannedThreat* armed = NearestArmedThreat()) {
            // Decode the armed threat's own mask, never the owner's posture.
            FrameGuardPlan probe{};
            ResetPlan(probe, simFrame, airborne);
            PlanAccumulateContact(probe, armed->attackMask);
            PlanFinalize(probe, g_config.preferCrouch);
            g_anticipatoryGuard = probe.laneLatched
                ? SemanticGuardForLane(probe.lane, airborne)
                : SemanticDirection::Neutral;
            g_anticipatoryMask = armed->attackMask;
            g_anticipatoryWantsGuard = g_anticipatoryGuard != SemanticDirection::Neutral;
        }
    }

    g_telemetry.plan = g_plan;
    g_telemetry.sequence = g_sequence;
}

void BeginCollisionPhase() {
    // Epoch first so a stalled sim-frame counter (pause, frame step) still gets
    // exactly one plan per collision tick; the frame check is the backstop for a
    // phase hook that failed to install.
    if (g_planEpoch == g_phaseEpoch && g_plan.simFrame == ReadSimFrame()) {
        return;
    }
    g_planEpoch = g_phaseEpoch;
    BuildFramePlan();
}

void EndCollisionTick() {
    const uint32_t resultAfter = ReadMemory<uint32_t>(DummyEntity() + ENTITY_OFF_MAX_HIT_ACTIVE);
    if (resultAfter != g_defenderResultBefore && resultAfter >= 3 && resultAfter <= 9) {
        g_outcome.anyContact = true;
        g_outcome.finalResult = (uint8_t)resultAfter;
        g_outcome.resultMask |= (1u << resultAfter);
        g_telemetry.lastResult = resultAfter;
        g_telemetry.hasLastContact = true;
        g_telemetry.lastContactFrame = g_outcome.simFrame;
    }
    g_defenderResultBefore = resultAfter;

    if (g_outcome.anyContact) {
        SequenceNoteContactGroup(g_sequence, g_outcome.simFrame);
        g_telemetry.contactTicks++;
        if (g_outcome.finalResult < (uint8_t)(sizeof(g_telemetry.resultCounts) /
                                              sizeof(g_telemetry.resultCounts[0]))) {
            g_telemetry.resultCounts[g_outcome.finalResult]++;
        }
    }
    g_telemetry.outcome = g_outcome;
    g_telemetry.sequence = g_sequence;
    g_telemetry.plan = g_plan;
    g_phaseEpoch++;
}

// --- Scoped defender-lane augmentation -----------------------------------

class ScopedGuardLane {
public:
    ScopedGuardLane(uintptr_t defender, GuardLane lane)
        : m_defender(defender), m_applied(false), m_oldFlags(0), m_tempFlags(0) {
        const uint32_t bits = GuardLaneBits(lane);
        if (!defender || bits == 0) {
            return;
        }
        m_oldFlags = ReadMemory<uint32_t>(defender + ENTITY_OFF_MAX_HIT_FLAGS);
        m_tempFlags = ApplyGuardLaneBits(m_oldFlags, lane);
        if (m_tempFlags == m_oldFlags) {
            return;
        }
        if (WriteMemory<uint32_t>(defender + ENTITY_OFF_MAX_HIT_FLAGS, m_tempFlags)) {
            m_applied = true;
        }
    }

    ~ScopedGuardLane() {
        if (!m_applied) {
            return;
        }
        const uint32_t after = ReadMemory<uint32_t>(m_defender + ENTITY_OFF_MAX_HIT_FLAGS);
        WriteMemory<uint32_t>(m_defender + ENTITY_OFF_MAX_HIT_FLAGS,
                              RestoreGuardLaneBits(after, m_oldFlags));
    }

    bool applied() const { return m_applied; }
    uint32_t oldFlags() const { return m_oldFlags; }
    uint32_t tempFlags() const { return m_tempFlags; }

private:
    uintptr_t m_defender;
    bool m_applied;
    uint32_t m_oldFlags;
    uint32_t m_tempFlags;
};

// --- Hooks ---------------------------------------------------------------

int __cdecl Hook_OrdinaryGuard(int attackerCtx, int sourceObject, int16_t* contactPos,
                               char attackerFacing, int payloadAddress) {
    if (!g_origOrdinaryGuard) {
        return 1;
    }

    const uintptr_t defender = (uintptr_t)ReadMemory<uint32_t>((uintptr_t)attackerCtx + ENTITY_OFF_OPPONENT);
    const uintptr_t dummy = DummyEntity();

    if (!PracticeGateOpen() || defender == 0 || defender != dummy) {
        return g_origOrdinaryGuard(attackerCtx, sourceObject, contactPos, attackerFacing, payloadAddress);
    }

    BeginCollisionPhase();

    const uint32_t attackMask = ReadMemory<uint32_t>((uintptr_t)payloadAddress + 4);
    const uint32_t defenderFlags = ReadMemory<uint32_t>(defender + ENTITY_OFF_MAX_HIT_FLAGS);
    const bool airborne = ReadMemory<uint8_t>(defender + ENTITY_OFF_AIRBORNE) == 1;
    const uint32_t frame = g_plan.simFrame;

    g_telemetry.guardHookCalls++;
    g_outcome.ordinaryGuardHookCalled = true;
    if (sourceObject != 0) {
        g_outcome.hitDefContacts++;
    } else {
        g_outcome.directContacts++;
    }

    const DefenderGuardState defenderState = ReadDefenderGuardState(defender);
    const bool eligible = CanAutoGuardAtContact(defenderState);
    const bool nativeAlready = DefenderAlreadyGuards(attackMask, defenderFlags, airborne);

    // The exact scan may have missed an unusual path; latch from the contact the
    // resolver actually presented, but never create a second lane in one frame.
    if (!g_plan.laneLatched && g_plan.lane != GuardLane::Conflict && g_plan.lane != GuardLane::None) {
        PlanLatchFromContact(g_plan, attackMask, defenderFlags, g_config.preferCrouch);
        g_plan.contactOrdinal = g_sequence.resolvedContactGroups;
        if (!g_sequence.active) {
            ThreatKey key{};
            key.source = sourceObject != 0 ? ThreatSource::HitDef : ThreatSource::DirectPlayer;
            key.payloadAddress = (uintptr_t)payloadAddress;
            key.objectId = sourceObject != 0
                ? ReadMemory<uint32_t>((uintptr_t)sourceObject + HITDEF_OFF_ID)
                : ReadMemory<uint32_t>((uintptr_t)attackerCtx + ENTITY_OFF_ACTION_ID);
            BeginSequenceIfNeeded(frame, key);
            g_plan.contactOrdinal = g_sequence.resolvedContactGroups;
        }
        g_plan.policyWantsBlock =
            PolicyWantsBlock(EffectiveBlockPolicy(), g_sequence, g_plan.contactOrdinal);
    }

    const bool compatible = PlanAcceptsAttack(g_plan, attackMask, defenderFlags);
    if (g_plan.laneLatched && !compatible && !nativeAlready) {
        // A later contact this frame that the latched lane cannot cover.
        if (!SpecialGuardSatisfied(attackMask, defenderFlags)) {
            g_telemetry.refusedSpecialGuard++;
        } else if (!AttackHasGroundGuardLane(attackMask) && !airborne) {
            g_telemetry.refusedNoLane++;
        } else {
            g_plan.conflictCount++;
            g_telemetry.conflictFrames++;
        }
    }

    // Stance Only holds one lane rather than switching, so an attack the stance
    // does not cover is let through on purpose.
    const bool laneAllowed =
        PolicyAllowsLane(EffectiveBlockPolicy(), attackMask, g_config.preferCrouch);
    const bool augment =
        g_plan.policyWantsBlock && eligible && compatible && laneAllowed && !nativeAlready;
    if (g_plan.policyWantsBlock && eligible && compatible && !laneAllowed) {
        LOG_INFO("[AutoBlock] stance-only let a %s through frame=%u mask=0x%05X (stance covers %s)",
                 GroundGuardClassLabel(DecodeGroundGuardClass(attackMask)),
                 frame, (unsigned)attackMask,
                 g_config.preferCrouch ? "crouch" : "stand");
    }

    g_telemetry.hasLastContact = true;
    g_telemetry.lastContactFrame = frame;
    g_telemetry.lastSource = sourceObject != 0 ? ThreatSource::HitDef : ThreatSource::DirectPlayer;
    g_telemetry.lastHitDefSlot = -1;
    g_telemetry.lastHitDefId = 0;
    if (sourceObject != 0) {
        g_telemetry.lastHitDefId = ReadMemory<uint32_t>((uintptr_t)sourceObject + HITDEF_OFF_ID);
        g_telemetry.lastHitDefSlot =
            (int)(((uintptr_t)sourceObject - ADDR_SUMMON_ARRAY) / SUMMON_ENTRY_SIZE);
    }
    g_telemetry.lastAttackMask = attackMask;
    g_telemetry.lastClass = DecodeGroundGuardClass(attackMask);
    g_telemetry.lastLane = g_plan.lane;
    g_telemetry.lastEligible = eligible;
    g_telemetry.lastNativeGuardWithoutMod = nativeAlready;
    g_telemetry.lastPrearmed = g_inputPrearmed;
    g_telemetry.facingAtInput = g_facingAtInput;
    g_telemetry.facingAtContact = ReadMemory<int8_t>(defender + ENTITY_OFF_FACING);
    g_telemetry.lastFlagsBefore = defenderFlags;

    int result = 0;
    if (!augment) {
        g_telemetry.lastSafetyArm = false;
        g_telemetry.lastFlagsTemp = defenderFlags;
        result = g_origOrdinaryGuard(attackerCtx, sourceObject, contactPos, attackerFacing, payloadAddress);
    } else {
        ScopedGuardLane lane(defender, g_plan.lane);
        g_telemetry.lastSafetyArm = lane.applied();
        g_telemetry.lastFlagsTemp = lane.applied() ? lane.tempFlags() : defenderFlags;
        if (lane.applied()) {
            g_telemetry.laneApplications++;
            g_outcome.temporaryLaneApplied = true;
        }
        result = g_origOrdinaryGuard(attackerCtx, sourceObject, contactPos, attackerFacing, payloadAddress);
    }

    // Result 10 never reaches defender+1944; the resolver's return value is the
    // only place ordinary guard is reported.
    if (result != 1) {
        g_outcome.anyContact = true;
        g_outcome.finalResult = (uint8_t)result;
        if (result >= 3 && result <= 11) {
            g_outcome.resultMask |= (1u << result);
        }
    }
    g_telemetry.lastResult = (uint32_t)result;

    const uint32_t threatId = sourceObject != 0 ? g_telemetry.lastHitDefId
                                                : ReadMemory<uint32_t>((uintptr_t)attackerCtx + ENTITY_OFF_ACTION_ID);
    if (result == CONTACT_RESULT_GUARD &&
        g_lastHitWantedBlock &&
        g_lastHitThreatId == threatId &&
        g_lastHitEpoch + 1 == g_phaseEpoch) {
        g_telemetry.lateBlockSignatures++;
        LOG_ERROR("[AutoBlock] LATE BLOCK: hit on frame %u then guard on frame %u for threat %u",
                  g_lastHitFrame, frame, threatId);
    }

#ifdef _DEBUG
    // The resolver returns 10 or 1, so the invariant to check is that a lane we
    // actually supplied to a compatible, eligible contact produced the guard.
    // A failure here means the contact will fall through to the normal-hit
    // handler on this very frame - the defect this whole path exists to remove.
    if (augment && g_telemetry.lastSafetyArm) {
        assert(result == CONTACT_RESULT_GUARD &&
               "auto-block supplied a compatible guard lane but the resolver declined");
    }
#endif

    if (augment && result == CONTACT_RESULT_GUARD) {
        LOG_INFO("[AutoBlock] frame=%u %s mask=0x%05X class=%s plan=%s ordinal=%u prearmed=%d "
                 "safety_arm=%d flags=0x%08X->0x%08X result=%s",
                 frame,
                 sourceObject != 0 ? "hitdef" : "direct",
                 (unsigned)attackMask,
                 GroundGuardClassLabel(g_telemetry.lastClass),
                 GuardLaneLabel(g_plan.lane),
                 g_plan.contactOrdinal,
                 g_inputPrearmed ? 1 : 0,
                 g_telemetry.lastSafetyArm ? 1 : 0,
                 (unsigned)g_telemetry.lastFlagsBefore,
                 (unsigned)g_telemetry.lastFlagsTemp,
                 ContactResolutionLabel((uint32_t)result));
    }

    return result;
}

int __cdecl Hook_NormalHit(int attackerCtx, int sourceObject, int16_t* contactPos,
                           char attackerFacing, int payloadAddress, char otherAligned) {
    const int result = g_origNormalHit
        ? g_origNormalHit(attackerCtx, sourceObject, contactPos, attackerFacing,
                          payloadAddress, otherAligned)
        : 1;

    if (!PracticeGateOpen() || result != CONTACT_RESULT_HIT) {
        return result;
    }

    const uintptr_t defender = (uintptr_t)ReadMemory<uint32_t>((uintptr_t)attackerCtx + ENTITY_OFF_OPPONENT);
    if (defender != DummyEntity()) {
        return result;
    }

    g_outcome.anyContact = true;
    g_outcome.finalResult = CONTACT_RESULT_HIT;
    g_outcome.resultMask |= (1u << CONTACT_RESULT_HIT);

    g_lastHitThreatId = sourceObject != 0
        ? ReadMemory<uint32_t>((uintptr_t)sourceObject + HITDEF_OFF_ID)
        : ReadMemory<uint32_t>((uintptr_t)attackerCtx + ENTITY_OFF_ACTION_ID);
    g_lastHitFrame = g_plan.simFrame;
    g_lastHitEpoch = g_phaseEpoch;
    g_lastHitWantedBlock = g_plan.policyWantsBlock;

    if (g_plan.policyWantsBlock) {
        const uint32_t attackMask = ReadMemory<uint32_t>((uintptr_t)payloadAddress + 4);
        LOG_WARN("[AutoBlock] unguarded contact frame=%u %s mask=0x%05X class=%s plan=%s ordinal=%u",
                 g_plan.simFrame,
                 sourceObject != 0 ? "hitdef" : "direct",
                 (unsigned)attackMask,
                 GroundGuardClassLabel(DecodeGroundGuardClass(attackMask)),
                 GuardLaneLabel(g_plan.lane),
                 g_plan.contactOrdinal);
    }

    return result;
}

int __cdecl Hook_GrabPhase(int match) {
    BeginCollisionPhase();
    return g_origGrabPhase ? g_origGrabPhase(match) : 0;
}

char __cdecl Hook_DamagePhase(int match) {
    BeginCollisionPhase();
    return g_origDamagePhase ? g_origDamagePhase(match) : 0;
}

int __cdecl Hook_SummonPhase(int match) {
    BeginCollisionPhase();
    const int result = g_origSummonPhase ? g_origSummonPhase(match) : 0;
    EndCollisionTick();
    return result;
}

bool CreateAndEnable(uintptr_t target, void* detour, void** original, const char* label) {
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (status != MH_OK) {
        snprintf(g_installError, sizeof(g_installError), "%s create failed (%d)", label, (int)status);
        LOG_ERROR("[AutoBlock] %s", g_installError);
        return false;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK) {
        snprintf(g_installError, sizeof(g_installError), "%s enable failed (%d)", label, (int)status);
        LOG_ERROR("[AutoBlock] %s", g_installError);
        return false;
    }
    LOG_INFO("[AutoBlock] hooked %s @ 0x%08X", label, (unsigned)target);
    return true;
}

} // namespace

bool PracticeDefense_Install() {
    if (g_installed) {
        return g_ordinaryGuardHooked;
    }

    g_installError[0] = '\0';
    PracticeDefense_Reset();

    g_ordinaryGuardHooked = CreateAndEnable(ADDR_DEFENSE_ORDINARY_GUARD,
                                            reinterpret_cast<void*>(&Hook_OrdinaryGuard),
                                            reinterpret_cast<void**>(&g_origOrdinaryGuard),
                                            "ordinary guard resolver");
    if (!g_ordinaryGuardHooked) {
        // First-frame-safe auto-block is unavailable; do not silently degrade to
        // the known-late post-render path.
        g_installed = true;
        return false;
    }

    CreateAndEnable(ADDR_COLLISION_GRAB_PHASE,
                    reinterpret_cast<void*>(&Hook_GrabPhase),
                    reinterpret_cast<void**>(&g_origGrabPhase),
                    "grab phase");
    CreateAndEnable(ADDR_COLLISION_DAMAGE_PHASE,
                    reinterpret_cast<void*>(&Hook_DamagePhase),
                    reinterpret_cast<void**>(&g_origDamagePhase),
                    "damage phase");
    CreateAndEnable(ADDR_COLLISION_SUMMON_PHASE,
                    reinterpret_cast<void*>(&Hook_SummonPhase),
                    reinterpret_cast<void**>(&g_origSummonPhase),
                    "summon hit phase");
    CreateAndEnable(ADDR_DEFENSE_NORMAL_HIT,
                    reinterpret_cast<void*>(&Hook_NormalHit),
                    reinterpret_cast<void**>(&g_origNormalHit),
                    "normal hit fallback");

    // The three native window-arming routines. All three have to take, because
    // the fallback for any one of them is the input driver, which cannot open a
    // window ahead of a hit it has not seen yet.
    const bool parryArm = CreateAndEnable(ADDR_ENTITY_CHECK_HIT_STATE,
                                          reinterpret_cast<void*>(&Hook_CheckHitState),
                                          reinterpret_cast<void**>(&g_origCheckHitState),
                                          "just-parry arm");
    const bool repelArm = CreateAndEnable(ADDR_ENTITY_CHECK_GUARD_STATE,
                                          reinterpret_cast<void*>(&Hook_CheckGuardState),
                                          reinterpret_cast<void**>(&g_origCheckGuardState),
                                          "repel arm");
    const bool pushArm = CreateAndEnable(ADDR_ENTITY_CHECK_AIR_TECH,
                                         reinterpret_cast<void*>(&Hook_CheckAirTech),
                                         reinterpret_cast<void**>(&g_origCheckAirTech),
                                         "push-away arm");
    g_armHooksInstalled = parryArm && repelArm && pushArm;
    // The guard-cancel offer: absolute defence is granted its stock here, one
    // whole tick phase earlier than the collision pass could manage.
    CreateAndEnable(ADDR_ENTITY_UPDATE_ACTION_ATTACKS,
                    reinterpret_cast<void*>(&Hook_UpdateActionAttacks),
                    reinterpret_cast<void**>(&g_origUpdateActionAttacks),
                    "guard cancel offer");

    // Tracing only: every category's resolver, so a contact that declined says
    // which of its own gate terms was not met.
    CreateAndEnable(ADDR_DEFENCE_RESOLVE_COUNTER,
                    reinterpret_cast<void*>(&Hook_ResolveCounter),
                    reinterpret_cast<void**>(&g_origResolveCounter),
                    "category 1 resolver");
    CreateAndEnable(ADDR_DEFENCE_RESOLVE_PARRY,
                    reinterpret_cast<void*>(&Hook_ResolveParry),
                    reinterpret_cast<void**>(&g_origResolveParry),
                    "category 2 resolver");
    CreateAndEnable(ADDR_DEFENCE_RESOLVE_REPEL,
                    reinterpret_cast<void*>(&Hook_ResolveRepel),
                    reinterpret_cast<void**>(&g_origResolveRepel),
                    "category 3 resolver");
    CreateAndEnable(ADDR_DEFENCE_RESOLVE_PUSH_AWAY,
                    reinterpret_cast<void*>(&Hook_ResolvePushAway),
                    reinterpret_cast<void**>(&g_origResolvePushAway),
                    "category 4 resolver");
    CreateAndEnable(ADDR_DEFENCE_RESOLVE_ABSOLUTE,
                    reinterpret_cast<void*>(&Hook_ResolveAbsolute),
                    reinterpret_cast<void**>(&g_origResolveAbsolute),
                    "category 5 resolver");
    CreateAndEnable(ADDR_DEFENCE_RESOLVE_DODGE,
                    reinterpret_cast<void*>(&Hook_ResolveDodge),
                    reinterpret_cast<void**>(&g_origResolveDodge),
                    "category 6 resolver");

    g_armHooksInstalled = parryArm && repelArm && pushArm;
    g_arm.installed = g_armHooksInstalled;
    if (!g_armHooksInstalled) {
        LOG_WARN("[Defence] native arming unavailable (parry=%d repel=%d push=%d); "
                 "the window mechanics fall back to simulated input",
                 parryArm ? 1 : 0, repelArm ? 1 : 0, pushArm ? 1 : 0);
    }

    g_installed = true;
    return true;
}

void PracticeDefense_Uninstall() {
    if (!g_installed) {
        return;
    }
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENSE_ORDINARY_GUARD));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENSE_NORMAL_HIT));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_COLLISION_GRAB_PHASE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_COLLISION_DAMAGE_PHASE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_COLLISION_SUMMON_PHASE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_ENTITY_CHECK_HIT_STATE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_ENTITY_CHECK_GUARD_STATE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_ENTITY_CHECK_AIR_TECH));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENCE_RESOLVE_COUNTER));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENCE_RESOLVE_PARRY));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENCE_RESOLVE_REPEL));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENCE_RESOLVE_PUSH_AWAY));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENCE_RESOLVE_ABSOLUTE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_DEFENCE_RESOLVE_DODGE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_ENTITY_UPDATE_ACTION_ATTACKS));
    g_installed = false;
    g_ordinaryGuardHooked = false;
    g_armHooksInstalled = false;
    g_arm.installed = false;
}

bool PracticeDefense_IsInstalled() {
    return g_installed;
}

bool PracticeDefense_HasOrdinaryGuardHook() {
    return g_ordinaryGuardHooked;
}

const char* PracticeDefense_GetInstallError() {
    return g_installError;
}

void PracticeDefense_SetConfig(const PracticeDefenseConfig& config) {
    g_config = config;
}

SemanticDirection PracticeDefense_GetAnticipatoryGuard() {
    return g_anticipatoryGuard;
}

bool PracticeDefense_WantsAnticipatoryGuard() {
    return g_anticipatoryWantsGuard;
}

uint32_t PracticeDefense_GetAnticipatoryMask() {
    return g_anticipatoryMask;
}

void PracticeDefense_NoteAnticipatoryFacing(int8_t facing, bool prearmed) {
    g_facingAtInput = facing;
    g_inputPrearmed = prearmed;
}

const PracticeDefenseTelemetry& PracticeDefense_GetTelemetry() {
    return g_telemetry;
}

const PracticeDefenseArmState& PracticeDefense_GetArmState() {
    return g_arm;
}

bool PracticeDefense_ArmHooksActive() {
    return g_armHooksInstalled;
}

void PracticeDefense_OnCommandMatchesEntry(uintptr_t entity) {
    CommandMatchesEntry(entity);
}

void PracticeDefense_OnCommandMatchesExit(uintptr_t entity) {
    CommandMatchesExit(entity);
}

void PracticeDefense_SetCounterGuardRouteAvailable(bool available) {
    if (g_counterGuardRouteAvailable == available) {
        return;
    }
    g_counterGuardRouteAvailable = available;
    LOG_INFO("[Defence] counter-guard route seam %s",
             available ? "available" : "unavailable (preemptive absolute defence "
                                       "falls back to feeding 214D)");
}

bool PracticeDefense_ConsumeDefensiveSuccess(uint32_t* outFrame, uint32_t* outResult) {
    if (!g_defensiveSuccessPending) {
        return false;
    }
    g_defensiveSuccessPending = false;
    if (outFrame) *outFrame = g_defensiveSuccessFrame;
    if (outResult) *outResult = g_defensiveSuccessResult;
    return true;
}

bool PracticeDefense_BlockForcedByResponse() {
    return g_config.enabled &&
           g_config.policy == BlockPolicy::Off &&
           ResponseRequiresBlockstun(
               EffectiveResponse(PracticeDefense_DummyDefenseCategory()));
}

DefenseInputKind PracticeDefense_DefenseInput() {
    return g_defenseInput;
}

int PracticeDefense_ArmStep() {
    return g_defenseDrive.armStep;
}

int PracticeDefense_DummyDefenseCategory() {
    const uint32_t charId = ReadCharacterId(DummyEntity());
    if (charId >= 22) {
        return 0;
    }
    return (int)ReadMemory<uint32_t>(ADDR_DEFENSE_CATEGORY_TABLE + charId * 4);
}

bool PracticeDefense_SequenceActive() {
    return g_sequence.active;
}

uint32_t PracticeDefense_ResolvedContactGroups() {
    return g_sequence.resolvedContactGroups;
}

void PracticeDefense_Reset() {
    g_plan = FrameGuardPlan{};
    g_sequence = AutoBlockSequenceState{};
    memset(&g_outcome, 0, sizeof(g_outcome));
    g_telemetry = PracticeDefenseTelemetry{};
    g_scannedCount = 0;
    g_planEpoch = 0;
    g_phaseEpoch = 1;
    g_lastProcessedSimFrame = 0;
    g_anticipatoryGuard = SemanticDirection::Neutral;
    g_anticipatoryMask = 0;
    g_defenseDrive = DefenseDriveState{};
    g_defenseInput = DefenseInputKind::None;
    g_facingAtInput = 0;
    g_inputPrearmed = false;
    g_lastHitThreatId = 0;
    g_lastHitFrame = 0xFFFFFFFFu;
    g_lastHitEpoch = 0;
    g_lastHitWantedBlock = false;
    g_lastThreatClass = GroundGuardClass::None;
    g_lastDrive = DefenseDriveSample{};
    g_defensiveSuccessPending = false;
    g_defensiveSuccessFrame = 0;
    g_defensiveSuccessResult = 0;
    g_arm = PracticeDefenseArmState{};
    g_arm.installed = g_armHooksInstalled;
}

void PracticeDefense_CaptureState(PracticeDefenseRuntimeState* out) {
    if (!out) {
        return;
    }
    out->sequence = g_sequence;
    out->plan = g_plan;
    out->lastProcessedSimFrame = g_lastProcessedSimFrame;
}

void PracticeDefense_RestoreState(const PracticeDefenseRuntimeState* state) {
    if (!state) {
        return;
    }
    g_sequence = state->sequence;
    g_plan = state->plan;
    g_lastProcessedSimFrame = state->lastProcessedSimFrame;
    // The plan is rebuilt from the restored world on the next collision phase.
    g_planEpoch = 0;
    g_scannedCount = 0;
    memset(&g_outcome, 0, sizeof(g_outcome));
    g_lastHitThreatId = 0;
    g_lastHitFrame = 0xFFFFFFFFu;
    g_lastHitEpoch = 0;
    g_lastHitWantedBlock = false;
    g_telemetry.sequence = g_sequence;
    g_telemetry.plan = g_plan;
}
