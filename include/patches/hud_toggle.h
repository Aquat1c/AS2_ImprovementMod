#pragma once

#include <stdbool.h>

// Per-element HUD hiding for training mode.
//
// The match HUD is one function, sub_4C05B0 at 0x4C05B0, which draws every
// element inline - 36 textured quads and 15 sprites, no per-element sub-calls to
// hook. What it does have is hard-coded screen coordinates, one distinct band
// per element, so the elements are separable even though the code is not:
//
//   quad y1 =  4  recording indicator      sprite y =  0  HUD frame
//   quad y1 = 22  health bars              sprite y = 20  timer frame
//   quad y1 = 41  guard gauges             sprite y = 40  portraits
//   quad y1 = 63  round marks              sprite y = 50  timer digits
//   quad y1 = 68  round marks              sprite y = 58  special gauge
//   quad y1 = 72  special gauge            sprite y = 69  special gauge
//   quad y1 = 85  name plates              sprite y = 421 meter frame
//   quad y1 = 460 meter bars               sprite y = 449 meter level
//
// So the seam is: hook the HUD function to know when we are inside it, and hook
// the two render primitives to drop the draws belonging to a hidden element.
// Skipping a draw is safe - the HUD ignores their return values.
//
// The combo/score popups are a separate function (sub_4C1F90) and get their
// draws filtered the same way, because that one also latches state.
//
// sub_4C7F30 is the SUPER CUT-IN, not the status callouts. The decomp settles
// it: it opens by drawing the full-width darkening band at y=80..299, alpha
// 0xC0, then the per-entity portraits over it. Hiding it removes the superflash.
//
// It was called "Status Callouts" here, and that name cost real time: hiding it
// removed the superflash while the "super"/"brake" text it was supposed to hide
// stayed on screen, because that text comes from somewhere else entirely. If
// the status words are ever wanted as a toggle, find their real source first -
// do not attach it to this.
//
// It is pure render, but it draws through sub_61A960 for the mirrored
// (facing-left) case, which is not a primitive this module hooks, so it is
// skipped whole rather than filtered.

enum HudElement {
    HUD_ELEM_HEALTH = 0,
    HUD_ELEM_METER,
    HUD_ELEM_GUARD,
    HUD_ELEM_TIMER,
    HUD_ELEM_PORTRAITS,
    HUD_ELEM_ROUNDS,
    HUD_ELEM_SPECIAL,
    HUD_ELEM_FRAME,
    HUD_ELEM_COMBO,
    HUD_ELEM_CUTIN,
    HUD_ELEM_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif

// sub_4C05B0 is already hooked by netplay_hud_vanilla for the nickname bars,
// and MinHook allows one hook per target - a second MH_CreateHook on it fails.
// So that hook calls these around the original instead of this module fighting
// it for the address.
void HudToggle_BeginHudRender(void);
void HudToggle_EndHudRender(void);

// True while a super cut-in is on screen (either entity +1244 set). Diagnostic
// only: lets the effect journal label what spawns during one.
bool HudToggle_IsCutInActive(void);

bool HudToggle_Install(void);
void HudToggle_Uninstall(void);
bool HudToggle_IsInstalled(void);

// Per element, because they do not all depend on the same hooks: the status
// callouts are a whole-function skip while the rest need the primitive filter.
bool HudToggle_ElementAvailable(int element);

bool HudToggle_IsHidden(int element);
void HudToggle_SetHidden(int element, bool hidden);
void HudToggle_Reset(void);

// True when at least one element is hidden, for a summary row.
int  HudToggle_HiddenCount(void);

const char* HudToggle_Name(int element);

#ifdef __cplusplus
}
#endif
