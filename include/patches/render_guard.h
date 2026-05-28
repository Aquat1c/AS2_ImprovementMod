#pragma once

namespace RenderGuard {

using MatchRenderPlayers_t = int (__cdecl *)(int match, char timerBit);

extern MatchRenderPlayers_t g_origMatchRenderPlayers;

int __cdecl Hook_MatchRenderPlayers(int match, char timerBit);

} // namespace RenderGuard
