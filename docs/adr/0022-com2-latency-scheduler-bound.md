# 22. COM2 Round-Trip Latency Is Scheduler-Cadence-Bound

Date: 2026-07-11
Status: Accepted (2026-07-15; the latency fix SHIPPED — the I/O-priority boost, see the final update)

> **Update (2026-10-29) — the starvation referee was never consulted, misread when it was, and
> the guard it referees had a hole (#335).** `ci-smoke-sched` LOAD mode — the only gate that arms
> the max-reschedule-gap ceiling refereeing `SCHED_BOOST_MAX_CONSEC` — was not in CI. Run by hand
> on main it failed 6/6 with maxgap 777–874 ticks. A new `gappid=` field on the SCHED line named
> the victim: pid 5, `flaky-demo`, the demo service the watchdog restarts at ~8 s. A freshly
> created process carried `last_scheduled = 0`, so the fairness scan read its gap as the whole
> uptime whenever the 0.5 s sampler caught it READY and not yet picked (a sub-rotation window, so
> chance decided; two earlier load runs read 46). `process_create` now stamps `last_scheduled`
> with the creation tick. That made the gate green 10/10 — and then a `sched` probe typed every
> 0.25 s from boot showed the real fault the referee had been built for: `alpha`, then `beta`,
> never picked for the probe's whole 20 s (gap 2001 ticks, growing tick for tick), half the boot
> roster never spawned. The plain round-robin scan started after the *current* pid, so every
> boosted pick — a keystroke handover, a COM2 byte, a NIC frame — re-anchored the rotation at the
> boost target; with boosts arriving faster than one rotation (~450 ms of hlt-holding kernel
> threads) the tail of the rotation was never reached, the K=2 cap notwithstanding (the cap only
> guarantees a plain pick, not *which* one). The net thread sits in that tail. Fixed with a
> rotation cursor that only plain picks advance (`rr_cursor`): a boost is now an insertion and the
> rotation resumes where it left off. Resuming the honest rotation exposed the cost the lucky
> table order had been hiding: the forced plain pick after `SCHED_BOOST_MAX_CONSEC` boosts now
> landed on an hlt-looping kernel thread about half the time, and that thread kept the CPU for its
> whole 50 ms quantum while the process whose I/O had just arrived waited — packet captures showed
> exactly two 50 ms gaps per HTTP fetch, at the request and at the FIN (fetch 30 → 170 ms, upload
> wire 117 → 249 ms; on main the plain pick after `qsh` happened to hit a run of quick ring-3
> yielders). So the plain slice is bounded while a boost is pending: `scheduler_tick` hands over
> at the next tick if any READY process is flagged (one table scan per tick; not counted as a
> quantum expiry). The boost's bound is now tick-detect + one tick, not one quantum remainder.
> The gate's PING cadence moves from 0.5 s to 0.25 s — faster than a rotation, which is what lets
> the ceiling see the re-anchoring at all — it prints `maxgap_pid=` and fails if its load did not
> engage, and `ci-smoke-sched-load` is a Makefile target and a CI step. Revert-confirm: the
> stamp-only kernel under the 0.25 s gate reddens with the starved pid named; main's kernel
> reddens on either count.

> **Update (2026-10-28) — the same cadence bound on the network path (#333).**
> After #329/#331 removed the per-segment stalls, every handoff between the net thread and the
> ring-3 program on a connection still went through the full rotation: a program whose receive
> ring had just filled, or whose send ring had just drained, waited its turn behind the
> hlt-looping kernel threads, a frame that arrived while another process ran sat in the driver's
> software RX queue until the net thread's turn, and a SEND/CONNECT/CLOSE/RESOLVE made the thread
> run no sooner. Fixed with this ADR's mechanism, nothing new in the scheduler: (a) every
> syscall-side publish store that hands the net thread work flags it `io_boost`
> (`scheduler_net_kick`), so the caller's own WOULDBLOCK yield dispatches it; (b) the NIC IRQ,
> after the driver's drain, flags it and reschedules at once when it is READY
> (`scheduler_net_rx` — the `scheduler_console_rx` shape, on INTx and the virtio MSI-X RX
> vector); (c) the thread brackets each service pass, boosts the owner of every connection whose
> observable view changed (send side only at a half-free or fully acknowledged ring, so a per-ACK
> ping-pong cannot spend the `SCHED_BOOST_MAX_CONSEC` budget), UDP socket owners on delivery and
> the resolver on an answer, and then YIELDS through the SYS_YIELD gate (`scheduler_yield`)
> instead of sleeping out its quantum — `hlt` as before when nobody was woken. Yields and IRQ
> handovers are not preemptions, so the `ci-smoke-sched` band is unaffected; `SCHED_IO_BOOST 0`
> stubs it all out. Numbers in `docs/NETWORKING.md` and #333.

> **Update (2026-10-22) — the same cadence bound on the COM1 console, fixed by preemption.**
> On the public Firecracker console (tools/qos-console) every keystroke echoed ~350 ms late
> (p50 349.6 ms, spikes every 0.40 s): qsh polls `SYS_CONS_READ` and yields, and each of the
> ~8 hlt-looping kernel threads (alpha, beta, the demo services) keeps the CPU for its whole
> 5-tick quantum, so a key waited out most of the rotation. `SYS_CONS_READ` now notes its caller
> as the console reader (pid, generation) and whether the read was empty; the COM1 RX IRQ, after
> its drain, flags a reader parked on an empty read `io_boost` and calls `scheduler_reschedule`
> at once (`scheduler_console_rx`; the frame swap the tick uses, EOI after it in `irq_handler`).
> One handover per wait, so type-ahead during a running command does not churn the scheduler;
> the live console device cap is the authority and `process_destroy` clears the registration,
> as for the COM2 holder; `SCHED_IO_BOOST 0` stubs it out. Not counted as a preemption, so the
> `ci-smoke-sched` band is unaffected. Measured under Firecracker: echo p50 349.6 → 1.0 ms
> (max 372.9 → 1.6 ms); `imprint` Enter→prompt 117 → 10 ms.

> **Update (2026-07-15) — the deferred epic is implemented: the I/O-priority boost.**
> The option table's principled fix shipped, hardened by a 4-lens adversarial design panel
> (31 findings: 7 blockers, 15 majors — all folded in). Mechanism: a per-PCB `io_boost` flag;
> set (a) at the end of every successful `ipc_send` for a READY receiver (own irq bracket —
> ipc_send had no existing one) and (b) from `scheduler_tick` when the registered COM2 holder
> has RX pending. `pick_next`'s DEFAULT round-robin arm prefers a boosted READY process;
> the flag is consumed only at the dispatch COMMIT (pick_next stays pure — it is also used
> as a bare predicate by the CPUKILL pre-check). Guards the panel forced: a sticky 0xFF
> absent-device latch (an unbacked COM2 port reads all-ones — the boost would otherwise fire
> every tick of every COM2-less CI boot); `SCHED_BOOST_MAX_CONSEC=2` (a boost ping-pong would
> otherwise monopolize every yield-path reschedule AND, by resetting the quantum counter,
> keep quantum expiry itself from firing — starving the roster invisibly to every liveness
> floor); the COM2-holder registration is (pid, generation) + live-cap-validated at the boost
> site and cleared in process_destroy (a bare pid would boost whatever recycles it — the
> ADR-0023 lesson); `boost_count` counts HONORED picks at the commit (a set-side counter is
> the switch_count vacuity trap). Gates re-armed in the same increment: ci-smoke-latency now
> asserts PING median < 0.30 s (boost-off phase-locks to the full ~0.45 s rotation — fail-side
> structural on any host) AND a positive honored-boost delta (binding effect to mechanism),
> plus STATUS median < 0.6 s with a 3-of-5 success floor; ci-smoke-sched LOAD mode paces its
> ping load at a fixed cadence (boost-invariant load profile) and arms a max_gap <= 600-tick
> starvation ceiling — the only metric that can referee the boost's fairness guard.
> Revert-confirm: `SCHED_IO_BOOST 0` reddens both latency assertions while ci-smoke-sched
> stays green (the preempt band is structurally boost-invariant: the expiry increment precedes
> the pick). The boost carries no authority (caps still gate every action) and no audit event
> (it would flood the 256-entry ring); its instrument is the counter.

> **Update (2026-07-11).** Prerequisite 1 below — a bounded latency assertion — **shipped** as
> `ci-smoke-latency` (`scripts/test_qos_latency.py`, via the new `QosVM.ping()` one-hop primitive).
> It asserts a bounded PING/PONG median (the pure transport + scheduler-cadence floor) and records the
> STATUS baseline; it is revert-confirmed against the real regression source — bumping
> `SCHED_QUANTUM_TICKS` reddens it. This gives any future latency-reduction work a proven guard and a
> recorded baseline (~0.45 s PING / ~0.90 s STATUS on the dev box).
>
> **Update (2026-07-13) — Accepted, both prerequisites complete.** Prerequisite 2 (the scheduler
> perf/stability baseline) shipped as `ci-smoke-latency`'s sibling `ci-smoke-sched`
> (`scripts/test_qos_sched.py`, CI job `scheduler-baseline`). A design panel refuted the naive metric —
> aggregate context switches are voluntary-yield-dominated, so `switch_count/tick` is both quantum-
> insensitive and host-throughput-dependent. The kernel instead grew a **dedicated `preempt_count`**
> incremented only at timer-quantum expiry (never on `SYS_YIELD`), exposed via a `SYSINFO_SCHED` sub-op
> / qsh `sched` command / `QosVM.sched()`, alongside a made-live `last_scheduled` and a per-PCB
> `sched_picks` for the fairness/tail snapshot. Calibration (WSL) confirmed **preemptions-per-1000-
> guest-ticks ≈ 1000/quantum, host-invariant** (~200 at q=5 idle+load with <0.5 % variance, ~50 at
> q=20); the gate asserts that scalar in `[100, 600]` plus a liveness floor, revert-confirmed by a
> `SCHED_QUANTUM_TICKS` 5→20 bump (rate drops to ~50, reddens the floor). Both prerequisites the fix was
> gated on now exist. **The latency fix itself (the I/O-priority-boost epic, or a measured quantum
> reduction) stays deferred** — it is now unblocked, with a proven latency gate AND a proven scheduler
> baseline to measure it against, but remains a future epic per this ADR's decision, not an autonomous
> increment.

## Context

Every agent-facing MCP tool that talks to a live VM round-trips over the COM2 serial
bridge — `qos_status` (STATUS), `qos_qpu_submit` (QSUBMIT), and the attestation handshake
(`QosVM.attest`) all block on a request→reply over that wire. So the COM2 round-trip latency
is the single largest floor on agent tool responsiveness.

While debugging the #201 attestation gate (a keyed STATUS retry that offset its nonce by one
on a slow CI runner), the reply latency was measured directly rather than assumed. Two clean
numbers, both low-variance:

- **PING → PONG: ~0.45 s.** The simplest possible round-trip — `swarm_svc` echoes the frame
  itself, no downstream hop (`dispatch_frame` → `FRAME_PONG`).
- **STATUS: ~1.35 s ≈ 3 × 0.45 s.** STATUS adds a `swarm_svc → ghostd → swarm_svc` IPC hop
  (the `GHOST_STATUS` query), so the round-trip crosses ~3 service scheduling boundaries.

The ~0.45 s quantum is not the serial hardware or `ghost_query`; it is the **scheduler
cadence**. The math closes exactly:

- `TIMER_DEFAULT_HZ = 100` → a 10 ms tick (`kernel/include/kernel/interrupts.h:95`).
- `SCHED_QUANTUM_TICKS = 5` → a 50 ms scheduling quantum per process
  (`kernel/include/kernel/scheduler.h:21`; the early-return at `kernel/src/scheduler.c:132`).
- ~9 runnable ring-3 citizens at rest (of the ~18-name roster in `kernel/src/citizens.c`).

`swarm_svc`'s main loop (`poll_com2(); qsub_poll_step(); heartbeat(); yield();`,
`user/swarm_svc.c`) yields voluntarily each pass, but its NEXT turn to read COM2 only comes
around once per full round-robin cycle ≈ `50 ms × ~9 ≈ 450 ms`. A one-hop PING costs one
cycle; a three-hop STATUS costs ~three. That is the whole latency.

## Decision

**Defer** reducing the latency. Do NOT change the timer HZ, the scheduler quantum, or the
service model as an autonomous increment. Record the root cause and the option space here,
and gate any future fix on two prerequisites that do not yet exist:

1. **A COM2 latency regression gate** — a CI smoke test that asserts a bounded PING/PONG (and
   STATUS) round-trip, so a fix can be *proven* and a future change can't silently regress it.
   Today there is no latency assertion, only functional ones.
2. **A perf/stability baseline** for the resonant scheduler — every candidate fit below is
   system-wide and interacts with `SCHED_RESONANT` (which has its own honest-measurement gate).
   An unmeasured change to a core scheduler constant is exactly the kind of edit this project's
   anti-vacuous-gate discipline (ADR-0016) exists to forbid.

## Options (for when the prerequisites exist)

| Option | Effect | Tradeoff |
|---|---|---|
| Lower `SCHED_QUANTUM_TICKS` (5 → 1–2) | ~90–180 ms/cycle; ~2.5–5× faster round-trips | 2.5–5× more context switches system-wide; perturbs `SCHED_RESONANT` fairness/measurement |
| Raise `TIMER_DEFAULT_HZ` (100 → 250/1000) | Finer scheduling granularity | More timer-IRQ overhead for every process; touches every timing assumption (audit ticks, cpu-limit, TCP timers) |
| I/O-priority boost | Reschedule a service the moment its awaited I/O (COM2 byte / IPC reply) is ready | A real new scheduler feature (run-queue + wake path), not a constant tweak — the highest value but the most work |
| Collapse the STATUS hop | `swarm_svc` answers STATUS without the `ghostd` IPC round-trip | Removes ~2/3 of STATUS latency but couples `swarm_svc` to ghostd's field state; changes the service model |

The I/O-priority boost is the principled fix (it targets latency without penalizing throughput
the way a blanket smaller quantum does), but it is a genuine scheduler feature and should be
its own epic behind the latency gate.

## Consequences

**Positive.** The latency is now a *known, measured, root-caused* characteristic with a
number (~0.45 s/hop) and a cause (round-robin cadence), not a mystery. Any future fix has a
concrete target and a clear first task (build the latency gate). The measurement also explains
the #201 nonce-offset failure mode (a retry deadline below the reply latency), already fixed
by pinning `attest()`'s per-attempt deadline above it.

**Negative.** Agents pay ~1.3 s per STATUS tool call and ~0.45 s per PING/QSUBMIT-hop today,
and this ADR ships no improvement — only the diagnosis. Multi-call agent workflows feel
sluggish in proportion to their COM2 chattiness.

**Residual risk.** The ~9-runnable-citizen figure is a rest-state estimate; under load (busy
citizens consuming full quanta) the cycle — and thus every tool's latency — grows. A latency
gate would also surface that tail, which the functional gates do not.

## Evidence

- Measured with a PING/PONG-vs-STATUS harness over the live COM2 socket (PING steady ~0.45 s
  ×5; STATUS ~1.35 s), 2026-07-11, during the #201 (PR 201) attestation debugging.
- Anchors: `kernel/include/kernel/scheduler.h:21` (`SCHED_QUANTUM_TICKS 5`),
  `kernel/include/kernel/interrupts.h:95` (`TIMER_DEFAULT_HZ 100`),
  `kernel/src/scheduler.c:132` (quantum early-return), `user/swarm_svc.c` (`poll_com2` loop).
- Latency gate `ci-smoke-latency` (`scripts/test_qos_latency.py`) — asserts the bounded PING median
  and records the STATUS baseline; revert-confirmed by a `SCHED_QUANTUM_TICKS` bump. It is prerequisite
  (1) above, now shipped, and the first increment of any future latency-reduction epic.
