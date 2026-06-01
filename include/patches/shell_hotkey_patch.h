#pragma once

// Shell hotkey policy: mod/docs/SHELL_HOTKEY_POLICY.md
//
// DO NOT add SC_TASKLIST, ActivateKeyboardLayout, SendInput, or d3d9-only
// DefWindowProc bypasses for Win/Alt+Shift. Route messages through hooked sub_633490.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

bool ShellHotkeyPatch_Install();
void ShellHotkeyPatch_Remove();

// d3d9_proxy must call this instead of CallWindowProcW(DXLib 0xFFFFxxxx stub).
// Returns false if hook not installed; *outResult valid when true.
bool ShellHotkey_CallGameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* outResult);

bool ShellHotkey_IsHookInstalled();
