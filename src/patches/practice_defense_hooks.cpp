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

bool PracticeGateOpen() {
    if (!g_config.enabled) return false;
    if (g_config.policy == BlockPolicy::Off) return false;
    if (g_config.controlSwapActive) return false;
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
    state.controlSwapActive = g_config.controlSwapActive;
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
            SequenceEnd(g_sequence);
        }
    }

    PlanFinalize(g_plan, g_config.preferCrouch);
    g_plan.contactOrdinal = g_sequence.resolvedContactGroups;
    g_plan.policyWantsBlock =
        g_sequence.active && PolicyWantsBlock(g_config.policy, g_sequence, g_plan.contactOrdinal);

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
    const bool policyWould = g_config.policy != BlockPolicy::Off &&
                             PolicyWantsBlock(g_config.policy, g_sequence, anticipatedOrdinal);

    if (policyWould && CanAutoGuardAtContact(defenderState)) {
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
            PolicyWantsBlock(g_config.policy, g_sequence, g_plan.contactOrdinal);
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

    const bool augment = g_plan.policyWantsBlock && eligible && compatible && !nativeAlready;

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
    g_installed = false;
    g_ordinaryGuardHooked = false;
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
    g_facingAtInput = 0;
    g_inputPrearmed = false;
    g_lastHitThreatId = 0;
    g_lastHitFrame = 0xFFFFFFFFu;
    g_lastHitEpoch = 0;
    g_lastHitWantedBlock = false;
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
