#include "patches/render_guard.h"

#include "as2_constants.h"
#include "core/game_state.h"
#include "log_window.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"

#include <stdint.h>

#include "rollback/rollback_session.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace RenderGuard {

MatchRenderPlayers_t g_origMatchRenderPlayers = nullptr;

namespace {

constexpr uintptr_t kMatchP1EntityOffset = 41072;
constexpr uintptr_t kMatchP2EntityOffset = 149884;
constexpr uint32_t kSpriteIndexLimit = 1024;
constexpr uint32_t kAnimIndexLimit = 4096;
constexpr uint32_t kCharIdLimit = 256;
constexpr uint32_t kNeutralSprite = 25;
constexpr uint32_t kNeutralRenderGroup = 3;
constexpr uint32_t kNeutralAnimIndex = 0x20;
constexpr uint32_t kLogCooldownFrames = 30;

struct RenderState {
    uint32_t char_data;
    uint32_t char_id;
    uint32_t main_sprite;
    uint32_t render_group;
    uint32_t overlay_sprite;
    uint8_t overlay_order;
    int16_t overlay_x;
    int16_t overlay_y;
    uint32_t overlay_blend;
    uint8_t overlay_alpha;
    uint8_t flash;
    uint32_t tint;
    uint32_t tint_timer;
    uint32_t anim_index;
};

uint32_t s_lastLogFrame[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
uint32_t s_suppressedLogs[2] = {};

bool IsPlausibleSprite(uint32_t sprite) {
    return sprite < kSpriteIndexLimit;
}

bool IsPlausibleOverlay(uint32_t sprite) {
    return sprite == 0xFFFFFFFFu || IsPlausibleSprite(sprite);
}

uint8_t ReadU8(uintptr_t address, uint8_t fallback = 0xFF) {
    __try {
        return *reinterpret_cast<volatile uint8_t*>(address);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

int16_t ReadI16(uintptr_t address, int16_t fallback = -1) {
    __try {
        return *reinterpret_cast<volatile int16_t*>(address);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

uint32_t ReadU32(uintptr_t address, uint32_t fallback = 0xFFFFFFFFu) {
    __try {
        return *reinterpret_cast<volatile uint32_t*>(address);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

bool IsPlausibleRenderState(const RenderState& state) {
    return state.char_data != 0 &&
           state.char_id < kCharIdLimit &&
           IsPlausibleSprite(state.main_sprite) &&
           IsPlausibleOverlay(state.overlay_sprite) &&
           state.anim_index < kAnimIndexLimit;
}

RenderState ReadRenderState(uintptr_t entity) {
    RenderState state{};
    if (!entity) {
        return state;
    }

    state.char_data = ReadU32(entity, 0);
    state.char_id = state.char_data ? ReadU32(state.char_data + 176, 0xFFFFFFFFu) : 0xFFFFFFFFu;
    state.main_sprite = ReadU32(entity + ENTITY_OFF_RENDER_MAIN_SPRITE);
    state.render_group = ReadU32(entity + ENTITY_OFF_RENDER_GROUP);
    state.overlay_sprite = ReadU32(entity + ENTITY_OFF_RENDER_OVERLAY_SPRITE);
    state.overlay_order = ReadU8(entity + ENTITY_OFF_RENDER_OVERLAY_ORDER);
    state.overlay_x = ReadI16(entity + ENTITY_OFF_RENDER_OVERLAY_X);
    state.overlay_y = ReadI16(entity + ENTITY_OFF_RENDER_OVERLAY_Y);
    state.overlay_blend = ReadU32(entity + ENTITY_OFF_RENDER_OVERLAY_BLEND);
    state.overlay_alpha = ReadU8(entity + ENTITY_OFF_RENDER_OVERLAY_ALPHA);
    state.flash = ReadU8(entity + ENTITY_OFF_RENDER_FLASH_FLAG);
    state.tint = ReadU32(entity + ENTITY_OFF_RENDER_TINT_STATE);
    state.tint_timer = ReadU32(entity + ENTITY_OFF_RENDER_TINT_TIMER);
    state.anim_index = ReadU32(entity + ENTITY_OFF_ANIM_INDEX);
    return state;
}

void WriteNeutralRenderState(uintptr_t entity) {
    WriteMemory<uint32_t>(entity + ENTITY_OFF_RENDER_MAIN_SPRITE, kNeutralSprite);
    WriteMemory<uint32_t>(entity + ENTITY_OFF_RENDER_GROUP, kNeutralRenderGroup);
    WriteMemory<uint32_t>(entity + ENTITY_OFF_RENDER_OVERLAY_SPRITE, 0xFFFFFFFFu);
    WriteMemory<uint8_t>(entity + ENTITY_OFF_RENDER_OVERLAY_ORDER, 0);
    WriteMemory<int16_t>(entity + ENTITY_OFF_RENDER_OVERLAY_X, -1);
    WriteMemory<int16_t>(entity + ENTITY_OFF_RENDER_OVERLAY_Y, -1);
    WriteMemory<uint32_t>(entity + ENTITY_OFF_RENDER_OVERLAY_BLEND, 0);
    WriteMemory<uint8_t>(entity + ENTITY_OFF_RENDER_OVERLAY_ALPHA, 0xFF);
    WriteMemory<uint8_t>(entity + ENTITY_OFF_RENDER_FLASH_FLAG, 0);
    WriteMemory<uint32_t>(entity + ENTITY_OFF_RENDER_TINT_STATE, 1);
    WriteMemory<uint32_t>(entity + ENTITY_OFF_RENDER_TINT_TIMER, 0);
    WriteMemory<uint32_t>(entity + ENTITY_OFF_ANIM_INDEX, kNeutralAnimIndex);
}

void LogCorrection(int slot,
                   uintptr_t entity,
                   const RenderState& before,
                   const RenderState& after,
                   const char* reason) {
    const uint32_t frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
    const bool shouldLog = s_lastLogFrame[slot] == 0xFFFFFFFFu ||
                           frame < s_lastLogFrame[slot] ||
                           frame - s_lastLogFrame[slot] >= kLogCooldownFrames;

    if (!shouldLog) {
        ++s_suppressedLogs[slot];
        return;
    }

    Rollback::NetplayLog_Write(
        "RENDERGUARD", (int32_t)frame,
        "corrected slot=P%d entity=0x%08X reason=%s char_data=0x%08X char=%u "
        "main=0x%08X->0x%08X group=%u->%u overlay=0x%08X->0x%08X "
        "order=%u->%u ovxy=(%d,%d)->(%d,%d) blend=0x%08X->0x%08X alpha=%u->%u "
        "flash=%u->%u tint=%u/%u->%u/%u anim=0x%08X->0x%08X mode=%u sub=%u suppressed=%u",
        slot + 1,
        (unsigned)entity,
        reason ? reason : "invalid render state",
        before.char_data,
        before.char_id,
        before.main_sprite,
        after.main_sprite,
        before.render_group,
        after.render_group,
        before.overlay_sprite,
        after.overlay_sprite,
        before.overlay_order,
        after.overlay_order,
        before.overlay_x,
        before.overlay_y,
        after.overlay_x,
        after.overlay_y,
        before.overlay_blend,
        after.overlay_blend,
        before.overlay_alpha,
        after.overlay_alpha,
        before.flash,
        after.flash,
        before.tint,
        before.tint_timer,
        after.tint,
        after.tint_timer,
        before.anim_index,
        after.anim_index,
        GetGameMode(),
        GetSubstate(),
        s_suppressedLogs[slot]);
    LOG_WARN("[RENDERGUARD] Corrected P%d render state: main=0x%08X overlay=0x%08X anim=0x%08X reason=%s",
             slot + 1,
             before.main_sprite,
             before.overlay_sprite,
             before.anim_index,
             reason ? reason : "invalid render state");

    s_lastLogFrame[slot] = frame;
    s_suppressedLogs[slot] = 0;
}

const char* ClassifyInvalidState(const RenderState& state) {
    if (state.char_data == 0) {
        return "bad character data pointer";
    }
    if (state.char_id >= kCharIdLimit) {
        return "bad character id";
    }
    if (!IsPlausibleSprite(state.main_sprite)) {
        return "bad main sprite";
    }
    if (!IsPlausibleOverlay(state.overlay_sprite)) {
        return "bad overlay sprite";
    }
    if (state.anim_index >= kAnimIndexLimit) {
        return "bad animation index";
    }
    return "invalid render state";
}

bool SanitizeEntity(int slot, uintptr_t entity) {
    if (!entity || slot < 0 || slot > 1) {
        return true;
    }

    const RenderState before = ReadRenderState(entity);
    if (IsPlausibleRenderState(before)) {
        return true;
    }

    const char* reason = ClassifyInvalidState(before);
    if (before.char_data == 0 || before.char_id >= kCharIdLimit) {
        LogCorrection(slot, entity, before, before, reason);
        return false;
    }

    WriteNeutralRenderState(entity);
    const RenderState after = ReadRenderState(entity);
    LogCorrection(slot, entity, before, after, reason);
    return IsPlausibleRenderState(after);
}

bool SanitizeMatch(int match) {
    if (!match) {
        return true;
    }

    const uintptr_t matchBase = (uintptr_t)match;
    const bool p1Safe = SanitizeEntity(0, matchBase + kMatchP1EntityOffset);
    const bool p2Safe = SanitizeEntity(1, matchBase + kMatchP2EntityOffset);
    return p1Safe && p2Safe;
}

} // namespace

int __cdecl Hook_MatchRenderPlayers(int match, char timerBit) {
    if (!g_origMatchRenderPlayers) {
        return 0;
    }

    // Is the vanilla player renderer being driven during rollback REPLAY
    // ticks? EfzRevival/qoh99 both suppress render work during replay; if we
    // do not, a depth-30 transaction issues 30 sets of draws that composite
    // into the single presented buffer — which is what several combo digits
    // on screen at once would look like. Measure before assuming.
    {
        static uint32_t s_replayCalls = 0;
        static uint32_t s_liveCalls = 0;
        static DWORD s_lastMs = 0;
        if (Rollback::RollbackSession_IsRollingBack()) ++s_replayCalls; else ++s_liveCalls;
        const DWORD now = GetTickCount();
        if (s_lastMs == 0) s_lastMs = now;
        if ((DWORD)(now - s_lastMs) >= 1000) {
            s_lastMs = now;
            Rollback::NetplayLog_Write("RENDERGUARD", -1,
                "render calls/s: during_replay=%u live=%u",
                s_replayCalls, s_liveCalls);
            s_replayCalls = 0;
            s_liveCalls = 0;
        }
    }

    __try {
        if (!SanitizeMatch(match)) {
            const uint32_t frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
            Rollback::NetplayLog_Write(
                "RENDERGUARD", (int32_t)frame,
                "skipped vanilla renderer: unrecoverable entity render context match=0x%08X timer=%d mode=%u sub=%u",
                (unsigned)match,
                (int)(uint8_t)timerBit,
                GetGameMode(),
                GetSubstate());
            return 0;
        }
        return g_origMatchRenderPlayers(match, timerBit);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        const uint32_t frame = ReadMemory<uint32_t>(ADDR_SIM_FRAME_COUNTER);
        Rollback::NetplayLog_Write(
            "RENDERGUARD", (int32_t)frame,
            "ERROR vanilla renderer exception swallowed match=0x%08X timer=%d mode=%u sub=%u",
            (unsigned)match,
            (int)(uint8_t)timerBit,
            GetGameMode(),
            GetSubstate());
        LOG_ERROR("[RENDERGUARD] Vanilla renderer exception swallowed at frame %u", frame);
        return 0;
    }
}

} // namespace RenderGuard
