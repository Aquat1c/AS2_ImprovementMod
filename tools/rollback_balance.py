#!/usr/bin/env python3
"""Measure rollback balance between two peers.

The question this answers: does one side spend the match near its prediction
ceiling while the other predicts almost nothing? Rollback COUNT being equal
does not settle it -- the counts can match while one peer does several times
the resim work, because what costs CPU is DEPTH.

The statistic that matters is the SPECULATION WINDOW: how far a peer runs its
local simulation past its confirmed frame (`rb_frame - conf`). PaceSlew acts on
exactly this quantity, comparing `peer_depth - local_depth` against a 2-frame
deadband, so a persistent gap means the controller is not converging.

Usage:
    python tools/rollback_balance.py <host_log_dir> <client_log_dir>
    python tools/rollback_balance.py <run_dir>        # expects host/ and client/
"""

import glob
import os
import re
import sys

PAT = re.compile(
    r"RollbackDebug\] rb=(\d+) game_abs=(\d+) origin=(\d+) conf=(\d+) "
    r"remote=(\d+) rb=(\d+) maxrb=(\d+) pred=(\d+) misp=(\d+) "
    r"delay=(\d+) budget=(\d+)")
SLEW = re.compile(r"slew_ppm=(-?\d+)")


def load(d):
    rows, slews = [], []
    for f in glob.glob(os.path.join(d, "*.log")):
        try:
            fh = open(f, encoding="utf-8", errors="replace")
        except OSError:
            continue
        with fh:
            for line in fh:
                m = PAT.search(line)
                if m:
                    g = [int(x) for x in m.groups()]
                    rows.append(dict(frame=g[0], conf=g[3], remote=g[4],
                                     count=g[5], maxrb=g[6], pred=g[7],
                                     misp=g[8], budget=g[10],
                                     spec=g[0] - g[3]))
                s = SLEW.search(line)
                if s:
                    slews.append(int(s.group(1)))
    rows.sort(key=lambda r: r["frame"])
    return rows, slews


def stat(v):
    if not v:
        return "n/a"
    v = sorted(v)
    return "avg=%.2f p50=%d p95=%d max=%d" % (
        sum(v) / len(v), v[len(v) // 2], v[int(len(v) * .95)], v[-1])


def main():
    a = sys.argv[1:]
    if len(a) == 1:
        hd, cd = os.path.join(a[0], "host"), os.path.join(a[0], "client")
    elif len(a) == 2:
        hd, cd = a
    else:
        print(__doc__)
        return 2

    h, hs = load(hd)
    c, cs = load(cd)
    if not h or not c:
        print("no RollbackDebug samples (host=%d client=%d)" % (len(h), len(c)))
        return 2

    print("samples: host=%d client=%d\n" % (len(h), len(c)))
    for name, rows, slews in (("HOST", h, hs), ("CLIENT", c, cs)):
        print("=== %s ===" % name)
        print("  rollbacks     %d   mispredictions %d"
              % (rows[-1]["count"], rows[-1]["misp"]))
        print("  spec window   %s" % stat([r["spec"] for r in rows]))
        print("  depth         %s" % stat([r["maxrb"] for r in rows]))
        print("  pred          %s" % stat([r["pred"] for r in rows]))
        eng = [s for s in slews if s != 0]
        print("  slew engaged  %d/%d samples (%.0f%%)  %s"
              % (len(eng), len(slews),
                 100.0 * len(eng) / max(1, len(slews)), stat(eng)))
        print()

    hspec = sum(r["spec"] for r in h) / len(h)
    cspec = sum(r["spec"] for r in c) / len(c)
    lo, hi = min(hspec, cspec), max(hspec, cspec)
    ratio = hi / lo if lo > 0.01 else float("inf")
    lead = "host" if hspec > cspec else "client"
    print("speculation ratio %.2fx (%s deeper: %.2f vs %.2f)"
          % (ratio, lead, hi, lo))
    # A converged controller keeps the two within roughly the deadband it acts
    # on; sustained separation beyond that means it is not doing its job.
    if ratio >= 2.0:
        print("VERDICT: IMBALANCED — %s carries the speculative work" % lead)
        return 1
    if ratio >= 1.4:
        print("VERDICT: mild imbalance")
        return 0
    print("VERDICT: balanced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
