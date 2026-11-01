#!/usr/bin/env python3
"""Measure the timing margin of the N-way society gates (issue #361, step 1).

Runs the N-way gates exactly as CI does and timestamps every wait point the
harness logs, measured from the moment that wait BEGINS (the line the test
prints just before it starts waiting) to the moment it is satisfied (the
"OK:" line).  That elapsed time, against the wait's own budget inside the
test script, is the margin issue #361 asks for.  The outer gate timeouts
(Makefile KEYAUTH_N_GATE_TIMEOUT / SOCIETY_AGENTS_N_GATE_TIMEOUT) are
recorded too, as totals.

Nothing here changes an assertion or a budget (ADR-0016): the gates run
untouched; this script only reads their stdout.

Usage:
  python3 scripts/nway_margin_measure.py --run N [--out nway_margin_run.json]
         [--target ci-smoke-keyauth-n-gate] [--target ci-smoke-society-agents-n-gate]

Every WAIT below is (start marker, end marker, budget seconds, source).  The
markers are literal substrings of lines the tests print today; if a test is
reworded the wait simply records as "unmeasured" rather than lying.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time

# (name, start substring, end substring, budget_s, where the budget lives)
WAITS = [
    # scripts/test_qos_keyauth_n.py — leg (a): C withheld, A/B must keep excluding it
    ("keyauth_leg_a_exclusion",
     "sampling frame-from-C admission on A,B",
     "OK: neither A nor B admits ANY frame from the keyless C",
     16.0 + 5.0,
     "test_qos_keyauth_n.py leg (a) sampling window (16s + slack)"),
    # scripts/test_qos_keyauth_n.py — leg (b): post-key N-way sync
    ("keyauth_post_key_sync",
     "--- leg (b): ADMIT",
     "OK: all three authenticated members synchronized post-key",
     180.0,
     "test_qos_keyauth_n.py await_sync_n(timeout=180)"),
    # scripts/test_qos_society_agents_n.py — 1. each VM's own society completes
    ("agentd_demo_ok",
     "OK: three attested members booted with agentdemo",
     "AGENTD: DEMO OK on all three members",
     150.0,
     "test_qos_society_agents_n.py deadline = now + 150.0"),
    # scripts/test_qos_society_agents_n.py — 3. fields still synchronize under load
    ("society_field_sync",
     "OK: all three societies exchanged results",
     "OK: all three fields synchronized under agentdemo load",
     180.0,
     "test_qos_society_agents_n.py await_sync_n(threshold=0.80, timeout=180)"),
]

# Outer gate budgets, single-sourced in the Makefile; mirrored here only so a
# run's total can be reported against them.  Edit the Makefile, not this.
GATE_BUDGETS_S = {
    "ci-smoke-keyauth-n-gate": 600.0,
    "ci-smoke-society-agents-n-gate": 720.0,
}
GATE_BANNERS = {
    "ci-smoke-keyauth-n-gate": "=== QuantumOS N-Way Authenticated Swarm Plane",
    "ci-smoke-society-agents-n-gate": "=== QuantumOS N-Way Society-of-Societies",
}


def run_gate(target: str, record: dict) -> int:
    """Run one make target, streaming its output and stamping the wait points."""
    t0 = time.monotonic()
    gate = {"target": target, "budget_s": GATE_BUDGETS_S.get(target), "waits": {}}
    record["gates"][target] = gate
    started = {}
    cmd = ["make", target]
    print(f"[measure] $ {' '.join(cmd)}", flush=True)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        now = time.monotonic() - t0
        if GATE_BANNERS.get(target, "\0") in line and "banner_s" not in gate:
            gate["banner_s"] = round(now, 3)
        for name, start, end, budget, src in WAITS:
            if name not in started and start in line:
                started[name] = now
                print(f"[measure] {name}: wait started at {now:.2f}s", flush=True)
            elif name in started and name not in gate["waits"] and end in line:
                elapsed = now - started[name]
                gate["waits"][name] = {
                    "elapsed_s": round(elapsed, 3),
                    "budget_s": budget,
                    "margin_s": round(budget - elapsed, 3),
                    "budget_source": src,
                }
                print(f"[measure] {name}: satisfied after {elapsed:.2f}s "
                      f"(budget {budget:.0f}s, margin {budget - elapsed:.2f}s)", flush=True)
    rc = proc.wait()
    gate["exit_code"] = rc
    gate["total_s"] = round(time.monotonic() - t0, 3)
    for name, start, _end, budget, src in WAITS:
        if name in started and name not in gate["waits"]:
            gate["waits"][name] = {"elapsed_s": None, "budget_s": budget,
                                   "margin_s": None, "budget_source": src,
                                   "note": "started but never satisfied (the gate failed here)"}
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--run", type=int, default=0, help="matrix run index (for the artifact)")
    ap.add_argument("--out", default="nway_margin_run.json")
    ap.add_argument("--target", action="append", default=None,
                    help="make target to run (repeatable); default: both N-way gates")
    a = ap.parse_args()
    targets = a.target or ["ci-smoke-keyauth-n-gate", "ci-smoke-society-agents-n-gate"]
    record = {
        "schema": 1,
        "run": a.run,
        "runner": os.environ.get("RUNNER_NAME", ""),
        "image": os.environ.get("ImageOS", "") + " " + os.environ.get("ImageVersion", ""),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "gates": {},
    }
    worst = 0
    for t in targets:
        rc = run_gate(t, record)
        worst = worst or rc
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
    print(f"[measure] wrote {a.out}", flush=True)
    # The measurement is the deliverable: a failing gate still yields a record,
    # and the matrix uses fail-fast: false, so report the gate's own exit code.
    return worst


if __name__ == "__main__":
    sys.exit(main())
