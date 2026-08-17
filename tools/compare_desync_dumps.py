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

# ── F7d fine-grained localization (FINE* lines, desync_fine_diag.cpp) ───────
MAIN_BASE = 0x76C5F8
FINE_REGIONS = [
    ("match_header",  0x76C5F8, 0x76C608),
    ("match_context", 0x76C608, 0x76E328),
    ("effect_array",  0x76E328, 0x76FC28),
    ("summon_array",  0x76FC28, 0x776668),
    ("p1_entity",     0x776668, 0x790F74),
    ("p2_entity",     0x790F74, 0x7AB880),
]
# main_state digest-mask table (kMainDigestMasks) -> fold segments. Each
# segment's Block64 starts fresh block alignment at the segment start, so
# "low/high half" of an 8-byte block is relative to the segment.
P1_OFF = 0x776668 - MAIN_BASE
P2_OFF = 0x790F74 - MAIN_BASE
_MASKS = [
    (0x700, 0x44),                    # F7c per_frame_temp
    (P1_OFF + 0x01B8, 8),             # F5
    (P1_OFF + 0x04DC, 0x260),         # F2
    (P1_OFF + 0x1A24C, 12),           # F4
    (P2_OFF + 0x01B8, 8),             # F5
    (P2_OFF + 0x04DC, 0x260),         # F2
    (P2_OFF + 0x1A24C, 12),           # F4
]
MAIN_SIZE = 0x7AB880 - MAIN_BASE


def fold_segments():
    segs = []
    pos = 0
    for off, size in _MASKS:
        segs.append((pos, off))
        pos = off + size
    segs.append((pos, MAIN_SIZE))
    return segs


SEGS = fold_segments()


def annotate_main_offset(off):
    """Region+offset name, digest membership, and block-half for a
    main_state offset."""
    addr = MAIN_BASE + off
    name = "?"
    for n, lo, hi in FINE_REGIONS:
        if lo <= addr < hi:
            name = "%s+0x%x" % (n, addr - lo)
            break
    for lo, hi in SEGS:
        if lo <= off < hi:
            half = "low" if ((off - lo) % 8) < 4 else "HIGH"
            return name, "hashed", half
    return name, "MASKED", "-"


FINEHDR_RE = re.compile(
    r"^FINEHDR frame=(\d+) (.*?)\s*$")
FINEWIN_RE = re.compile(
    r"^FINEWIN frame=(\d+) n=(\d+) crcs=([0-9a-f,]+)\s*$")
FINERAW_RE = re.compile(
    r"^FINERAW frame=(\d+) n=(\d+) hex=([0-9a-f]+)\s*$")
FINEENT_RE = re.compile(
    r"^FINEENT frame=(\d+) spans=(\S+) p1=([0-9a-f]+) p2=([0-9a-f]+)\s*$")

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
        "finehdr": {},     # frame -> {field: value-string}
        "finewin": {},     # frame -> [crc, ...]
        "fineraw": {},     # frame -> bytes
        "fineent": {},     # frame -> (spans, p1 bytes, p2 bytes)
    }
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = FINEHDR_RE.match(line)
            if m:
                frame = int(m.group(1))
                dump["finehdr"][frame] = dict(
                    kv.split("=", 1) for kv in m.group(2).split())
                continue
            m = FINEWIN_RE.match(line)
            if m:
                dump["finewin"][int(m.group(1))] = m.group(3).split(",")
                continue
            m = FINERAW_RE.match(line)
            if m:
                dump["fineraw"][int(m.group(1))] = bytes.fromhex(m.group(3))
                continue
            m = FINEENT_RE.match(line)
            if m:
                dump["fineent"][int(m.group(1))] = (
                    m.group(2), bytes.fromhex(m.group(3)), bytes.fromhex(m.group(4)))
                continue
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


def runs(indices):
    """Group a sorted int list into (start, end_inclusive) runs."""
    out = []
    for i in indices:
        if out and i == out[-1][1] + 1:
            out[-1][1] = i
        else:
            out.append([i, i])
    return out


def fine_diff(a, b, frame, raw_context=2):
    """Byte/window-level localization at `frame` from the FINE* records."""
    ha, hb = a["finehdr"].get(frame), b["finehdr"].get(frame)
    if ha and hb:
        div = [k for k in ha if k in hb and ha[k] != hb[k]]
        if div:
            print("  FINEHDR divergent fields at f%d:" % frame)
            for k in div:
                print("    %-8s A=%s B=%s" % (k, ha[k], hb[k]))
        else:
            print("  FINEHDR: all header scalars EQUAL at f%d "
                  "(rng/sim/mode/sub/subt/gtype/mpt/ridx/widx/effidx/fdisp"
                  "/ai/p1t/p2t)" % frame)
    else:
        print("  no FINEHDR pair for f%d" % frame)

    wa, wb = a["finewin"].get(frame), b["finewin"].get(frame)
    if wa and wb and len(wa) == len(wb):
        bad = [i for i in range(len(wa)) if wa[i] != wb[i]]
        print("  FINEWIN divergent 64B windows at f%d: %d of %d"
              % (frame, len(bad), len(wa)))
        for i in bad[:40]:
            off = i * 64
            name, memb, _ = annotate_main_offset(off)
            print("    win %4d main+0x%05x addr=0x%08x [%s] (%s) A=%s B=%s"
                  % (i, off, MAIN_BASE + off, name, memb, wa[i], wb[i]))
        if len(bad) > 40:
            print("    ... %d more" % (len(bad) - 40))
    else:
        print("  no FINEWIN pair for f%d" % frame)

    ea_, eb_ = a["fineent"].get(frame), b["fineent"].get(frame)
    if ea_ and eb_:
        spans = []
        for part in ea_[0].split(","):
            off, size = part.split(":")
            spans.append((int(off, 16), int(size, 16)))
        def ent_off(concat):
            pos = 0
            for off, size in spans:
                if concat < pos + size:
                    return off + (concat - pos)
                pos += size
            return -1
        for pi, plabel in ((1, "p1"), (2, "p2")):
            da, db = ea_[pi], eb_[pi]
            bad = [i for i in range(min(len(da), len(db))) if da[i] != db[i]]
            if bad:
                print("  FINEENT %s divergent bytes at f%d:" % (plabel, frame))
                for lo, hi in runs(bad)[:10]:
                    print("    %s_entity+0x%x..0x%x A=%s B=%s"
                          % (plabel, ent_off(lo), ent_off(hi),
                             da[lo:hi + 1].hex(), db[lo:hi + 1].hex()))

    ra, rb = a["fineraw"].get(frame), b["fineraw"].get(frame)
    if ra and rb and len(ra) == len(rb):
        bad = [i for i in range(len(ra)) if ra[i] != rb[i]]
        print("  FINERAW divergent bytes at f%d (raw image = "
              "header+context+effect_array): %d bytes" % (frame, len(bad)))
        for lo, hi in runs(bad)[:32]:
            name, memb, half = annotate_main_offset(lo)
            av = ra[lo:hi + 1].hex()
            bv = rb[lo:hi + 1].hex()
            print("    main+0x%05x..0x%05x addr=0x%08x [%s] (%s, block-%s) "
                  "A=%s B=%s"
                  % (lo, hi, MAIN_BASE + lo, name, memb, half,
                     av[:64], bv[:64]))
        if len(runs(bad)) > 32:
            print("    ... %d more runs" % (len(runs(bad)) - 32))
        if not bad:
            print("    (raw image equal -> divergence is beyond "
                  "effect_array: summons or entities)")
    else:
        print("  no FINERAW pair for f%d" % frame)


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
            # ── F7d fine-grained localization at the divergence onset ───────
            if a["finewin"] or a["fineraw"]:
                print()
                print("fine-grained localization (confirm-seam records):")
                prev = [f for f in common if f < frame]
                if prev:
                    print(" last AGREEING frame f%d:" % prev[-1])
                    fine_diff(a, b, prev[-1])
                print(" first DIVERGENT frame f%d:" % frame)
                fine_diff(a, b, frame)
                later = [f for f in common if f > frame][:2]
                for f2 in later:
                    print(" following frame f%d:" % f2)
                    fine_diff(a, b, f2)

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
