#!/usr/bin/env bash
# Alice Senki 2 ImprovementMod - Linux launcher.
#
# Starts the game under Wine with the DLL overrides the mod needs, so nobody
# has to open winecfg. Run it from the game folder (it cd's there itself):
#
#   ./run_as2_linux.sh                 normal launch
#   ./run_as2_linux.sh --check         show what would run, do not launch
#   ./run_as2_linux.sh --debug         full Wine trace + mod logs in logs/linux-capture-<time>/
#   ./run_as2_linux.sh --vanilla       launch WITHOUT the mod (comparison run)
#   ./run_as2_linux.sh --no-wsock32    keep the mod but skip the wsock32 helper proxy
#
# Environment (all optional):
#   WINEPREFIX   prefix to use (Wine's default ~/.wine when unset)
#   AS2_WINE     runner command instead of "wine" (e.g. a Lutris/umu wrapper)
#   AS2_EXE      game executable name if auto-detection fails
#   WINEDLLOVERRIDES  your own overrides are kept; d3d9/wsock32 entries are replaced
#   AS2_WINEDEBUG     WINEDEBUG channels used by --debug
set -u

LAUNCHER_VERSION="2026-09-14"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 1
cd "$here" || exit 1

mode_debug=0
mode_check=0
mode_vanilla=0
use_wsock32=1
game_args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --debug)      mode_debug=1 ;;
        --check)      mode_check=1 ;;
        --vanilla)    mode_vanilla=1 ;;
        --no-wsock32) use_wsock32=0 ;;
        --help|-h)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        --) shift; game_args=("$@"); break ;;
        *)  game_args+=("$1") ;;
    esac
    shift
done

say()  { printf '[as2-launcher] %s\n' "$*"; }
warn() { printf '[as2-launcher] WARNING: %s\n' "$*" >&2; }
die()  { printf '[as2-launcher] ERROR: %s\n' "$*" >&2; exit 1; }

# --- game executable -------------------------------------------------------
exe="${AS2_EXE:-}"
if [ -z "$exe" ]; then
    for candidate in "as2.exe" "アリス戦記２.exe"; do
        if [ -f "$candidate" ]; then exe="$candidate"; break; fi
    done
fi
[ -n "$exe" ] && [ -f "$exe" ] || die "game executable not found in $here (set AS2_EXE=name.exe)"

# --- runner ---------------------------------------------------------------
runner="${AS2_WINE:-wine}"
runner_bin="${runner%% *}"
if ! command -v "$runner_bin" >/dev/null 2>&1; then
    die "'$runner_bin' is not installed or not on PATH. Install Wine (e.g. 'sudo apt install wine' / 'sudo pacman -S wine' / 'sudo dnf install wine') or set AS2_WINE to your launcher command."
fi
runner_version="$("$runner_bin" --version 2>/dev/null | head -n 1)"

# --- mod files -------------------------------------------------------------
missing=""
for f in d3d9.dll as2_rollback.dll SDL3.dll; do
    [ -f "$f" ] || missing="$missing $f"
done
if [ "$mode_vanilla" -eq 0 ] && [ -n "$missing" ]; then
    warn "mod files missing:$missing - the game will start without the mod"
fi
if [ -f d3d9.dll ] && ! grep -q "AS2Proxy_" d3d9.dll 2>/dev/null; then
    warn "d3d9.dll in this folder is not the ImprovementMod proxy (DXVK or another wrapper?)"
fi
if [ "$use_wsock32" -eq 1 ] && [ ! -f wsock32.dll ]; then
    use_wsock32=0
fi

# --- DLL overrides ---------------------------------------------------------
# Keep whatever the user already set, but our two entries must be ours.
merge_overrides() {
    local existing="${WINEDLLOVERRIDES:-}" out="" entry
    local IFS=';'
    for entry in $existing; do
        [ -z "$entry" ] && continue
        case "$entry" in
            d3d9=*|wsock32=*|*,d3d9=*|*,wsock32=*)
                warn "replacing your override '$entry' with the launcher's" ;;
            *) out="${out:+$out;}$entry" ;;
        esac
    done
    printf '%s' "$out"
}

kept="$(merge_overrides)"
if [ "$mode_vanilla" -eq 1 ]; then
    ours="d3d9=b;wsock32=b"                 # builtin only: both proxies bypassed
elif [ "$use_wsock32" -eq 1 ]; then
    ours="d3d9=n,b;wsock32=n,b"             # native proxy first, Wine builtin reachable
else
    ours="d3d9=n,b;wsock32=b"
fi
export WINEDLLOVERRIDES="${kept:+$kept;}$ours"
export AS2_LAUNCHER="run_as2_linux.sh/$LAUNCHER_VERSION mode=$( [ "$mode_vanilla" -eq 1 ] && printf vanilla || printf mod ) debug=$mode_debug"

prefix_shown="${WINEPREFIX:-$HOME/.wine (default)}"

# --- summary ---------------------------------------------------------------
say "game       : $here/$exe"
say "runner     : $runner ($runner_version)"
say "prefix     : $prefix_shown"
say "overrides  : $WINEDLLOVERRIDES"
say "session    : ${XDG_SESSION_TYPE:-unknown} display=${DISPLAY:-none} wayland=${WAYLAND_DISPLAY:-none}"
[ "$mode_vanilla" -eq 1 ] && say "mode       : VANILLA (mod bypassed for comparison)"
if [ "$mode_check" -eq 1 ]; then
    say "check only; not launching"
    exit 0
fi

mkdir -p logs 2>/dev/null
stamp="$(date +%Y%m%d-%H%M%S)"

if [ "$mode_debug" -eq 1 ]; then
    out="logs/linux-capture-$stamp"
    mkdir -p "$out" || die "cannot create $out"
    export WINEDEBUG="${AS2_WINEDEBUG:-+timestamp,+pid,+tid,+loaddll,+module,+seh,+debugstr}"
    {
        printf 'launcher=%s\nrunner=%s\nversion=%s\nprefix=%s\ncwd=%s\nexe=%s\n' \
            "$AS2_LAUNCHER" "$runner" "$runner_version" "$prefix_shown" "$PWD" "$exe"
        printf 'WINEDLLOVERRIDES=%s\nWINEDEBUG=%s\n' "$WINEDLLOVERRIDES" "$WINEDEBUG"
        printf 'session=%s display=%s wayland=%s\n' "${XDG_SESSION_TYPE:-unset}" "${DISPLAY:-unset}" "${WAYLAND_DISPLAY:-unset}"
        printf 'LANG=%s LC_ALL=%s\n' "${LANG:-unset}" "${LC_ALL:-unset}"
        command -v glxinfo >/dev/null 2>&1 && glxinfo -B 2>/dev/null | sed -n '1,25p'
        for f in "$exe" d3d9.dll wsock32.dll as2_rollback.dll SDL3.dll; do
            [ -f "$f" ] || continue
            command -v file >/dev/null 2>&1 && file -- "$f"
            sha256sum -- "$f"
        done
    } > "$out/environment.txt" 2>&1
    wine_log="$out/wine-unfiltered.log"
    say "debug capture -> $out"
else
    wine_log="logs/wine-$stamp.log"
fi

say "launching... (Wine output -> $wine_log)"
# shellcheck disable=SC2086
$runner "$exe" "${game_args[@]}" 2>&1 | tee "$wine_log"
status=${PIPESTATUS[0]}

latest_modlog="$(ls -1dt logs/*/ 2>/dev/null | grep -v linux-capture | head -n 1)"
if [ "$mode_debug" -eq 1 ]; then
    printf 'wine_exit=%s\n' "$status" >> "$out/environment.txt"
    [ -n "$latest_modlog" ] && cp -r -- "$latest_modlog" "$out/modlogs" 2>/dev/null
    [ -f crash_log.txt ] && cp -- crash_log.txt "$out/" 2>/dev/null
    say "capture complete: send the whole folder $out"
fi

say "game exited with status $status"
if [ -n "$latest_modlog" ]; then
    say "mod logs: $latest_modlog (startup_stage.txt shows the last stage reached)"
fi
exit "$status"
