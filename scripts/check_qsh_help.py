#!/usr/bin/env python3
"""Fail the build when qsh's `help` table and its dispatcher disagree.

user/qsh.c prints `help` from one table, HELP[], and routes commands in execute().
Before #358 the two had drifted (a command listed that never existed as described,
six commands that existed and were not listed). This gate keeps them equal:

  * every command execute() matches with is_cmd(line, "X") / arg_of(line, "X")
    must have a HELP[] row named X, and
  * every HELP[] row must be a command execute() matches.

The check is textual on purpose: it reads the C source, needs no toolchain and runs
in the `build` job alongside the other static gates (ADR-0016: a gate that cannot
fail for the right reason is not a gate, so it is exercised with a removed row and
an unlisted command in its own self-test, `--selftest`).

Usage: python3 scripts/check_qsh_help.py [--print] [--selftest] [path/to/qsh.c]
Exit 0 on agreement, 1 on any difference (each one named).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QSH = ROOT / "user" / "qsh.c"

ROW_RE = re.compile(r'^\s*\{\s*"[^"]*"\s*,\s*"([^"]+)"\s*,', re.M)          # {"group", "name", ...
DISPATCH_RE = re.compile(r'(?:is_cmd|arg_of)\(\s*line\s*,\s*"([^"]+)"\s*\)')


def table_names(src: str) -> list[str]:
    start = src.index("static const struct help_row HELP[] = {")
    end = src.index("};", start)
    return ROW_RE.findall(src[start:end])


def dispatched_names(src: str) -> list[str]:
    start = src.index("static void execute(const char *line) {")
    # the function ends at the next top-level comment block that opens the main loop
    end = src.index("\n/* ----", start)
    seen: list[str] = []
    for name in DISPATCH_RE.findall(src[start:end]):
        if name not in seen:
            seen.append(name)
    return seen


def compare(src: str) -> list[str]:
    table = table_names(src)
    disp = dispatched_names(src)
    problems: list[str] = []
    for name in disp:
        if name not in table:
            problems.append(f"execute() handles `{name}` but HELP[] has no row for it")
    for name in table:
        if name not in disp:
            problems.append(f"HELP[] lists `{name}` but execute() never matches it")
    dup = {n for n in table if table.count(n) > 1}
    for name in sorted(dup):
        problems.append(f"HELP[] lists `{name}` more than once")
    return problems


def selftest(src: str) -> int:
    """Negative controls: the gate must fail when a row is removed or a command is unlisted."""
    ok = True
    if compare(src):
        print("selftest: the real source must pass first")
        return 1
    names = table_names(src)
    victim = names[len(names) // 2]
    row_start = src.index(f'"{victim}",')
    line_start = src.rfind("\n", 0, row_start) + 1
    line_end = src.index("},", row_start) + 2
    without_row = src[:line_start] + src[line_end:]
    if not any(victim in p and "no row" in p for p in compare(without_row)):
        print(f"selftest FAILED: removing the `{victim}` row was not caught")
        ok = False
    extra = src.replace('    } else {\n        cmd_unknown(line);',
                        '    } else if (is_cmd(line, "zzunlisted")) {\n        cmd_unknown(line);\n'
                        '    } else {\n        cmd_unknown(line);', 1)
    if extra == src or not any("zzunlisted" in p for p in compare(extra)):
        print("selftest FAILED: an unlisted dispatched command was not caught")
        ok = False
    print("selftest ok: a removed row and an unlisted command both fail the gate" if ok else "")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    args = [a for a in argv if not a.startswith("--")]
    path = Path(args[0]) if args else QSH
    src = path.read_text(encoding="utf-8")
    if "--print" in argv:
        print("HELP[] rows:", " ".join(table_names(src)))
        print("dispatched: ", " ".join(dispatched_names(src)))
    if "--selftest" in argv:
        return selftest(src)
    problems = compare(src)
    if problems:
        for p in problems:
            print("check_qsh_help:", p)
        print("check_qsh_help: add or remove the HELP[] row in user/qsh.c in the same change "
              "(help is printed from that table; `help <cmd>` reads its detail from it)")
        return 1
    print(f"check_qsh_help: {len(table_names(src))} commands, table and dispatcher agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
