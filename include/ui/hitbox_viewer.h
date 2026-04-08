/**
 * Alice Senki 2 — Hitbox / Hurtbox / Collision Viewer
 *
 * Derived entirely from decompilation of Entity_UpdateHitDetection,
 * HitDef_Create, Weather_UpdateScroll, and sprite rendering functions.
 *
 * Three box systems are visualised:
 *   1. Per-animation hurtboxes   (entity + ANIM_DATA + 104*frameIdx + 8)
 *   2. Global HitDef array       (ADDR_SUMMON_ARRAY, 100 × 272 bytes)
 *   3. Pushbox / active rect     (entity + 0x6A0)
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
