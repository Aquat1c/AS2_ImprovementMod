/**
 * Alice Senki 2 — Hitbox / Hurtbox / Extended Hurtbox / Collision Viewer
 *
 * Derived from reverse-engineering notes for Entity_UpdateHitDetection,
 * Entity_UpdateGrabAlignment, Entity_UpdateDamageApplication,
 * Entity_UpdateThrowInteraction, Entity_ResolveAttackCollision,
 * Entity_ResolveBodyCollision, Entity_UpdateSummonHitDetection,
 * HitDef_Create, Weather_UpdateScroll, and sprite rendering functions.
 *
 * Five box systems are visualised:
 *   1. Pushbox / collision box   (@0)  — body push + clash target
 *   2. Attack hitboxes           (@8)  — melee + throw attacker side
 *   3. Hurtboxes                 (@40) — universal vulnerability (melee + summons)
 *   4. Extended hurtboxes        (@72) — melee-only vulnerability + tech throw
 *   5. Global HitDef array       (ADDR_SUMMON_ARRAY, 100 × 272 bytes)
 *
 * Player-vs-player melee detection pipeline (game loop order):
 *   Entity_UpdateGrabAlignment:  attacker hitbox@8 vs defender hurtbox@40
 *   Entity_UpdateDamageApplication: attacker hitbox@8 vs defender ext-hurtbox@72
 *   Both dispatch to same character handler table (dword_73E070).
 *
 * Entity state indicators:
 *   - ATK:         entity+0x6C8 == 1 && entity+0x6D4 != 0
 *   - CLASH:       entity+0x77C rank, entity+0x788 continuation ID
 *   - MAX HIT:     entity+0x78C/+0x78E/+0x790 raw max-hit lanes
 *   - INVINCIBLE:  legacy defender gate at entity+0x78C (the source the older viewer used)
 *                  plus direct defense bits from entity+0x794 (0x2000 melee, 0x4000 projectile)
 *                  and marker-gated special states from ((entity+0x788)|(entity+0x794)) & (0x10000|0x8000)
 *                  when entity+0x79C != 0. entity+0x79D is tracked separately in the info overlay.
 *   - CONTACT OVERRIDE: entity+0x6CC & 0x20000 literal collision override that bypasses @40/@72
 *                  box-size / overlap checks; kept out of the main HUD labels because no cleaner
 *                  in-game mechanic name is verified yet.
 *   - PROJ IMMUNE: entity+0x6CC & 0x800
 *   - BREAK:       command-20 timer at entity+0x688. The HUD label follows the
 *                  active window itself, while `Entity_TryAction_Super1` gates stay
 *                  in the info panel as "usable now" debug.
 *   - LIGHT BREAK: command-21 timer at entity+0x689. `0x689` is the Orange /
 *                  Light Break countdown window, not a mechanic ID by itself;
 *                  `Entity_TryAction_Super2` gates are tracked separately in debug.
 *   - CANCEL:      older action-107 / 49 / 52 / 59-62 routes, kept as debug.
 */

#pragma once

/// Call once at mod startup.
void HitboxViewer_Init();

/// Call every frame from the overlay render path (after game render,
/// before ImGui's own EndFrame).  Uses ImGui foreground draw-list.
void HitboxViewer_Render();

/// Render ImGui controls (checkboxes, colour pickers, etc.)
/// inside the mod-menu window.
void HitboxViewer_RenderControls();

/// Programmatic enable/disable (for hotkey toggle).
void HitboxViewer_SetEnabled(bool enabled);
bool HitboxViewer_IsEnabled();
void HitboxViewer_ToggleEnabled();
