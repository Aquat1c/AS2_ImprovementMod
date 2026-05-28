/**
 * Alice Senki 2 - Session Pump Hook
 *
 * Keeps the online session drain alive during blocking native asset loads.
 * The win-screen loader can spend several seconds inside one mode-handler
 * frame, before EndScene/ModOnFrame gets a chance to call Session_Update().
 */
#pragma once

namespace Net {

using AssetLoadFromArchive_t = int(__cdecl *)(const char*, char*, int, int);

extern AssetLoadFromArchive_t g_origAssetLoadFromArchive;

void SessionPumpHook_Init();
void SessionPumpHook_Shutdown();

int __cdecl Hook_Asset_LoadFromArchive(const char* archive,
                                        char* patch,
                                        int assetIndex,
                                        int patchIndex);

} // namespace Net
