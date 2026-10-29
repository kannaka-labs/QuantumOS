/**
 * QuantumOS Preemptive Scheduler
 *
 * Round-robin preemptive scheduler driven by the PIT timer tick.
 * Context switching is done on the interrupt frame: on a scheduling
 * tick the saved cpu_state_t of the interrupted process is written to
 * its PCB and the next process's saved state is written back onto the
 * frame, so the iretq at the end of irq_common resumes the next
 * process directly.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <kernel/types.h>
#include <kernel/interrupts.h>

/* Scheduling quantum in timer ticks (ticks are 10 ms at TIMER_DEFAULT_HZ) */
#define SCHED_QUANTUM_TICKS 5

/* ADR-0022 I/O-priority boost (the deferred latency epic). When 1, a process
 * whose awaited I/O just arrived — an IPC message delivered to its mailbox,
 * or a COM2 RX byte for the registered COM2 holder — is flagged io_boost and
 * picked out of turn at the next reschedule (default round-robin arm only),
 * collapsing a COM2 hop from ~one full rotation (~450 ms at rest) to
 * tick-detect (<=10 ms) + one quantum remainder (<=50 ms). 0 restores the
 * exact pre-epic pick behavior — the latency gate's revert-confirm flag. */
#define SCHED_IO_BOOST 1

/* Max CONSECUTIVE boosted picks before one plain round-robin pick is forced
 * (the starvation guard). Two IPC-chatty peers ping-ponging sends would
 * otherwise monopolize every yield-path reschedule AND — because each yield
 * resets the global quantum counter — keep quantum expiry (and preempt_count,
 * and the watchdog's turn) from ever firing. K=2 guarantees >=1 plain pick
 * per 3, so a rotation of N runnable completes within ~3N picks and max_gap
 * stays within a small multiple of today's envelope (refereed by the armed
 * maxgap ceiling in ci-smoke-sched); a 1-boost PING hop rides through
 * untouched and STATUS hops absorb at most a couple of plain slices, well
 * inside the armed 0.6 s bound.
 *
 * The forced plain pick resumes the ROTATION, not the table position after
 * the boosted process (#335, rr_cursor in scheduler.c): until then every
 * boosted pick re-anchored the round-robin at the boost target, and boosts
 * faster than one rotation — a console typed at 4 keys/s was enough —
 * starved the tail of the rotation without bound, this cap notwithstanding.
 * And the forced plain slice is bounded to ONE TICK while a boost is pending
 * (scheduler_tick): resuming the honest rotation meant the plain pick landed
 * on an hlt-looping kernel thread half the time, and a 50 ms quantum there
 * put two 50 ms gaps into every HTTP fetch. */
#define SCHED_BOOST_MAX_CONSEC 2

/* Attach the scheduler to the timer tick. Call after process_init(). */
status_t scheduler_init(void);

/* Create a kernel thread: own kmalloc'd stack, entry runs at the given
 * priority with interrupts enabled. The entry function must not return. */
status_t kernel_thread_create(const char *name, void (*entry)(void), uint8_t priority,
                              uint32_t *pid_out);

/* Number of context switches performed by the scheduler */
uint64_t scheduler_get_switches(void);

/* Timer-quantum PREEMPTIONS only (ADR-0022 prereq-2): switches caused by a
 * quantum expiry, excluding voluntary SYS_YIELD. This is the quantum-sensitive,
 * host-invariant quantity a scheduler baseline is built on (see scheduler.c). */
uint64_t scheduler_get_preempts(void);

/* HONORED I/O boosts (ADR-0022): counted only when a boosted pick is actually
 * COMMITTED to the interrupt frame — never on flag-sets — so the counter is
 * non-vacuous by construction (the switch_count lesson: a set-side counter
 * reads nonzero while the pick path is dead). The latency gate asserts a
 * positive delta across its ping batch to bind the measured latency win to
 * the boost mechanism rather than an unrelated cadence change. */
uint64_t scheduler_get_boosts(void);

/* Flag `pid` for one out-of-turn pick iff it is currently READY (ADR-0022).
 * Called by ipc_send after a successful enqueue (inside the caller's irq
 * bracket) and by the COM2 RX check in scheduler_tick. Advisory only: worst
 * case is one extra or one missed boost; never corrupts scheduling state. */
void scheduler_boost_if_ready(uint32_t pid);

/* Register/clear the COM2-holder for the RX boost, as a (pid, generation)
 * pair — a bare pid would re-attach to whatever recycles it (the ADR-0023
 * lesson). Set at every start_slot grant_com2 mint (overwrite semantics, so
 * a watchdog rebirth re-registers structurally); cleared in process_destroy.
 * The boost site additionally validates the live COM2 device cap, keeping
 * the capability table the single source of truth for the authority. */
void scheduler_com2_holder_set(uint32_t pid, uint32_t generation);
void scheduler_com2_holder_clear(uint32_t pid);

/* COM1 keystroke preemption. SYS_CONS_READ notes its caller as the console
 * reader (pid, generation) and whether the read came back empty (`got` == 0:
 * parked waiting for a key); process_destroy clears it. scheduler_console_rx,
 * called from the COM1 RX IRQ after the drain, hands the CPU to a waiting
 * reader at once instead of after the rest of the round-robin rotation. The
 * live console device cap stays the authority, as for the COM2 boost.
 * No-ops when SCHED_IO_BOOST is 0. */
void scheduler_console_reader_note(uint32_t pid, uint32_t generation, uint32_t got);
void scheduler_console_reader_clear(uint32_t pid);
void scheduler_console_rx(cpu_state_t *state);

/* The net thread as a boost target (#333). scheduler_net_thread_set registers
 * the kernel thread's pid at creation. scheduler_net_kick flags it io_boost if
 * READY — called at every syscall-side publish store that hands it work (TCP
 * connect/listen/send/close, a RECV that reopens a closed window, UDP
 * sendto/close, a resolve post) — so the next reschedule, typically the
 * caller's own WOULDBLOCK yield, goes to the stack instead of to the rest of
 * the rotation. scheduler_net_rx, run from the NIC IRQ after the driver's
 * drain, flags the thread and reschedules at once when it is READY (the
 * scheduler_console_rx shape): a frame no longer waits out whichever
 * hlt-looping thread holds the quantum. No-ops when SCHED_IO_BOOST is 0. */
void scheduler_net_thread_set(uint32_t pid);
void scheduler_net_kick(void);
void scheduler_net_rx(cpu_state_t *state);

/* Voluntary yield from a KERNEL thread running with interrupts enabled (never
 * from IRQ context or a cli'd syscall): enters the SYS_YIELD path through the
 * int 0x80 gate, so the frame swap is the one every other reschedule uses, in
 * every scheduler arm. The net thread calls it after a service pass that woke
 * a waiter, handing the CPU to that (boosted) process instead of sleeping out
 * the rest of its quantum (#333). */
void scheduler_yield(void);

/* ADR-0022 prereq-2 fairness/tail snapshot over the runnable ring-3 citizens.
 * Writes max reschedule gap (max now - last_scheduled), the run-count spread
 * (max - min sched_picks), the runnable count, and the pid carrying the max gap
 * (#335: so a starvation reading names its victim). Read-only; `now` is the
 * caller's single timer_get_ticks() read (kept coherent with its tick report). */
void scheduler_get_fairness(uint64_t now, uint64_t *max_gap, uint64_t *spread, uint32_t *runnable_n,
                            uint32_t *gap_pid);

/* Timer callback (exposed for diagnostics/testing) */
void scheduler_tick(cpu_state_t *state);

/* Immediate reschedule (voluntary yield): save the current context and
 * swap the frame to the next READY process, resetting the quantum. */
void scheduler_reschedule(cpu_state_t *state);

/* Switch away from the current process WITHOUT saving its context
 * (used after it has been terminated/killed). */
void scheduler_kill_current(cpu_state_t *state);

#endif /* SCHEDULER_H */
