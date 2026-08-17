# Kill-path audit gate (plan M6 / INV-1).
#
# A session may only be terminated by:
#   (a) the ConnectionSupervisor declaring Dead,
#   (b) an explicit user action (decline/cancel/quit, local or remote),
#   (c) hard incompatibility at handshake.
#
# This gate fails when a NEW caller of the terminal funnel
# (HandleDisconnection / OpenDisconnectError) appears outside the allowlist,
# so heuristic kill paths cannot silently creep back in. Update the allowlist
# only with a justification comment.
#
# Usage: powershell -File tools/check_killpaths.ps1   (exit 0 = pass)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# file => expected max call-site count of HandleDisconnection|OpenDisconnectError.
# Counts include definitions/declarations in the controller itself.
$allow = @{
    # The funnel's own definition + menu-local user actions + supervisor entry.
    "src\net\netplay_menu_controller.cpp" = 99   # funnel owner (definitions + internal routing)
    # Supervisor: the ONLY network-originated teardown (INV-1a / re0.7 INV-12).
    # Two typed terminals since M3: silence Dead + ProgressDeadline (§2.4).
    "src\net\connection_supervisor.cpp"   = 2
    # session2 (M3): PacingClockDead — LOCAL fail-closed terminal (INV-20,
    # M2 obligation), not network-originated; routed through Session2_Terminate
    # first, then the UI funnel.
    "src\net\session2.cpp"                = 1
    # User actions (INV-1b):
    "src\net\pause_handler.cpp"           = 1    # remote quit from pause menu
    "src\net\mode_ownership.cpp"          = 2    # user left charsel / game left netplay context
    # match_setup (M5, replaces pregame_sync): SetPhase(Error) is reserved
    # for genuine fail-closed terminals only (config validation failure,
    # second baseline mismatch → ConfirmedDesync via Session2_Terminate);
    # every recoverable path routes through RestartPregame on the live
    # connection (INV-12). Second site = session-already-lost UI guard.
    "src\net\match_setup.cpp"             = 2
    # Legacy heuristic callers being burned down by M6 — DO NOT ADD, only remove:
    "src\net\match_lifecycle.cpp"         = 1    # session-lost guard (M6 target)
    "src\patches\input_override.cpp"      = 1    # AbortRollbackDispatcher (M6 target)
}

$pattern = "(NetMenu::)?(HandleDisconnection|OpenDisconnectError)\s*\("
$fail = $false

$files = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.cpp
foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length + 1)
    # Count actual call sites only: skip comment lines and declarations.
    $hits = @(Select-String -Path $f.FullName -Pattern $pattern |
              Where-Object { $_.Line.Trim() -notmatch '^(//|\*|void\s)' })
    if ($hits.Count -eq 0) { continue }
    if (-not $allow.ContainsKey($rel)) {
        Write-Host "FAIL: $rel has $($hits.Count) kill-path call(s) but is not in the allowlist:" -ForegroundColor Red
        $hits | ForEach-Object { Write-Host ("  {0}:{1}: {2}" -f $rel, $_.LineNumber, $_.Line.Trim()) }
        $fail = $true
        continue
    }
    if ($hits.Count -gt $allow[$rel]) {
        Write-Host "FAIL: $rel has $($hits.Count) kill-path call(s), allowlist permits $($allow[$rel]):" -ForegroundColor Red
        $hits | ForEach-Object { Write-Host ("  {0}:{1}: {2}" -f $rel, $_.LineNumber, $_.Line.Trim()) }
        $fail = $true
    }
}

if ($fail) {
    Write-Host "`nKill-path gate FAILED. Session teardown must go through the ConnectionSupervisor (INV-1); see docs/AS2_0_7_Netplay_Session_Resilience_Rework_Plan.md." -ForegroundColor Red
    exit 1
}
Write-Host "Kill-path gate passed." -ForegroundColor Green
exit 0
