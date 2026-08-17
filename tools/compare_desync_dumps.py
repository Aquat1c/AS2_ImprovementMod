#!/usr/bin/env python3
"""Compare the two sides' ConfirmedDesync evidence dumps (re0.7 post-M8).

Both peers write a desync_dump_<pid>_f<frame>.txt on a ConfirmedDesync:
the detecting side from ReportEngineTerminal, the surviving side from the
Disconnect-receive hook (goodbye reason ConfirmedDesync). Each dump opens
with a machine-readable section:

    DIAG source=... rb_frame=... local_crc=... remote_crc=...
    EVIDENCE frame=... local_hash=... peer_hash=... first_divergent=...
    FIRSTDIVERGENT field=... frame=...          (detecting side only)
    RINGCOUNT n=...
    RING frame=<canonical> epoch=.. hash=.. rng=.. hp0=.. hp1=.. p1=.. p2=..
    REGION name=<token> addr=0x... size=... crc=0x...

This tool prints:
  1. the first divergent CONFIRMED frame across the two rings (hash chain),
  2. which fields diverged there (hash / rng / hp0 / hp1 / inputs),
  3. a region-CRC diff table,
  4. the input context around the divergence.

Usage:  python compare_desync_dumps.py <side_a_dump.txt> <side_b_dump.txt>
Exit codes: 0 = compared cleanly, 1 = usage/parse error.
"""

import re
import sys

RING_RE = re.compile(
    r"^RING frame=(\d+) epoch=(\d+) hash=([0-9a-fA-F]+) rng=([0-9a-fA-F]+) "
    r"hp0=(\d+) hp1=(\d+) p1=0x([0-9a-fA-F]+) p2=0x([0-9a-fA-F]+)\s*$")
EVIDENCE_RE = re.compile(
    r"^EVIDENCE frame=(\d+) epoch=(\d+) local_hash=([0-9a-fA-F]+) "
    r"peer_hash=([0-9a-fA-F]+) local_rng=([0-9a-fA-F]+) peer_rng=([0-9a-fA-F]+) "
    r"local_hp0=(\d+) local_hp1=(\d+) peer_hp0=(\d+) peer_hp1=(\d+) "
    r"first_divergent=(\w+)\s*$")
REGION_RE = re.compile(
    r"^REGION name=(\S+) addr=0x([0-9a-fA-F]+) size=(\d+) crc=0x([0-9a-fA-F]+)\s*$")
DIAG_RE = re.compile(r"^DIAG source=(\S+) rb_frame=(-?\d+)")


def parse_dump(path):
    dump = {
        "path": path,
        "source": None,
        "rb_frame": None,
        "evidence": None,
        "ring": {},        # canonical frame -> entry dict
        "regions": {},     # name -> (addr, size, crc)
    }
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = DIAG_RE.match(line)
            if m:
                dump["source"] = m.group(1)
                dump["rb_frame"] = int(m.group(2))
                continue
            m = RING_RE.match(line)
            if m:
                frame = int(m.group(1))
                dump["ring"][frame] = {
                    "frame": frame,
                    "epoch": int(m.group(2)),
                    "hash": int(m.group(3), 16),
                    "rng": int(m.group(4), 16),
                    "hp0": int(m.group(5)),
                    "hp1": int(m.group(6)),
                    "p1": int(m.group(7), 16),
                    "p2": int(m.group(8), 16),
                }
                continue
            m = EVIDENCE_RE.match(line)
            if m:
                dump["evidence"] = {
                    "frame": int(m.group(1)),
                    "epoch": int(m.group(2)),
                    "local_hash": int(m.group(3), 16),
                    "peer_hash": int(m.group(4), 16),
                    "local_rng": int(m.group(5), 16),
                    "peer_rng": int(m.group(6), 16),
                    "local_hp0": int(m.group(7)),
                    "local_hp1": int(m.group(8)),
                    "peer_hp0": int(m.group(9)),
                    "peer_hp1": int(m.group(10)),
                    "first_divergent": m.group(11),
                }
                continue
            m = REGION_RE.match(line)
            if m:
                dump["regions"][m.group(1)] = (
                    int(m.group(2), 16), int(m.group(3)), int(m.group(4), 16))
    return dump


FIELDS = ("hash", "rng", "hp0", "hp1", "p1", "p2")


def divergent_fields(ea, eb):
    return [f for f in FIELDS if ea[f] != eb[f]]


def fmt_entry(e):
    return ("hash=%016x rng=%08x hp0=%u hp1=%u p1=0x%04x p2=0x%04x"
            % (e["hash"], e["rng"], e["hp0"], e["hp1"], e["p1"], e["p2"]))


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1

    a = parse_dump(argv[1])
    b = parse_dump(argv[2])
    for d in (a, b):
        print("side: %-24s source=%-22s rb_frame=%s ring_frames=%d regions=%d"
              % (d["path"], d["source"], d["rb_frame"],
                 len(d["ring"]), len(d["regions"])))
        if not d["ring"]:
            print("  WARNING: no RING lines parsed -- old-format dump?")
    print()

    # ── 1+2: first divergent confirmed frame across the two rings ──────────
    common = sorted(set(a["ring"]) & set(b["ring"]))
    if not common:
        print("no overlapping confirmed frames between the two rings "
              "(windows did not intersect)")
    else:
        print("overlapping confirmed frames: %d..%d (%d frames)"
              % (common[0], common[-1], len(common)))
        first_div = None
        for frame in common:
            fields = divergent_fields(a["ring"][frame], b["ring"][frame])
            if fields:
                first_div = (frame, fields)
                break
        if first_div is None:
            print("hash chains AGREE over the whole overlap -- the divergence "
                  "is newer than both rings' windows (or in the un-confirmed "
                  "suffix); check the EVIDENCE pair below")
        else:
            frame, fields = first_div
            print()
            print("FIRST DIVERGENT CONFIRMED FRAME: %d" % frame)
            print("  divergent fields: %s" % ", ".join(fields))
            print("  side A: %s" % fmt_entry(a["ring"][frame]))
            print("  side B: %s" % fmt_entry(b["ring"][frame]))
            # ── 4: input context around the divergence ──────────────────────
            print()
            print("input context (A|B, * marks the divergent frame, "
                  "! marks input disagreement):")
            for f in common:
                if f < frame - 8 or f > frame + 8:
                    continue
                ea, eb = a["ring"][f], b["ring"][f]
                in_mismatch = (ea["p1"] != eb["p1"] or ea["p2"] != eb["p2"])
                hash_mismatch = ea["hash"] != eb["hash"]
                mark = "*" if f == frame else (" " if not hash_mismatch else ">")
                print("  %s f%-8d A: p1=0x%04x p2=0x%04x | B: p1=0x%04x p2=0x%04x%s"
                      % (mark, f, ea["p1"], ea["p2"], eb["p1"], eb["p2"],
                         "   !INPUTS DIFFER (canonical stream broke!)"
                         if in_mismatch else ""))

    # ── evidence lines (the failing SyncHash pair as each side saw it) ──────
    print()
    for label, d in (("A", a), ("B", b)):
        ev = d["evidence"]
        if ev is None:
            print("side %s: no EVIDENCE (surviving side that never saw the "
                  "mismatch locally)" % label)
        else:
            print("side %s EVIDENCE: frame=%d first_divergent=%s" %
                  (label, ev["frame"], ev["first_divergent"]))
            print("   local:  hash=%016x rng=%08x hp=%d,%d" %
                  (ev["local_hash"], ev["local_rng"],
                   ev["local_hp0"], ev["local_hp1"]))
            print("   peer:   hash=%016x rng=%08x hp=%d,%d" %
                  (ev["peer_hash"], ev["peer_rng"],
                   ev["peer_hp0"], ev["peer_hp1"]))

    # ── 3: region-CRC diff table ────────────────────────────────────────────
    print()
    names = [n for n in a["regions"] if n in b["regions"]]
    if not names:
        print("no overlapping REGION lines")
    else:
        print("region CRC diff (dumped at each side's detection instant -- "
              "post-divergence state, drift markers not proof):")
        print("  %-16s %-10s %-10s  %s" % ("region", "side A", "side B", ""))
        for n in names:
            _, _, ca = a["regions"][n]
            _, _, cb = b["regions"][n]
            print("  %-16s 0x%08x 0x%08x  %s"
                  % (n, ca, cb, "" if ca == cb else "DIFFERS"))
        only_a = set(a["regions"]) - set(b["regions"])
        only_b = set(b["regions"]) - set(a["regions"])
        if only_a:
            print("  (only in A: %s)" % ", ".join(sorted(only_a)))
        if only_b:
            print("  (only in B: %s)" % ", ".join(sorted(only_b)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
