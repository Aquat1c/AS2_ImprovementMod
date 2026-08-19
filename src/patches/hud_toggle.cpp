#include "patches/hud_toggle.h"

#include "core/game_state.h"
#include "as2_constants.h"
#include "log_window.h"
#include "MinHook.h"

#include <stdio.h>
#include <string.h>

namespace {

typedef int (__cdecl* DrawQuad_t)(long, int, long, int, long, int, long, int, int, int);
typedef int (__cdecl* DrawSprite_t)(int, int, int, int);
typedef int (__cdecl* ComboRender_t)(int);
typedef int (__cdecl* StatusRender_t)(int*);

DrawQuad_t    g_origDrawQuad = nullptr;
DrawSprite_t  g_origDrawSprite = nullptr;
ComboRender_t g_origComboRender = nullptr;
StatusRender_t g_origStatusRender = nullptr;

bool g_installed = false;
bool g_hidden[HUD_ELEM_COUNT] = {};

// Only true while the corresponding render function is on the stack, so the
// primitive hooks cost one predictable branch everywhere else in the frame.
bool g_inHud = false;
bool g_inCombo = false;

// True across a super cut-in, read by the effect journal so the ids spawned
// during one can be told apart from ordinary combat effects.
bool g_cutInActive = false;

const char* const kNames[HUD_ELEM_COUNT] = {
    "Health Bars",
    "Meter Bars",
    "Guard Gauge",
    "Timer",
    "Portraits",
    "Round Marks",
    "Special Gauge",
    "HUD Frame",
    "Combo Display",
    "Super Cut-In",
};

// Training only: hiding the HUD in a real match would be a competitive change,
// and none of these settings are synchronised.
bool FilterActive() {
    return g_installed && GetGameType() == GAMETYPE_TRAINING;
}

bool Hidden(int element) {
    return element >= 0 && element < HUD_ELEM_COUNT && g_hidden[element];
}

// The y band a quad belongs to. -1 for anything the HUD draws that we do not
// offer a toggle for, which is left alone.
int QuadElement(int y1) {
    switch (y1) {
        case 22:  return HUD_ELEM_HEALTH;
        case 41:  return HUD_ELEM_GUARD;
        case 63:
        case 68:  return HUD_ELEM_ROUNDS;
        case 72:  return HUD_ELEM_SPECIAL;
        case 460: return HUD_ELEM_METER;
        default:  return -1;
    }
}

int SpriteElement(int y) {
    switch (y) {
        case 0:   return HUD_ELEM_FRAME;
        case 20:
        case 50:  return HUD_ELEM_TIMER;
        case 40:  return HUD_ELEM_PORTRAITS;
        case 58:
        case 69:  return HUD_ELEM_SPECIAL;
        case 421:
        case 449: return HUD_ELEM_METER;
        default:  return -1;
    }
}

// Every actual drop is logged, rate-limited per element. Without this the logs
// could only ever say what the gates DECIDED, never what they DID, so a report
// of something missing could not be pinned on this filter or cleared of it.
uint32_t g_suppressCount[HUD_ELEM_COUNT] = {};

void NoteSuppression(int element, const char* kind, int detail) {
    if (element < 0 || element >= HUD_ELEM_COUNT) {
        return;
    }
    const uint32_t n = ++g_suppressCount[element];
    if (n <= 3u || (n % 600u) == 0u) {
        LOG_INFO("[HudToggle] DROPPED %s (%s detail=%d) count=%u",
                 kNames[element], kind, detail, n);
    }
}

bool SuppressDraw(int element) {
    return FilterActive() && Hidden(element);
}

int __cdecl Hook_DrawQuad(long x1, int y1, long x2, int y2, long x3, int y3,
                          long x4, int y4, int texture, int flags) {
    if (g_inHud && SuppressDraw(QuadElement(y1))) {
        NoteSuppression(QuadElement(y1), "quad y1", y1);
        return 0;
    }
    if (g_inCombo && SuppressDraw(HUD_ELEM_COMBO)) {
        NoteSuppression(HUD_ELEM_COMBO, "combo quad y1", y1);
        return 0;
    }
    return g_origDrawQuad(x1, y1, x2, y2, x3, y3, x4, y4, texture, flags);
}

int __cdecl Hook_DrawSprite(int x, int y, int sprite, int flags) {
    if (g_inHud && SuppressDraw(SpriteElement(y))) {
        NoteSuppression(SpriteElement(y), "sprite y", y);
        return 0;
    }
    if (g_inCombo && SuppressDraw(HUD_ELEM_COMBO)) {
        NoteSuppression(HUD_ELEM_COMBO, "combo sprite y", y);
        return 0;
    }
    return g_origDrawSprite(x, y, sprite, flags);
}

int __cdecl Hook_ComboRender(int match) {
    // Not skipped outright: this function also latches per-entity display state
    // at entity +420..+424 for both players, so suppressing the call would
    // leave that stale. Only its draws are dropped.
    const bool prev = g_inCombo;
    g_inCombo = true;
    const int result = g_origComboRender(match);
    g_inCombo = prev;
    return result;
}

// Cut-in diagnostic.
//
// sub_4C7F30 does exactly two things, and the decomp pins both to the same
// gate. The darkening band is
//
//   if (match[10579] || match[37782]) {
//       Render_SetBlendMode(1, 0xC0); Render_SetDrawColor(0, 0, 0);
//       Render_DrawTexturedQuad(0, 80, 639, 80, 639, 299, 0, 299, match[191], 1);
//   }
//
// and the portraits are gated per entity on the same word. match[10579] is
// P1 entity +1244 and match[37782] is P2 entity +1244 (entity bases 41072 and
// 149884 inside match), set by Entity_CheckPriority.
//
// match[191] is the handle table loaded by sub_4A9280 as
// Asset_LoadAllFromArchive(match + 764, "data\eft.bin", "data\eft.pal", 0) -
// so it is eft.bin sprite 0. If that handle is 0 or -1 the band draws nothing
// while the portraits still appear, which is what a missing backdrop looks
// like. This logs the gate words and the handle on every cut-in activation.
void LogCutInActivation(int* match) {
    if (!match) {
        return;
    }
    const int p1Gate = match[10579];
    const int p2Gate = match[37782];
    const bool active = (p1Gate != 0) || (p2Gate != 0);

    g_cutInActive = active;

    static bool s_wasActive = false;
    if (active == s_wasActive) {
        return;
    }
    s_wasActive = active;
    if (!active) {
        return;
    }

    LOG_INFO("[CutIn] activated: p1_1244=%d p2_1244=%d band_handle(eft[0])=0x%08X "
             "eft[1]=0x%08X eft[2]=0x%08X",
             p1Gate, p2Gate,
             (unsigned)match[191], (unsigned)match[192], (unsigned)match[193]);
}

int __cdecl Hook_StatusRender(int* match) {
    LogCutInActivation(match);

    // Safe to skip outright, unlike the combo display: this one only reads. It
    // has to be skipped rather than filtered because the mirrored draw goes
    // through sub_61A960, which is not one of the primitives hooked here.
    const bool suppress = SuppressDraw(HUD_ELEM_CUTIN);
    // One line per change, not per frame. The game type is part of the key so
    // entering a non-training match emits a fresh line: without it the log
    // proves nothing about whether this hook runs outside training, which is
    // exactly the question a missing cut-in there raises.
    static int s_lastLogged = -1;
    const uint32_t gameType = GetGameType();
    const int state = (int)suppress * 1000 +
                      (int)Hidden(HUD_ELEM_CUTIN) * 100 +
                      (int)gameType;
    if (state != s_lastLogged) {
        s_lastLogged = state;
        LOG_INFO("[HudToggle] status callout reached: hidden=%d gametype=%u training=%d -> %s",
                 Hidden(HUD_ELEM_CUTIN) ? 1 : 0,
                 gameType,
                 gameType == GAMETYPE_TRAINING ? 1 : 0,
                 suppress ? "SKIPPED" : "drawn");
    }
    if (suppress) {
        return 0;
    }
    return g_origStatusRender(match);
}

bool CreateAndEnable(uintptr_t target, void* detour, void** original, const char* label) {
    if (MH_CreateHook(reinterpret_cast<void*>(target), detour, original) != MH_OK) {
        LOG_ERROR("[HudToggle] %s create failed @ 0x%08X", label, (unsigned)target);
        return false;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(target)) != MH_OK) {
        LOG_ERROR("[HudToggle] %s enable failed @ 0x%08X", label, (unsigned)target);
        return false;
    }
    LOG_INFO("[HudToggle] hooked %s @ 0x%08X", label, (unsigned)target);
    return true;
}

} // namespace

bool HudToggle_Install(void) {
    if (g_installed) {
        return true;
    }

    const bool quad = CreateAndEnable(ADDR_RENDER_DRAW_QUAD,
                                      reinterpret_cast<void*>(&Hook_DrawQuad),
                                      reinterpret_cast<void**>(&g_origDrawQuad),
                                      "draw quad");
    const bool sprite = CreateAndEnable(ADDR_RENDER_DRAW_SPRITE,
                                        reinterpret_cast<void*>(&Hook_DrawSprite),
                                        reinterpret_cast<void**>(&g_origDrawSprite),
                                        "draw sprite");
    // The combo toggle rides the same primitive filter, so it is only useful
    // alongside them.
    CreateAndEnable(ADDR_COMBO_DISPLAY_RENDER,
                    reinterpret_cast<void*>(&Hook_ComboRender),
                    reinterpret_cast<void**>(&g_origComboRender),
                    "combo display");
    CreateAndEnable(ADDR_STATUS_CALLOUT_RENDER,
                    reinterpret_cast<void*>(&Hook_StatusRender),
                    reinterpret_cast<void**>(&g_origStatusRender),
                    "status callouts");

    // Both primitives or neither: with only one of them the filter would drop
    // half of an element and leave the rest on screen.
    if (!(quad && sprite)) {
        LOG_ERROR("[HudToggle] per-element hiding unavailable; leaving the HUD alone");
        if (quad)   MH_DisableHook(reinterpret_cast<void*>(ADDR_RENDER_DRAW_QUAD));
        if (sprite) MH_DisableHook(reinterpret_cast<void*>(ADDR_RENDER_DRAW_SPRITE));
        g_origDrawQuad = nullptr;
        g_origDrawSprite = nullptr;
    }

    g_installed = true;
    return true;
}

void HudToggle_Uninstall(void) {
    if (!g_installed) {
        return;
    }
    MH_DisableHook(reinterpret_cast<void*>(ADDR_RENDER_DRAW_QUAD));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_RENDER_DRAW_SPRITE));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_COMBO_DISPLAY_RENDER));
    MH_DisableHook(reinterpret_cast<void*>(ADDR_STATUS_CALLOUT_RENDER));
    g_installed = false;
    g_inHud = false;
    g_inCombo = false;
}

void HudToggle_BeginHudRender(void) {
    g_inHud = true;
}

void HudToggle_EndHudRender(void) {
    g_inHud = false;
}

bool HudToggle_IsInstalled(void) {
    // Only meaningful with the primitive filter behind it.
    return g_installed && g_origDrawQuad && g_origDrawSprite;
}

// The word sprites ("Super", "Brake", ...) are queued effects, spawned per
// player with the caster's facing so they land on that player's own side.
// rollback_status_fx already classifies them for its journal, and its ranges are
// the authority here: 166..190 are the prm status-message sprites and 231..239
// the super/cut-in effects.
//
// The effect queue draw (sub_4AB0F0) is one function for every effect in the
// game and is too big for the decompiler to show, so there is nothing to filter
// inside it. Dropping the spawn is the seam: an effect that was never queued is
// indistinguishable from one that already expired.
bool HudToggle_ElementAvailable(int element) {
    if (element < 0 || element >= HUD_ELEM_COUNT) {
        return false;
    }
    if (element == HUD_ELEM_CUTIN) {
        return g_origStatusRender != nullptr;
    }
    return g_origDrawQuad != nullptr && g_origDrawSprite != nullptr;
}

bool HudToggle_IsCutInActive(void) {
    return g_cutInActive;
}

bool HudToggle_IsHidden(int element) {
    return Hidden(element);
}

void HudToggle_SetHidden(int element, bool hidden) {
    if (element < 0 || element >= HUD_ELEM_COUNT) {
        return;
    }
    // The cut-in is a whole-function skip and stands on its own; the rest ride
    // the primitive filter, so without it they cannot be honoured.
    if (element == HUD_ELEM_CUTIN) {
        if (!g_origStatusRender) {
            return;
        }
    } else if (!g_origDrawQuad || !g_origDrawSprite) {
        return;
    }
    g_hidden[element] = hidden;
    LOG_INFO("[HudToggle] %s -> %s", kNames[element], hidden ? "Hidden" : "Shown");
}

void HudToggle_Reset(void) {
    memset(g_hidden, 0, sizeof(g_hidden));
}

int HudToggle_HiddenCount(void) {
    int count = 0;
    for (int i = 0; i < HUD_ELEM_COUNT; ++i) {
        if (g_hidden[i]) {
            ++count;
        }
    }
    return count;
}

const char* HudToggle_Name(int element) {
    if (element < 0 || element >= HUD_ELEM_COUNT) {
        return "?";
    }
    return kNames[element];
}
