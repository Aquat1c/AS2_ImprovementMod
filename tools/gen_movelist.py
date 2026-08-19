"""Generate include/training/character_moves.h from the translated JP wiki.

The pause menu's trigger action picker should only offer motions the selected
dummy actually has, so the guide is the source of truth for the roster's move
notation. It also carries a defence-category table, which is a free cross-check
on the dword_73E070 trace in docs/re0.7/DEFENSIVE_RESPONSES.md.

The guide writes an Input cell as compound notation - "236A/B/C",
"421A/B/C; j.421A/B/C", "6D or 4D; air OK" - so a token has to be expanded
across its buttons and variants rather than matched whole.

Run from the mod root:  python tools/gen_movelist.py
"""

import zipfile, re, io, os, collections

DOC = os.path.join(os.path.dirname(__file__), '..', '..',
                   'Alice_Senki_2_JPWiki_Translated (1).docx')
OUT = os.path.join(os.path.dirname(__file__), '..', 'include', 'training',
                   'character_moves.h')

ROSTER = ["Rance", "Hatsune", "Patton", "Seed", "Raysen", "Aria", "Maria",
          "Shizuka", "Fanel", "Miki", "Menad", "Hanny King", "Satsu",
          "Tiger Joe", "Escalayer", "Makutsudo", "Alietta", "Nalzgis",
          "Demon Rance", "Little Princess", "TADA", "Nalzgis (Boss)"]
ALIAS = {"Makutsudou": "Makutsudo"}

# Must match kScriptActionLabels in practice_runtime.cpp, index for index.
MOTIONS = ["None", "5X", "2X", "jX", "6X", "4X", "236X", "623X", "214X", "421X",
           "632X", "412X", "22X", "41236X", "214236X", "[2]8X", "2[8]X",
           "[4]6X", "4[6]X", "Jump", "Dash Forward", "Dash Back",
           "63214X", "236236X", "214214X", "632146X", "360X", "2369X",
           "21416X", "66X", "3X", "1X"]

# Motion prefix as the guide writes it -> what the trigger runner can perform.
# Every entry corresponds to a real command in the game's own table at 0x723480;
# a prefix that is absent has no command behind it, so offering it would be a
# move that never comes out. Those are counted and reported instead.
PREFIX_TO_MOTION = {
    '': '5X', '5': '5X', '2': '2X', '6': '6X', '4': '4X',
    '3': '3X', '1': '1X',
    '236': '236X', '623': '623X', '214': '214X', '421': '421X',
    '632': '632X', '412': '412X', '22': '22X',
    '41236': '41236X', '214236': '214236X',
    '[2]8': '[2]8X', '2[8]': '2[8]X', '[4]6': '[4]6X', '4[6]': '4[6]X',
    '63214': '63214X', '236236': '236236X', '214214': '214214X',
    '632146': '632146X', '360': '360X', '2369': '2369X', '21416': '21416X',
    # 66X is the dash attack (forward, neutral, forward + button). "Dash
    # Forward" stays a movement-only trigger with no button.
    '66': '66X', '44': 'Dash Back',
}

# Two pages in the docx are unusable: Escalayer's has no move table at all (only
# prose), and Nalzgis's omits her specials. Their entries here are the |name= and
# |input= fields of the {{MoveData}} blocks on the English wiki, verbatim, and
# replace the docx section for that character rather than merging with it.
# Adding a character here is the fix for any other page the docx renders badly.
WIKI_OVERRIDE = {
    "Nalzgis": [
        "5A", "2A", "5B", "2B", "5C", "2C", "jA", "jB", "jC",
        "Close 6/4C",            # throw, skipped
        "6A", "6C", "6/4D (Air OK)",
        "236A/B (Air OK)", "236C (Air OK)",
        "[2]8X", "j623X", "j214X",
        "21416C",                # Aqualanas, motion the runner has no sequence for
        "j236D",
    ],
    "Escalayer": [
        "5A", "2A", "5B", "2B", "5C", "2C", "jA", "jB", "jC",
        "66A", "66B", "66C",
        "Close 6/4C",            # throw, skipped
        "6A", "6B (Air OK)",
        "236X (Air OK)", "41236X", "623X", "214X (Air OK)",
        "236236C (Air OK)",      # Beatend-Escalation, likewise unsupported
        "236D",
    ],
}

DEFENCE_CATEGORY = [
    ('Defensive movement', 1), ('guard counter', 1),
    ('Parry by guarding', 2),
    ('Attack Parry', 3), ('Repel', 3),
    ('Push-away', 4),
    ('Absolute Defense', 5),
    ('Dodge', 6),
]

# One notation token: optional air prefix, a motion prefix, an optional "+",
# then one or more buttons written as A, A/B/C, the A-C range form, or X for
# "any strength". The trailing group only eats a separator directly followed by
# a button letter, so "236A/B/C" is one token while "4D / 6D" and "2C - Low."
# are not.
TOKEN = re.compile(
    r'(j\.|j(?=[0-9\[ABCDX])|air\s+)?((?:\[?[0-9]\]?)*)\+?([ABCDX])'
    r'((?:\s*[-/]\s*[ABCDX])*)')
SEP = re.compile(r'(?:\s|[;,/]|\bor\b|\band\b)+')

# "6/4D" is two moves sharing a button, not a button list. Split it before the
# scan so the token regex never has to see the alternative form.
SHARED_BUTTON = re.compile(r'(\d)/(\d)([ABCDX])')

# A throw needs the opponent in range, which the trigger runner cannot arrange,
# so offering it would be a move that silently never comes out.
THROW_PREFIX = re.compile(r'^\s*close\b', re.IGNORECASE)

# Pages differ: some use a bare table cell, some write "Name - 236A/B/C" and
# some "Name (236+A-C)". Try the whole line first, then the tail after a name.
NAME_SPLIT = [re.compile(r'\((.+)\)\s*$'),
              re.compile(r'\s[-–—]\s(.+)$'),
              re.compile(r':\s*(.+)$')]

# "Absolute Demon King Defense (22D): A 2-gauge mode activation." - the notation
# is parenthesised mid-line with the description after a colon, so neither the
# trailing-paren pattern nor the length cap would ever reach it.
NAMED_PAREN = re.compile(r'^[^()]{1,48}\(([^()]{1,24})\)\s*[::]')


def doc_lines():
    z = zipfile.ZipFile(DOC)
    d = z.read('word/document.xml').decode('utf-8')
    return [l.strip() for l in re.sub(r'<[^>]+>', '\n', d).split('\n') if l.strip()]


def expand_buttons(first, trailing):
    """A/B/C is a list, A-C is a range, X is any strength, and they can mix."""
    out = [first]
    for sep, b in re.findall(r'\s*([-/])\s*([ABCDX])', trailing or ''):
        if sep == '-' and out[-1] in 'ABCD' and b in 'ABCD':
            lo, hi = 'ABCD'.index(out[-1]), 'ABCD'.index(b)
            out.extend('ABCD'[lo + 1:hi + 1] if lo < hi else [b])
        else:
            out.append(b)

    expanded = []
    for b in out:
        expanded.extend(['A', 'B', 'C'] if b == 'X' else [b])
    seen = []
    for b in expanded:
        if b not in seen:
            seen.append(b)
    return seen


def scan_notation(line):
    """Expand a run of notation into (motion, button, air, notation) tuples.

    Notation always *begins* the run, so the scan is anchored at column 0 and
    whatever is left over after the last token is an annotation ("air OK",
    "on incoming attack"). Prose that merely mentions a move cannot get in,
    because it does not start with one.
    """
    pos, out, unsupported, tokens, sawPrefix = 0, [], 0, 0, False
    while pos < len(line):
        m = SEP.match(line, pos)
        if m and m.end() > pos:
            pos = m.end()
            continue
        m = TOKEN.match(line, pos)
        if not m:
            break
        pos = m.end()
        tokens += 1

        air = bool(m.group(1))
        prefix = m.group(2)
        buttons = expand_buttons(m.group(3), m.group(4))
        sawPrefix = sawPrefix or air or bool(prefix)

        motion = 'jX' if (air and not prefix) else PREFIX_TO_MOTION.get(prefix)
        if not motion:
            unsupported += len(buttons)
            continue
        for b in buttons:
            out.append((MOTIONS.index(motion), 'ABCD'.index(b), air,
                        ('j.' if air else '') + prefix + b))

    # A cell of bare letters is the notation legend ("A / B / C - Weak / Medium
    # / Strong"), never a move row; every real one writes 5A or j.A.
    if not tokens or not sawPrefix:
        return None
    return out, unsupported, sawPrefix


def parse_input_cell(line):
    if THROW_PREFIX.match(line):
        return None

    # Checked before the length cap: this shape carries its notation in the
    # parentheses and its prose after, so the line is long by construction.
    m = NAMED_PAREN.match(line)
    if m:
        hit = scan_notation(SHARED_BUTTON.sub(r' ', m.group(1).strip()))
        if hit and hit[2]:
            return hit[0], hit[1]

    if len(line) > 60:
        return None
    line = SHARED_BUTTON.sub(r'\1\3 \2\3', line)
    hit = scan_notation(line)
    if hit:
        return hit[0], hit[1]
    # "Charging Star - 236A/B/C" / "D-Cutter (236+A-C)": retry past the name,
    # but only accept a real motion prefix there so a sentence ending in a
    # stray letter cannot pass.
    for pattern in NAME_SPLIT:
        m = pattern.search(line)
        if not m:
            continue
        hit = scan_notation(m.group(1).strip())
        if hit and hit[2]:
            return hit[0], hit[1]
    return None


def main():
    lines = doc_lines()

    body_start = None
    for i, l in enumerate(lines):
        if ALIAS.get(l, l) in ROSTER and i + 1 < len(lines) and \
                lines[i + 1].startswith(('Profile', 'Notation', 'Translation', 'Alice Senki')):
            body_start = i
            break

    defence = {}
    for i, l in enumerate(lines[:body_start]):
        key = ALIAS.get(l, l)
        if key in ROSTER and i + 1 < len(lines):
            for needle, cat in DEFENCE_CATEGORY:
                if needle.lower() in lines[i + 1].lower():
                    defence[key] = (cat, lines[i + 1])
                    break

    marks = [(i, ALIAS.get(l, l)) for i, l in enumerate(lines)
             if i >= body_start and ALIAS.get(l, l) in ROSTER]
    sections = collections.OrderedDict()
    dropped = collections.Counter()
    for n, (i, name) in enumerate(marks):
        end = marks[n + 1][0] if n + 1 < len(marks) else len(lines)
        bucket = sections.setdefault(name, collections.OrderedDict())
        source = WIKI_OVERRIDE.get(name) or lines[i:end]
        for l in source:
            parsed = parse_input_cell(l)
            if not parsed:
                continue
            moves, bad = parsed
            dropped[name] += bad
            for motion, button, air, note in moves:
                # An air special is the same input as its ground version - the
                # runner's jX is button-only - so they collapse, and the ground
                # notation wins because that is what the dummy produces standing
                # still.
                key = (motion, button)
                if key not in bucket or (bucket[key][0] and not air):
                    bucket[key] = (air, note)

    print('defence categories parsed: %d' % len(defence))
    for name in ROSTER:
        got = sections.get(name)
        cat = defence.get(name, (0, ''))[0]
        print('  %-16s cat %d  %-10s (%d notations the runner cannot perform)'
              % (name, cat, ('%d moves' % len(got)) if got else '-- none --',
                 dropped[name]))

    o = []
    o.append('#pragma once')
    o.append('')
    o.append('// GENERATED by tools/gen_movelist.py from the translated JP wiki.')
    o.append('// Do not edit by hand; re-run the script instead.')
    o.append('//')
    o.append('// Per-character move notation, reduced to the motions the practice trigger')
    o.append('// runner can actually execute. Motions the runner has no sequence for')
    o.append('// (63214, 21416, ...) are dropped rather than offered.')
    o.append('')
    o.append('#include <stdint.h>')
    o.append('')
    o.append('namespace Training {')
    o.append('')
    o.append('struct CharacterMove {')
    o.append('    uint8_t motion;        // index into kScriptActionLabels')
    o.append('    uint8_t button;        // 0=A 1=B 2=C 3=D')
    o.append("    const char* notation;  // the guide's own text, e.g. \"236A\"")
    o.append('};')
    o.append('')
    for ci, name in enumerate(ROSTER):
        got = sections.get(name) or {}
        if not got:
            continue
        o.append('static const CharacterMove kMoves_%d[] = {  // %s' % (ci, name))
        for (m, b), (air, note) in got.items():
            o.append('    { %2d, %d, "%s" },' % (m, b, note))
        o.append('};')
    o.append('')
    o.append('struct CharacterMoveList {')
    o.append('    const CharacterMove* moves;')
    o.append('    int count;')
    o.append("    // From the guide's own defence table: 1 unique, 2 just-parry, 3 repel,")
    o.append('    // 4 push-away, 5 absolute defence, 6 dodge. Matches dword_73E070.')
    o.append('    uint8_t defenceCategory;')
    o.append('    const char* defenceName;')
    o.append('};')
    o.append('')
    o.append('static const CharacterMoveList kCharacterMoveLists[] = {')
    for ci, name in enumerate(ROSTER):
        got = sections.get(name) or {}
        cat, desc = defence.get(name, (0, ''))
        desc = desc.replace('"', "'")[:40]
        if got:
            o.append('    { kMoves_%d, %d, %d, "%s" },   // %d %s' % (ci, len(got), cat, desc, ci, name))
        else:
            o.append('    { nullptr, 0, %d, "%s" },   // %d %s (no parsed moves)' % (cat, desc, ci, name))
    o.append('};')
    o.append('')
    o.append('constexpr int kCharacterMoveListCount =')
    o.append('    (int)(sizeof(kCharacterMoveLists) / sizeof(kCharacterMoveLists[0]));')
    o.append('')
    o.append('// The pause menu sizes its picker grid against this.')
    o.append('constexpr int kCharacterMoveMaxCount = %d;'
             % max([len(v) for v in sections.values()] or [0]))
    o.append('')
    o.append('} // namespace Training')

    io.open(OUT, 'w', encoding='utf-8', newline='\r\n').write('\n'.join(o) + '\n')
    print('\nwrote %s' % os.path.normpath(OUT))


if __name__ == '__main__':
    main()
