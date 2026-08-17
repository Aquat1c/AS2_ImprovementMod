#!/usr/bin/env python3
"""analyze_stat.py — re0.7 M8 acceptance judge for the frozen STAT instrument.

Parses the per-second STAT lines that FrameScheduler emits into the netplay
log (as2_netplay_fullpath_<pid>.log) and outputs the plan §7.5 acceptance
verdicts. The STAT line format is FROZEN (plan §2.10, journal M0-6):

  [HH:MM:SS.mmm] [STAT    ] f<frame> sim_fps=<f> present_p50_us=<u>
  present_p99_us=<u> hold_pred=<u> hold_life=<u> hold_input=<u> hold_ext=<u>
  rollbacks=<u> rb_max=<u> slew_ppm=<d> debt=<d> silence_ms=<u>

Usage:
  python tools/analyze_stat.py --profile online-clean logs/as2_netplay_fullpath_1234.log [more logs...]
  python tools/analyze_stat.py --profile le1 --skip 10 host.log client.log
  python tools/analyze_stat.py --profile report host.log        # no gating

Profiles (thresholds from plan §7.3 / §7.5):
  offline       §7.5#1: offline pacing run. Zero holds/rollbacks, present p99
                <= 17.2 ms, sim rate at cadence +/-0.02, slew 0, debt 0.
  online-clean  §7.5#2: online, 0% loss, coverage >= required+1. ZERO holds of
                any cause, sim rate +/-0.02, present p99 <= 18.0 ms.
  le1           §7.3 LE-1 (155 ms / 0% loss / D0 R8 field replication): zero
                holds, CONTINUOUS rollbacks expected (depth ~5, rb_max <= 8),
                sim rate +/-0.02, present p99 <= 18.0 ms.
  lossy         Generic lossy cell (§7.3 3%/1%/5% rows): gameplay holds
                (pred+input+ext) < 2/min, present p99 <= 18.0 ms, no gating on
                rollbacks (they are the mechanism working).
  degraded      §7.3 10%+jitter / burst rows: report-only for holds, still
                gates on slew bounds and the incident scan (zero teardowns is
                judged by the incident section + your scenario expectation).
  report        No gating; prints everything.

Options:
  --skip N          drop the first N STAT seconds (warmup / menu navigation)
  --last N          keep only the last N STAT seconds after --skip
  --min-seconds N   fail (NO-DATA) when fewer than N STAT seconds remain
                    (default 30)
  --allow-lifecycle N   permit up to N hold_life counts in zero-hold profiles
                    (documented escape: user-paced winscreen waits; default 0
                    — the plan text is "zero holds of any cause")
  --cadence {auto,60,58.8}   expected sim rate (default auto: nearest match)
  --json            emit a machine-readable JSON report to stdout instead of
                    the human report
Exit code: 0 = all gated criteria PASS on all files; 1 = any FAIL; 2 = no data.
"""

import argparse
import json
import math
import re
import sys

STAT_RE = re.compile(
    r"\[(?P<ts>\d{2}:\d{2}:\d{2}\.\d{3})\]\s+\[STAT\s*\]\s+(?:f(?P<frame>-?\d+)\s+)?"
    r"sim_fps=(?P<sim_fps>[-\d.]+)\s+"
    r"present_p50_us=(?P<p50>\d+)\s+"
    r"present_p99_us=(?P<p99>\d+)\s+"
    r"hold_pred=(?P<hold_pred>\d+)\s+"
    r"hold_life=(?P<hold_life>\d+)\s+"
    r"hold_input=(?P<hold_input>\d+)\s+"
    r"hold_ext=(?P<hold_ext>\d+)\s+"
    r"rollbacks=(?P<rollbacks>\d+)\s+"
    r"rb_max=(?P<rb_max>\d+)\s+"
    r"slew_ppm=(?P<slew>-?\d+)\s+"
    r"debt=(?P<debt>-?\d+)\s+"
    r"silence_ms=(?P<silence>\d+)"
)

# Incident scan: whole-log substring counters (case-sensitive, cheap).
# Each entry: (label, substring, meaning shown in the report).
INCIDENT_PATTERNS = [
    ("f7_terminal",     "F-7 TERMINAL",                    "post-match intent contradiction terminal (must be 0)"),
    ("resync_request",  "Sent ResyncRequest",              "frontend starvation interrogation (must be 0 on plain loss, M5 note)"),
    ("inv8_begin",      "BeginInputPhase while phase",     "INV-8 deferred/duplicate frontend Begin fire (must be 0)"),
    ("confirmed_desync","ConfirmedDesync",                 "engine sync-hash desync terminal"),
    ("baseline_mm",     "BaselineMismatch",                "baseline digest terminal (after retry)"),
    ("protocol_viol",   "ProtocolViolation",               "protocol-violation terminal"),
    ("progress_dead",   "ProgressDeadline",                "wedged-peer progress teardown"),
    ("pacing_dead",     "PacingClockDead",                 "dead-QPC pacing terminal"),
    ("replay_desync",   "REPLAY DESYNC",                   "replay playback hash divergence (M7 gate: must be 0)"),
    ("blackout",        "Blackout ENTER",                  "egress fault-injection blackout windows (context)"),
    ("inject_active",   "FAULT INJECTION",                 "fault injection armed (context)"),
    ("soak_summary",    "SUMMARY",                         "soak summary lines (verdict echoed below)"),
]

SOAK_VERDICT_RE = re.compile(r"verdict=(SOAK-(?:PASS|FAIL))")

CADENCES = {"60": 60.0, "58.8": 1000.0 / 17.0}  # 58.82...


def parse_ts_ms(ts):
    h, m, rest = ts.split(":")
    s, ms = rest.split(".")
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)


def percentile(sorted_vals, q):
    if not sorted_vals:
        return 0
    idx = min(len(sorted_vals) - 1, int(math.ceil(q * len(sorted_vals))) - 1)
    return sorted_vals[max(0, idx)]


def analyze_file(path, args):
    stats = []
    incidents = {k: 0 for k, _, _ in INCIDENT_PATTERNS}
    soak_verdicts = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = STAT_RE.search(line)
            if m:
                stats.append({
                    "ts_ms": parse_ts_ms(m.group("ts")),
                    "sim_fps": float(m.group("sim_fps")),
                    "p50": int(m.group("p50")),
                    "p99": int(m.group("p99")),
                    "hold_pred": int(m.group("hold_pred")),
                    "hold_life": int(m.group("hold_life")),
                    "hold_input": int(m.group("hold_input")),
                    "hold_ext": int(m.group("hold_ext")),
                    "rollbacks": int(m.group("rollbacks")),
                    "rb_max": int(m.group("rb_max")),
                    "slew": int(m.group("slew")),
                    "debt": int(m.group("debt")),
                    "silence": int(m.group("silence")),
                })
                continue
            for key, needle, _ in INCIDENT_PATTERNS:
                if needle in line:
                    incidents[key] += 1
                    if key == "soak_summary":
                        sm = SOAK_VERDICT_RE.search(line)
                        if sm:
                            soak_verdicts.append(sm.group(1))

    total_lines = len(stats)
    if args.skip > 0:
        stats = stats[args.skip:]
    if args.last is not None:
        stats = stats[-args.last:]

    result = {
        "file": path,
        "stat_seconds_total": total_lines,
        "stat_seconds_analyzed": len(stats),
        "incidents": incidents,
        "soak_verdicts": soak_verdicts,
        "criteria": [],   # (name, gated, passed, detail)
        "verdict": "NO-DATA",
    }
    if len(stats) < args.min_seconds:
        return result

    n = len(stats)
    dur_min = n / 60.0

    sim = [s["sim_fps"] for s in stats]
    p50s = sorted(s["p50"] for s in stats)
    p99s = [s["p99"] for s in stats]
    holds_pred = sum(s["hold_pred"] for s in stats)
    holds_life = sum(s["hold_life"] for s in stats)
    holds_input = sum(s["hold_input"] for s in stats)
    holds_ext = sum(s["hold_ext"] for s in stats)
    holds_gameplay = holds_pred + holds_input + holds_ext
    holds_all = holds_gameplay + holds_life
    rollbacks = sum(s["rollbacks"] for s in stats)
    rb_max = max(s["rb_max"] for s in stats)
    slew_max = max(abs(s["slew"]) for s in stats)
    slew_nonzero = sum(1 for s in stats if s["slew"] != 0)
    debt_max = max(s["debt"] for s in stats)
    silence_max = max(s["silence"] for s in stats)

    sim_mean = sum(sim) / n
    p99_max = max(p99s)
    p99_over = None  # filled after threshold known
    p50_median = percentile(p50s, 0.50)

    # STAT continuity: gaps > 2.5 s between consecutive lines (log-flush loss
    # or a wedged process; the emitter is strictly per-second).
    gaps = 0
    for a, b in zip(stats, stats[1:]):
        d = b["ts_ms"] - a["ts_ms"]
        if d < 0:
            d += 24 * 3600 * 1000  # midnight wrap
        if d > 2500:
            gaps += 1

    # Cadence target
    if args.cadence == "auto":
        target = min(CADENCES.values(), key=lambda c: abs(sim_mean - c))
    else:
        target = CADENCES[args.cadence]

    prof = args.profile
    p99_limit = 17200 if prof == "offline" else 18000
    p99_over = sum(1 for v in p99s if v > p99_limit)

    def crit(name, gated, passed, detail):
        result["criteria"].append(
            {"name": name, "gated": gated, "passed": bool(passed), "detail": detail})

    zero_hold_profile = prof in ("offline", "online-clean", "le1")
    life_budget = args.allow_lifecycle

    # -- Hold-cause breakdown (always reported) ------------------------------
    crit("hold_breakdown", False, True,
         "pred=%d life=%d input=%d ext=%d (total=%d over %.1f min, %.2f holds/min)"
         % (holds_pred, holds_life, holds_input, holds_ext, holds_all,
            dur_min, holds_all / dur_min if dur_min else 0.0))

    # -- Zero-holds criterion (§7.5#2 / LE-1 / offline) ----------------------
    if zero_hold_profile:
        gp_ok = holds_gameplay == 0
        life_ok = holds_life <= life_budget
        crit("zero_gameplay_holds", True, gp_ok,
             "pred+input+ext = %d (required 0)" % holds_gameplay)
        crit("zero_lifecycle_holds", True, life_ok,
             "life = %d (allowed %d%s)" % (holds_life, life_budget,
              "; plan says zero of ANY cause — use --allow-lifecycle only with a documented reason"
              if holds_life and life_ok else ""))
    elif prof == "lossy":
        rate = holds_gameplay / dur_min if dur_min else 0.0
        crit("gameplay_holds_per_min", True, rate < 2.0,
             "%.2f/min (required < 2/min, §7.3)" % rate)
    else:
        crit("gameplay_holds_per_min", False, True,
             "%.2f/min (report only)" % (holds_gameplay / dur_min if dur_min else 0.0))

    # -- Present p99 ---------------------------------------------------------
    crit("present_p99", prof != "report", p99_over == 0,
         "max per-second p99 = %d us, %d/%d seconds over %d us limit"
         % (p99_max, p99_over, n, p99_limit))
    crit("present_p50_median", False, True, "%d us (target ~%d us)"
         % (p50_median, int(round(1e6 / target))))

    # -- Sim rate ------------------------------------------------------------
    sim_ok = abs(sim_mean - target) <= 0.02
    crit("sim_rate", prof in ("offline", "online-clean", "le1"), sim_ok,
         "mean %.4f fps vs target %.4f (tolerance 0.02); min second %.2f"
         % (sim_mean, target, min(sim)))

    # -- Rollbacks -----------------------------------------------------------
    if prof == "offline":
        crit("rollbacks", True, rollbacks == 0, "%d (offline requires 0)" % rollbacks)
    elif prof == "le1":
        crit("rollback_depth", True, rb_max <= 8,
             "rb_max=%d (LE-1 cell: max <= 8); total rollbacks=%d (continuous expected)"
             % (rb_max, rollbacks))
    else:
        crit("rollbacks", False, True, "total=%d rb_max=%d" % (rollbacks, rb_max))

    # -- Slew bounds (§2.8.4: absolute cap 25000 ppm) ------------------------
    crit("slew_bounds", prof != "report", slew_max <= 25000,
         "max |slew_ppm|=%d (cap 25000); nonzero on %d/%d seconds"
         % (slew_max, slew_nonzero, n))
    if prof == "offline":
        crit("slew_zero_offline", True, slew_max == 0,
             "max |slew_ppm|=%d (offline requires 0)" % slew_max)

    # -- Debt / silence / continuity (report) --------------------------------
    crit("debt_max", False, True, "%d" % debt_max)
    crit("silence_max_ms", False, True, "%d" % silence_max)
    crit("stat_gaps", prof != "report", gaps == 0,
         "%d gaps > 2.5 s between STAT seconds (flush loss or stall)" % gaps)

    # -- Incident gates ------------------------------------------------------
    hard_zero = ["f7_terminal", "inv8_begin", "replay_desync"]
    for key in hard_zero:
        crit("incident_" + key, prof != "report", incidents[key] == 0,
             "%d occurrences (must be 0)" % incidents[key])
    # ResyncRequest: hard-zero only in loss-free profiles; reported otherwise
    # (M5 note: must not fire on plain packet loss either — investigate any).
    crit("incident_resync_request", zero_hold_profile,
         incidents["resync_request"] == 0,
         "%d occurrences" % incidents["resync_request"])

    gated = [c for c in result["criteria"] if c["gated"]]
    result["verdict"] = "PASS" if all(c["passed"] for c in gated) else "FAIL"
    result["summary"] = {
        "sim_mean": round(sim_mean, 4), "target_fps": round(target, 4),
        "p99_max_us": p99_max, "p50_median_us": p50_median,
        "holds": {"pred": holds_pred, "life": holds_life,
                  "input": holds_input, "ext": holds_ext},
        "rollbacks": rollbacks, "rb_max": rb_max,
        "slew_max_ppm": slew_max, "debt_max": debt_max,
        "silence_max_ms": silence_max, "stat_gaps": gaps,
        "minutes": round(dur_min, 2),
    }
    return result


def print_human(result):
    print("=" * 72)
    print("FILE: %s" % result["file"])
    print("STAT seconds: %d total, %d analyzed" %
          (result["stat_seconds_total"], result["stat_seconds_analyzed"]))
    if result["verdict"] == "NO-DATA":
        print("VERDICT: NO-DATA (below --min-seconds; is this the right log / did the scheduler install?)")
        return
    for c in result["criteria"]:
        flag = ("PASS" if c["passed"] else "FAIL") if c["gated"] else "info"
        print("  [%-4s] %-26s %s" % (flag, c["name"], c["detail"]))
    inc = result["incidents"]
    shown = {k: v for k, v in inc.items() if v}
    if shown:
        print("  incidents: " + ", ".join("%s=%d" % kv for kv in sorted(shown.items())))
    for v in result["soak_verdicts"]:
        print("  soak: %s" % v)
    print("VERDICT: %s" % result["verdict"])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="netplay log file(s) to analyze")
    ap.add_argument("--profile", default="report",
                    choices=["offline", "online-clean", "le1", "lossy",
                             "degraded", "report"])
    ap.add_argument("--skip", type=int, default=0)
    ap.add_argument("--last", type=int, default=None)
    ap.add_argument("--min-seconds", type=int, default=30)
    ap.add_argument("--allow-lifecycle", type=int, default=0)
    ap.add_argument("--cadence", default="auto", choices=["auto", "60", "58.8"])
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    results = [analyze_file(p, args) for p in args.logs]
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            print_human(r)
        verdicts = [r["verdict"] for r in results]
        print("=" * 72)
        print("OVERALL: %s" % ("PASS" if all(v == "PASS" for v in verdicts)
                               else ("NO-DATA" if all(v == "NO-DATA" for v in verdicts)
                                     else "FAIL")))
    if any(r["verdict"] == "FAIL" for r in results):
        sys.exit(1)
    if all(r["verdict"] == "NO-DATA" for r in results):
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
