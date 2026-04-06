/**
 * Alice Senki 2 - Hitbox/Hurtbox/Collision Display
 * Debug visualization for combat boxes
 */

#pragma once

#include <stdint.h>

// Initialize hitbox display system
void HitboxDisplay_Init();

// Render hitbox overlays (call after game render, before ImGui)
// Draws boxes in world coordinates, transformed to screen space
void HitboxDisplay_Render();

// Toggle display on/off
void HitboxDisplay_SetEnabled(bool enabled);
bool HitboxDisplay_IsEnabled();

// Individual box type toggles
void HitboxDisplay_SetShowHitboxes(bool show);
void HitboxDisplay_SetShowHurtboxes(bool show);
void HitboxDisplay_SetShowCollision(bool show);
void HitboxDisplay_SetShowPushbox(bool show);

bool HitboxDisplay_GetShowHitboxes();
bool HitboxDisplay_GetShowHurtboxes();
bool HitboxDisplay_GetShowCollision();
bool HitboxDisplay_GetShowPushbox();

// Render ImGui controls for hitbox settings
void HitboxDisplay_RenderControls();
