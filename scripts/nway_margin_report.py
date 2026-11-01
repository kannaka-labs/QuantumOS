#!/usr/bin/env python3
"""Aggregate N-way gate margin records (issue #361, step 1).

Reads every nway_margin_run.json written by scripts/nway_margin_measure.py
(one per matrix run, downloaded as artifacts) and prints, per wait point,
the min / median / max elapsed time against that wait's OWN budget — the
150 s and 180 s deadlines inside the test scripts that actually fire on a
slow runner — plus each gate's total against the Makefile timeout.

The margin is budget minus the WORST observed elapsed.  A margin under
about 20% of the budget means the gate is living on runner timing, which is
what #361 suspects; a negative margin is a run that timed out.

Usage:
  python3 scripts/nway_margin_report.py [DIR]      # default: .
  python3 scripts/nway_margin_report.py DIR --json  # machine-readable too
"""
from __future__ import annotations

import glob
import json
import os
import statistics
import sys


def load(root: str) -> list[dict]:
    files = sorted(glob.glob(os.path.join(root, "**", "nway_margin_run.json"), recursive=True))
    out = []
    for f in files:
        try:
            with open(f, encoding="utf-8") as fh:
                d = json.load(fh)
        except (OSError, ValueError) as e:
            print(f"warning: skipping {f}: {e}", file=sys.stderr)
            continue
        if d.get("schema") != 1:
            print(f"warning: skipping {f}: unknown schema {d.get('schema')!r}", file=sys.stderr)
            continue
        out.append(d)
    return out


def summarize(records: list[dict]) -> dict:
    waits: dict[str, dict] = {}
    gates: dict[str, dict] = {}
    for r in records:
        for target, g in r.get("gates", {}).items():
            gt = gates.setdefault(target, {"budget_s": g.get("budget_s"), "totals": [],
                                           "failures": 0, "runs": 0})
            gt["runs"] += 1
            if g.get("exit_code", 0) != 0:
                gt["failures"] += 1
            if g.get("total_s") is not None:
                gt["totals"].append(g["total_s"])
            for name, w in g.get("waits", {}).items():
                wt = waits.setdefault(name, {"budget_s": w.get("budget_s"),
                                             "source": w.get("budget_source", ""),
                                             "elapsed": [], "unsatisfied": 0})
                if w.get("elapsed_s") is None:
                    wt["unsatisfied"] += 1
                else:
                    wt["elapsed"].append(w["elapsed_s"])

    def stats(vals):
        if not vals:
            return None
        return {"n": len(vals), "min": min(vals), "median": statistics.median(vals), "max": max(vals)}

    for w in waits.values():
        s = stats(w["elapsed"])
        w["stats"] = s
        w["margin_s"] = (w["budget_s"] - s["max"]) if (s and w["budget_s"] is not None) else None
    for g in gates.values():
        s = stats(g["totals"])
        g["stats"] = s
        g["margin_s"] = (g["budget_s"] - s["max"]) if (s and g["budget_s"] is not None) else None
    return {"runs": len(records), "waits": waits, "gates": gates}


def fmt(v, unit="s"):
    return "   n/a" if v is None else f"{v:6.1f}{unit}"


def print_report(summary: dict) -> None:
    print(f"N-way gate margin report: {summary['runs']} run(s)")
    print()
    print(f"{'wait point':<28} {'n':>3} {'min':>8} {'median':>8} {'max':>8} {'budget':>8} {'margin':>8}  budget source")
    print("-" * 110)
    for name, w in summary["waits"].items():
        s = w["stats"] or {}
        flag = ""
        if w["margin_s"] is not None and w["budget_s"]:
            if w["margin_s"] < 0:
                flag = "  TIMED OUT"
            elif w["margin_s"] < 0.2 * w["budget_s"]:
                flag = "  THIN"
        unsat = f"  ({w['unsatisfied']} never satisfied)" if w["unsatisfied"] else ""
        print(f"{name:<28} {s.get('n', 0):>3} {fmt(s.get('min'))} {fmt(s.get('median'))} "
              f"{fmt(s.get('max'))} {fmt(w['budget_s'])} {fmt(w['margin_s'])}  {w['source']}{flag}{unsat}")
    print()
    print(f"{'gate (total)':<28} {'n':>3} {'min':>8} {'median':>8} {'max':>8} {'budget':>8} {'margin':>8}  failures")
    print("-" * 110)
    for target, g in summary["gates"].items():
        s = g["stats"] or {}
        print(f"{target:<28} {s.get('n', 0):>3} {fmt(s.get('min'))} {fmt(s.get('median'))} "
              f"{fmt(s.get('max'))} {fmt(g['budget_s'])} {fmt(g['margin_s'])}  {g['failures']}/{g['runs']}")
    print()
    print("margin = budget - worst observed.  THIN = under 20% of the budget; negative = a timeout.")
    print("Budgets are the test scripts' own deadlines (see 'budget source'); nothing here changes them.")


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = args[0] if args else "."
    records = load(root)
    if not records:
        print(f"no nway_margin_run.json records under {root}", file=sys.stderr)
        return 1
    summary = summarize(records)
    print_report(summary)
    if "--json" in sys.argv:
        print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
