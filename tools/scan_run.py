#!/usr/bin/env python3
"""Acceptance-run log auditor.

Scans every instance's log for a run and reports:
  1. per-game GAMEREPORT lines (the harness's own end-of-match summary)
  2. KNOWN failure signatures (desync, stall, terminal, route wedge, ...)
  3. ANYTHING ELSE that looks wrong -- every WRN/ERR line and every line whose
     wording suggests trouble -- grouped and counted.

(3) is the point. A fixed allow-list only ever catches failures we already
know about, and an unattended run is exactly where a NOVEL failure hides. So
the default posture is: anything not explicitly recognised as benign gets
surfaced.

Usage:
    python tools/scan_run.py [--since HH:MM:SS] [game-dir ...]
Exit code 0 = clean, 1 = something needs a human (or another fix).
"""

import argparse
import glob
import os
import re
import sys
from collections import Counter, defaultdict

DEFAULT_DIRS = [
    r"D:/dev/alice_senki/game/Alice in wonderland 2",
    r"D:/dev/alice_senki/game/Alice in wonderland 2 - instB",
    r"D:/dev/alice_senki/game/Alice in wonderland 2 - instC",
]

# Hard failures: any occurrence fails the run.
FATAL = [
    (r"CONFIRMED DESYNC", "desync"),
    (r"sync hash mismatch", "desync"),
    (r"REPLAY MISMATCH", "local nondeterminism"),
    (r"replay_bad=[1-9]", "replay verification failed"),
    (r"F-7 TERMINAL", "post-match intent terminal"),
    (r"stopped responding", "progress-deadline kill"),
    (r"Config rejected", "config handshake rejected"),
    (r"ProtocolViolation", "protocol violation terminal"),
    (r"restore FAILED", "savestate restore failed"),
    (r"ENGINE2 restore FAILED", "savestate restore failed"),
    (r"engine terminal", "engine terminal"),
    (r"InternalInvariant", "engine invariant"),
    (r"NO-OP: LIVE STATE UNCHANGED", "restore did nothing"),
]

# Degraded: recovery paths that WORKED but should not have been needed.
# These are what "nothing weird in the logs" is really about.
DEGRADED = [
    (r"ROUTE RELEASED", "win-screen route needed recovery"),
    (r"deferral exhausted", "cross-phase deferral gave up"),
    (r"handoff timeout", "win-screen handoff timed out"),
    (r"fail-open watchdog", "ladder wedged, watchdog fired"),
    (r"ANOMALY", "explicit anomaly marker"),
    (r"FAIL-CLOSED", "ownerless input route"),
    (r"Stale .* proposal ignored", "stale barrier proposal"),
    (r"overflow=[1-9]|overflow_forced=[1-9]", "queue overflow"),
    (r"truncated=[1-9]", "rollback truncated at boundary"),
    (r"Substate escaped", "non-deterministic prompt entry"),
    (r"consume failed despite ready frame", "frontend consume failure"),
    (r"Timed out waiting", "harness timeout"),
    (r"Session failed", "session failed"),
    (r"connect retry", "connect retry"),
]

# Benign noise that would otherwise swamp the "anything else" bucket.
BENIGN = [
    r"Runtime freeze ENABLED",          # normal startup/boundary hold
    r"Runtime freeze DISABLED",
    r"Begin deferred: no active frontend epoch",  # transient, retried
    r"Autopunch",
    r"UPnP|PCP|STUN|NAT",
    r"Unmatched Asset_LoadAllFromArchive",  # vanilla palette path
    r"pump stall",                      # menu-thread timing note
    r"Deadline rebase",                 # scheduler boundary rebase
    r"Inbound silence while outbound active",
    r"consume lag",
    r"Game-thread Session_Update gap",
    r"waiting for remote frame",        # normal lockstep wait
    r"Resending SyncAnnounce",
    r"no remote announce yet",
    r"skipped vanilla renderer",        # render guard, non-fatal by design
    r"AI-learning guard",
    r"SPECTATE_HOST",
]

LEVEL_RE = re.compile(r"\[(WRN|ERR|WARNING|ERROR)\s*\]|\[(WRN|ERR)\]")
TIME_RE = re.compile(r"^\[(\d{2}:\d{2}:\d{2})")


def newest_run(gamedir):
    logs = os.path.join(gamedir, "logs")
    if not os.path.isdir(logs):
        return None
    runs = [d for d in glob.glob(os.path.join(logs, "20*")) if os.path.isdir(d)]
    return max(runs, key=os.path.getmtime) if runs else None


# Shapes that are the harness's OWN shutdown after the ladder finishes. They are
# only benign AFTER the final "ladder complete" -- the same words mid-run mean a
# real mid-match loss, so they are suppressed by position, never by wording.
TEARDOWN = re.compile(
    r"Remote canceled the session|Hidden netplay flow lost session|"
    r"OpenDisconnectError|Disconnect during PlayableGameplay|"
    r"MatchSetup\] Abort:|Forcing return to menu")


def scan_file(path, since, fatal, degraded, other, reports, ladder_done_at=None):
    benign = re.compile("|".join(BENIGN))
    try:
        fh = open(path, encoding="utf-8", errors="replace")
    except OSError:
        return
    with fh:
        for line in fh:
            if ladder_done_at:
                m = TIME_RE.match(line)
                if m and m.group(1) >= ladder_done_at and TEARDOWN.search(line):
                    continue
            if since:
                m = TIME_RE.match(line)
                if m and m.group(1) < since:
                    continue
            if "GAMEREPORT" in line:
                reports.append(line.rstrip())
                continue
            for pat, label in FATAL:
                if re.search(pat, line):
                    fatal[label].append(line.rstrip())
                    break
            else:
                for pat, label in DEGRADED:
                    if re.search(pat, line):
                        degraded[label].append(line.rstrip())
                        break
                else:
                    if LEVEL_RE.search(line) and not benign.search(line):
                        # Collapse to a shape so repeats group together.
                        shape = re.sub(r"\d+", "N", line.rstrip())[:150]
                        other[shape] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="*", default=None)
    ap.add_argument("--since", default=None, help="HH:MM:SS lower bound")
    args = ap.parse_args()
    dirs = args.dirs or DEFAULT_DIRS

    fatal = defaultdict(list)
    degraded = defaultdict(list)
    other = Counter()
    reports = []

    scanned = []
    for d in dirs:
        run = newest_run(d)
        if not run:
            continue
        scanned.append(run)
        run_files = glob.glob(os.path.join(run, "*.log"))
        # The ladder-completion time is run-wide: the marker and the teardown
        # warnings land in DIFFERENT log files, so a per-file search finds
        # nothing and suppresses nothing.
        ladder_done_at = None
        for f in run_files:
            try:
                with open(f, encoding="utf-8", errors="replace") as fh:
                    for l in fh:
                        if "ladder complete" in l:
                            m = TIME_RE.match(l)
                            if m and (ladder_done_at is None or m.group(1) > ladder_done_at):
                                ladder_done_at = m.group(1)
            except OSError:
                pass
        for f in run_files:
            scan_file(f, args.since, fatal, degraded, other, reports, ladder_done_at)

    print("=== scanned runs ===")
    for r in scanned:
        print("  " + r)

    print("\n=== per-game reports (%d) ===" % len(reports))
    for r in reports:
        print("  " + r.split("] ", 2)[-1].strip())

    print("\n=== FATAL (%d kinds) ===" % len(fatal))
    for label, lines in sorted(fatal.items()):
        print("  %-34s x%d" % (label, len(lines)))
        for l in lines[:3]:
            print("      " + l.strip()[:190])

    print("\n=== DEGRADED / recovery used (%d kinds) ===" % len(degraded))
    for label, lines in sorted(degraded.items()):
        print("  %-34s x%d" % (label, len(lines)))
        for l in lines[:2]:
            print("      " + l.strip()[:190])

    print("\n=== other warnings/errors, grouped (%d shapes) ===" % len(other))
    for shape, n in other.most_common(25):
        print("  x%-5d %s" % (n, shape.strip()[:170]))

    bad = len(fatal) > 0 or len(degraded) > 0 or len(other) > 0
    print("\nVERDICT: " + ("NEEDS ATTENTION" if bad else "CLEAN"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
