#!/usr/bin/env python3
"""The framebuffer font must cover every printable ASCII byte.

The framebuffer font (kernel/src/fb.c) used to be deliberately minimal, and
glyph_for() silently renders missing characters as blanks — which is how
the field view's caption shipped reading "OST  E D  E" instead of
"GHOST FIELD LIVE" (found on the first real-hardware boot). Since #264 the
same font draws the whole screen console on UEFI machines (shell output,
kernel log, panic banner), so the contract is stronger: one row per byte
32..126, in order, and every banner literal within that range. This check
makes a missing or misordered glyph a build failure instead of a silent
blank.

Exit 0 if the table is complete, 1 otherwise.
"""
import re
import sys

FB = "kernel/src/fb.c"
FIRST, LAST = 32, 126


def main():
    src = open(FB, encoding="utf-8").read()

    m = re.search(r"font_ascii\[FONT_COUNT\]\[8\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        print("ERROR: font_ascii[FONT_COUNT][8] table not found in fb.c")
        return 1
    rows = re.findall(r"\{\s*(0x[0-9A-Fa-f]{2}(?:\s*,\s*0x[0-9A-Fa-f]{2}){7})\s*\},\s*/\*\s*'(\\?.)'\s*\*/",
                      m.group(1))
    labels = [lab[-1] for _, lab in rows]
    expected = [chr(c) for c in range(FIRST, LAST + 1)]
    if labels != expected:
        bad = [(i, got, want) for i, (got, want) in enumerate(zip(labels, expected)) if got != want]
        print(f"ERROR: font_ascii has {len(labels)} rows, expected {len(expected)} "
              f"(bytes {FIRST}..{LAST} in order); first mismatches: {bad[:5]}")
        return 1
    count = re.search(r"#define FONT_COUNT (\d+)", src)
    if not count or int(count.group(1)) != len(expected):
        print("ERROR: FONT_COUNT does not match the 95 printable ASCII bytes")
        return 1

    banners = set()
    for pat in (r'fb_banner\([^,]+,\s*"([^"]*)"', r'fb_draw_string\([^,]+,[^,]+,\s*"([^"]*)"'):
        for mm in re.finditer(pat, src):
            banners.update(mm.group(1))
    missing = sorted(c for c in banners if not (FIRST <= ord(c) <= LAST))
    if missing:
        print(f"ERROR: fb.c draws characters outside the font's range: {missing}")
        return 1
    print(f"OK: font_ascii covers bytes {FIRST}..{LAST} in order; "
          f"all {len(banners)} distinct banner characters are within range")
    return 0


if __name__ == "__main__":
    sys.exit(main())
