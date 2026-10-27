#!/usr/bin/env python3
"""
Scheduler perf/stability baseline gate (ADR-0022, prerequisite 2).

ADR-0022 root-caused the agent-tool latency floor to the scheduler cadence and DEFERS
the fix behind two prerequisites. Prereq 1 (the COM2 latency gate, test_qos_latency.py)
shipped. This is prereq 2: a perf/stability baseline so a future scheduler-constant
change (the deferred fix) is MEASURED, not blind (ADR-0016 anti-vacuous discipline).

THE METRIC (design-panel-refined, calibration-selected): the design panel's red-team
refuted the naive metric. Aggregate context switches (`switches`) are dominated by
VOLUNTARY yields — every long-lived citizen busy-yields, resetting the quantum counter
thousands of times per 10 ms tick, so the timer-preemption path is nearly dead at rest.
switches/tick is therefore BOTH quantum-insensitive (a gate on it is vacuous) AND host-CPU
dependent (flaky on slow CI). The kernel instead exposes a DEDICATED `preempt` counter,
incremented ONLY on timer-quantum expiry (never on SYS_YIELD). PR-1's calibration confirmed
preempt-per-1000-guest-ticks = ~1000/quantum, host-invariant (~200 at q=5 idle+load with
<0.5% variance, ~50 at q=20). So this gate ASSERTS that scalar in [FLOOR, CEIL] (see the
constants below) — plus a non-vacuous LIVENESS floor so it also reddens on a dead/wedged/
frozen scheduler. Everything asserted is a delta over GUEST ticks, so a slow CI runner reads
the same value (it just takes longer wall-clock to fill the window).

Env:
  QOS_KERNEL       path to kernel.elf32 (else the bridge default)
  QOS_SCHED_LOAD   "1" to drive continuous COM2 PING load while sampling (default idle)
  QOS_SCHED_TICKS  target guest-tick window to accumulate before reporting (default 3000)

Run:  python3 scripts/test_qos_sched.py            # idle baseline
      QOS_SCHED_LOAD=1 python3 scripts/test_qos_sched.py   # under-load baseline
Gate: make ci-smoke-sched
"""
import atexit
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosVM, QosError  # noqa: E402

# The scheduler must genuinely be multiplexing a real roster — a non-vacuous liveness
# floor that reddens on a dead/single-runnable/wedged scheduler but not on any healthy
# cadence. Observed steady-state runnable (READY|RUNNING minus idle/kernel, includes the
# demo kernel threads) is ~20-22 at rest; the floor is set generously below that.
LIVENESS_MIN_SWITCHES = 100   # context switches over the measured window
LIVENESS_MIN_RUNNABLE = 12    # settled runnable count (observed ~20 at rest)

# Armed perf band on the calibration-selected scalar: preemptions per 1000 guest ticks.
# WSL calibration measured preempt/1000t = ~200 at SCHED_QUANTUM_TICKS=5 (idle AND load,
# <0.5% variance) and ~50 at q=20 — i.e. it tracks 1000/quantum, host-invariant (a ratio
# of guest-side counters, so a slow CI runner reads the same value). FLOOR = q5-median x
# 0.5 = 100 is load-bearing: it reddens when the round-robin cadence COARSENS (quantum
# raised past ~10, the sluggish direction ADR-0022 warns about) — a SCHED_QUANTUM_TICKS
# bump to 20 drops P to ~50 and trips it (revert-confirmed). CEIL = q5-median x 3 = 600 is
# a loose thrash cap that does NOT redden on the intended future fix (a SMALLER quantum
# raises P; q=2 -> ~500 still passes), only on pathological churn.
PREEMPT_PER_1000T_FLOOR = 100.0
PREEMPT_PER_1000T_CEIL = 600.0
# Idle-mode pause between `sched` samples. Each sample types "sched\n" into COM1,
# and since the keystroke handover (#248) every byte hands the CPU to the waiting
# shell and restarts the quantum, so the probe itself lowers P. A 50 ms poll read
# ~100 on a kernel whose real cadence is ~200 (#296: bisected to ecddc85; main
# reads ~198 sampled every 5 s). At 2 s the probe's handovers are a few per ~200
# ticks.
IDLE_SAMPLE_INTERVAL_S = 2.0
# Max reschedule-gap ceiling in guest ticks, asserted in LOAD mode only (the
# starvation referee for the ADR-0022 I/O boost; idle-mode gaps are dominated
# by one-shot citizens finishing, so LOAD is where a monopoly would show).
# Baseline ~100-110 ticks; K=2 boost-cap bound ~220; 600 = ~3x margin.
MAXGAP_TICKS_CEIL = 600

# State-gated warmup by GUEST TICKS: the transient boot self-tests (echo, watched-svc,
# quota-test, delegation-test, cpu-hog, qpu-test) run and exit inside the first few
# hundred ticks; cpu-hog in particular holds whole quanta and pollutes the preemption
# regime. Waiting for a tick floor clears them without depending on wall clock.
WARMUP_MAX_S = 25.0
WARMUP_MIN_TICKS = 500        # start measuring only after the boot-transient window
WINDOW_TICKS = int(os.environ.get("QOS_SCHED_TICKS", "3000"))
LOAD = os.environ.get("QOS_SCHED_LOAD", "") == "1"


def _fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    atexit.register(vm.shutdown)
    vm.boot(timeout=45)

    # Warm-up / state gate: wait past the boot-transient window (WARMUP_MIN_TICKS
    # guest ticks) so the transient self-test citizens — cpu-hog especially, which
    # holds whole quanta and pollutes the preemption regime — have exited before the
    # measured window opens.
    settled = None
    t_end = time.time() + WARMUP_MAX_S
    while time.time() < t_end:
        try:
            s = vm.sched(deadline_s=12.0)
        except QosError:
            time.sleep(0.3)
            continue
        if s["ticks"] >= WARMUP_MIN_TICKS:
            settled = s
            break
        time.sleep(0.4)
    if settled is None:
        # Not fatal to REPORT, but record that warmup never reached the tick floor.
        try:
            settled = vm.sched(deadline_s=12.0)
        except QosError:
            _fail("scheduler never produced a SCHED line (dead scheduler / no qsh)")
    print(f"warmup: reached tick={settled['ticks']} runnable={settled['runnable']} "
          f"(load={'on' if LOAD else 'off'})")

    # Sampling window: from S0, optionally drive continuous PING load (the only load
    # that makes the COM2 service chain consume whole quanta), sampling the scheduler
    # counters until >= WINDOW_TICKS guest ticks have elapsed. Everything asserted or
    # reported is a DELTA over guest ticks -> host-CPU-invariant (a slow runner just
    # takes longer wall-clock to fill the window).
    s0 = settled
    last = s0
    max_gap = s0["maxgap"]
    max_spread = s0["spread"]
    min_runnable = s0["runnable"]
    wall_deadline = time.time() + 90.0  # absolute safety bound on wall time
    while (last["ticks"] - s0["ticks"]) < WINDOW_TICKS and time.time() < wall_deadline:
        if LOAD:
            # Fixed 0.5s cadence (ADR-0022 I/O boost): un-paced back-to-back
            # pings would make the load profile a function of the reply
            # latency itself — the boost cut ping latency ~5x, which would
            # silently multiply the load behind the same "load baseline"
            # label and shift the calibration table the maxgap ceiling below
            # is armed against. Pacing makes the profile boost-invariant.
            t_ping = time.time()
            try:
                vm.ping(deadline_s=10.0)
            except QosError:
                pass  # a transient ping hiccup is not a scheduler fault; keep sampling
            remain = 0.5 - (time.time() - t_ping)
            if remain > 0:
                time.sleep(remain)
        else:
            time.sleep(IDLE_SAMPLE_INTERVAL_S)
        try:
            last = vm.sched(deadline_s=12.0)
        except QosError:
            continue
        max_gap = max(max_gap, last["maxgap"])
        max_spread = max(max_spread, last["spread"])
        min_runnable = min(min_runnable, last["runnable"])

    d_ticks = last["ticks"] - s0["ticks"]
    d_switch = last["switches"] - s0["switches"]
    d_preempt = last["preempt"] - s0["preempt"]
    if d_ticks < WINDOW_TICKS // 2:
        _fail(f"window too short (dticks={d_ticks} < {WINDOW_TICKS // 2}) — "
              "guest clock not advancing (scheduler/timer wedged?)")

    # Guest-tick-normalized candidate metrics (the calibration currency).
    p_preempt = 1000.0 * d_preempt / d_ticks
    p_switch = 1000.0 * d_switch / d_ticks

    # THE recorded baseline line — machine-parseable, aggregated across the 10-run
    # q=5/q=80 idle/load calibration table that SELECTS the PR-2 gated scalar.
    print(f"SCHEDBASE: load={1 if LOAD else 0} dticks={d_ticks} dswitch={d_switch} "
          f"dpreempt={d_preempt} preempt_per_1000t={p_preempt:.2f} "
          f"switch_per_1000t={p_switch:.2f} maxgap={max_gap} spread={max_spread} "
          f"runnable_min={min_runnable}")

    # NON-VACUOUS LIVENESS FLOOR: the scheduler must be multiplexing a real roster.
    # Reddens on a dead scheduler (no switches), a single-runnable degenerate, or a
    # frozen guest clock. Checked before the perf band so a wedged scheduler is
    # attributed correctly rather than as a band miss.
    if d_switch < LIVENESS_MIN_SWITCHES:
        _fail(f"scheduler not multiplexing: {d_switch} switches over {d_ticks} ticks "
              f"(< {LIVENESS_MIN_SWITCHES}) — dead/wedged scheduler")
    if min_runnable < LIVENESS_MIN_RUNNABLE:
        _fail(f"runnable roster collapsed to {min_runnable} (< {LIVENESS_MIN_RUNNABLE}) "
              "— citizens not staying schedulable")

    # ARMED PERF BAND (the calibration-selected scalar): preemptions per 1000 guest
    # ticks must sit in [FLOOR, CEIL]. The floor catches a coarsened round-robin cadence
    # (the scheduler getting more sluggish — ADR-0022's regression direction); the loose
    # ceiling catches pathological thrash without reddening on the intended smaller-
    # quantum fix. Revert-confirmed: SCHED_QUANTUM_TICKS 5 -> 20 drops P to ~50 < FLOOR.
    if p_preempt < PREEMPT_PER_1000T_FLOOR:
        _fail(f"preemption rate {p_preempt:.1f}/1000t below floor {PREEMPT_PER_1000T_FLOOR:.0f} "
              f"— round-robin cadence has COARSENED (quantum raised? scheduler more sluggish). "
              f"dpreempt={d_preempt} over dticks={d_ticks}. See ADR-0022.")
    if p_preempt > PREEMPT_PER_1000T_CEIL:
        _fail(f"preemption rate {p_preempt:.1f}/1000t above ceiling {PREEMPT_PER_1000T_CEIL:.0f} "
              f"— pathological reschedule thrash. dpreempt={d_preempt} over dticks={d_ticks}.")

    # ARMED FAIRNESS CEILING (ADR-0022 I/O boost): max reschedule-gap over the
    # window. This is the ONLY metric that can referee the boost's starvation
    # guard — a boost ping-pong monopoly keeps every liveness floor green
    # (starved processes stay READY; the pair switches plenty) and can even
    # keep the preempt band green outside the monopoly window, while max_gap
    # grows without bound. Baseline ~100-110 ticks (~1 rotation); the K=2
    # consecutive-boost cap bounds it at ~2-3 rotations (~220); ceiling 600 =
    # ~3x margin. Revert-confirm: removing the SCHED_BOOST_MAX_CONSEC guard
    # under LOAD drives the gap toward the whole 3000-tick window.
    if LOAD and max_gap > MAXGAP_TICKS_CEIL:
        _fail(f"max reschedule gap {max_gap} ticks exceeds {MAXGAP_TICKS_CEIL} — a citizen "
              f"is being starved (I/O-boost monopoly? see ADR-0022 SCHED_BOOST_MAX_CONSEC)")
    print(f"OK: scheduler live ({d_switch} switches, {min_runnable} runnable over "
          f"{d_ticks} ticks); preemption rate {p_preempt:.1f}/1000t within "
          f"[{PREEMPT_PER_1000T_FLOOR:.0f}, {PREEMPT_PER_1000T_CEIL:.0f}].")
    print("=== scheduler baseline gate PASSED (liveness + preemption-rate band) ===")


if __name__ == "__main__":
    main()
