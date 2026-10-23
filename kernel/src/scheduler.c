/**
 * QuantumOS Preemptive Scheduler Implementation
 *
 * Round-robin over the process table, driven by the PIT timer tick.
 * The context switch happens on the interrupt frame: irq_common saves
 * the full cpu_state_t on the stack and passes it to the C handler;
 * writing a different process's saved state onto that frame makes the
 * final iretq resume that process.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/vmspace.h>
#include <kernel/boot.h>
#include <kernel/manifest.h>
#include <kernel/audit.h>
#include <kernel/capability.h>
#include <kernel/com2_uart.h> /* com2_rx_pending + DEVICE_ID_COM2 (ADR-0022 RX boost) */
#include <kernel/console.h>   /* early_console_write for the CPUKILL boot line */
#ifdef SCHED_LOTTERY
#include <kernel/quantum.h>
#endif
#ifdef SCHED_RESONANT
#include <kernel/resonant_fixed.h>
#endif

static uint64_t switch_count = 0;
static uint32_t quantum_counter = 0;
/* Timer-quantum PREEMPTIONS only (ADR-0022 prereq-2): incremented exclusively on
 * the quantum-expiry path in scheduler_tick, NEVER on a voluntary SYS_YIELD. This
 * is the distinction that makes the scheduler baseline non-vacuous: switch_count is
 * dominated by voluntary yields (citizens busy-yield every loop pass, resetting
 * quantum_counter before it reaches SCHED_QUANTUM_TICKS), so switch_count/tick is
 * insensitive to the quantum. preempt_count is 1/quantum-paced by construction and
 * so is the quantity a SCHED_QUANTUM_TICKS change actually moves. */
static uint64_t preempt_count = 0;

#if SCHED_IO_BOOST
/* HONORED boosts only (ADR-0022): incremented at the dispatch COMMIT points
 * below, never where a flag is set — a set-side counter would read nonzero
 * with the pick path dead (the switch_count vacuity lesson, prereq-2). */
static uint64_t boost_count = 0;
/* Consecutive boosted picks since the last plain pick (the starvation guard,
 * see SCHED_BOOST_MAX_CONSEC). Single global counter; reset on any plain
 * pick, checked before the boost scan. */
static uint32_t consec_boost = 0;
/* Whether the LAST pick_next call selected via the boost scan. File-static
 * rather than a pick_next side effect on the PCB: pick_next stays PURE (it
 * is also used as a bare predicate by scheduler_tick's CPUKILL pre-check,
 * which must never consume a boost), and the commit points read this
 * immediately after their own pick_next call. */
static uint8_t pick_was_boost = 0;
/* The registered COM2 holder for the RX boost, as (pid, generation) — a bare
 * pid would boost whatever process recycles it (the ADR-0023 stale-pid
 * lesson). generation 0 with pid 0 = no holder. */
static uint32_t com2_holder_pid = 0;
static uint32_t com2_holder_gen = 0;
/* The console reader for the COM1 keystroke preemption, as (pid, generation),
 * plus whether its LAST SYS_CONS_READ came back empty — i.e. it is parked in
 * its poll-and-yield loop waiting for a key, not busy running a command. */
static uint32_t cons_reader_pid = 0;
static uint32_t cons_reader_gen = 0;
static uint8_t cons_reader_waiting = 0;
#endif

/* PID of the idle process (created second in process_init) */
#define IDLE_PROCESS_ID (KERNEL_PROCESS_ID + 1)

/**
 * Round-robin pick: scan the table starting after the current PID for a
 * READY process with a runnable context. The idle process is only
 * chosen when nothing else is ready.
 */
static process_t *pick_next(uint32_t from_pid) {
#if defined(SCHED_RESONANT)
    /* Resonant pick (opt-in, SCHED_RESONANT): the next process emerges from
     * the fixed-point coupled-oscillator field (Kuramoto order parameter +
     * per-process resonant priority) rather than table order. Integer-only,
     * so it is safe in this interrupt context — the dormant double-precision
     * scheduler is NOT called here (see resonant_fixed.h). */
    return resonant_fixed_pick(from_pid);
#elif defined(SCHED_LOTTERY)
    /* Lottery pick (opt-in, SCHED_LOTTERY): gather every ready, runnable,
     * non-idle process and draw one uniformly using the kernel's qseed-mixed
     * generator (integer only). The idle process remains the fallback when
     * nothing else is ready, exactly as in round-robin. */
    process_t *cand[MAX_PROCESSES];
    uint32_t n = 0;
    process_t *idle = NULL;

    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = process_get_by_pid(pid);
        if (!p || p->state != PROCESS_STATE_READY || !p->context_valid) {
            continue;
        }
        if (pid == IDLE_PROCESS_ID) {
            idle = p;
            continue;
        }
        cand[n++] = p;
    }

    (void)from_pid;
    if (n == 0) {
        return idle;
    }
    return cand[quantum_kernel_rand() % n];
#else
    process_t *idle = NULL;

#if SCHED_IO_BOOST
    /* Boost scan (ADR-0022): a READY citizen whose awaited I/O just arrived
     * is picked out of turn — unless the consecutive-boost budget is spent,
     * which forces a plain pick so a boost ping-pong can never starve the
     * rotation (or, by resetting the quantum counter on every yield, keep
     * quantum expiry itself from ever firing). READ-ONLY: the flag is
     * consumed at the dispatch commit, never here — this function is also
     * called as a bare predicate (the CPUKILL pre-check) and on the kill
     * path twice, and a scan-side clear would eat boosts with no dispatch.
     * Default round-robin arm ONLY: the RESONANT/LOTTERY arms above return
     * first, so this is a compile-time no-op for the opt-in experiments. */
    pick_was_boost = 0;
    if (consec_boost < SCHED_BOOST_MAX_CONSEC) {
        for (uint32_t off = 1; off <= MAX_PROCESSES; off++) {
            uint32_t pid = (from_pid + off) % MAX_PROCESSES;
            process_t *p = process_get_by_pid(pid);
            if (!p || p->state != PROCESS_STATE_READY || !p->context_valid || !p->io_boost) {
                continue;
            }
            if (pid == IDLE_PROCESS_ID) {
                continue; /* idle is never boost-eligible */
            }
            pick_was_boost = 1;
            return p;
        }
    }
#endif

    for (uint32_t off = 1; off <= MAX_PROCESSES; off++) {
        uint32_t pid = (from_pid + off) % MAX_PROCESSES;
        process_t *p = process_get_by_pid(pid);
        if (!p || p->state != PROCESS_STATE_READY) {
            continue;
        }
        if (!p->context_valid) {
            continue; /* nothing runnable saved yet */
        }
        if (pid == IDLE_PROCESS_ID) {
            idle = p;
            continue;
        }
        return p;
    }

    return idle;
#endif
}

#if SCHED_IO_BOOST
/* Consume a boost at the dispatch COMMIT: unconditionally shed the flag on
 * every committed switch (a process dispatched by ANY path must not carry a
 * stale boost into a later, unrelated pick), and account the boost budget /
 * counter only when the boost scan actually made this pick. */
static void boost_commit(process_t *next) {
    next->io_boost = 0;
    if (pick_was_boost) {
        boost_count++;
        consec_boost++;
    } else {
        consec_boost = 0;
    }
}

void scheduler_boost_if_ready(uint32_t pid) {
    process_t *p = process_get_by_pid(pid);
    if (p && p->state == PROCESS_STATE_READY) {
        p->io_boost = 1;
    }
}

void scheduler_com2_holder_set(uint32_t pid, uint32_t generation) {
    com2_holder_pid = pid;
    com2_holder_gen = generation;
}

void scheduler_com2_holder_clear(uint32_t pid) {
    if (com2_holder_pid == pid) {
        com2_holder_pid = 0;
        com2_holder_gen = 0;
    }
}

void scheduler_console_reader_note(uint32_t pid, uint32_t generation, uint32_t got) {
    cons_reader_pid = pid;
    cons_reader_gen = generation;
    cons_reader_waiting = (got == 0);
}

void scheduler_console_reader_clear(uint32_t pid) {
    if (cons_reader_pid == pid) {
        cons_reader_pid = 0;
        cons_reader_gen = 0;
        cons_reader_waiting = 0;
    }
}

/* COM1 keystroke preemption, run from the COM1 RX IRQ after the drain (IRQ
 * context, IF=0). Without it a keystroke waits for the reader's next turn in
 * the round-robin: every hlt-looping kernel thread (alpha, beta, the demo
 * services) keeps the CPU for its whole quantum, so with ~8 of them READY the
 * shell echoed each key ~350 ms late on the public console. Here the reader
 * is flagged and the CPU handed over at once — the same frame swap the timer
 * tick does, and the EOI follows in irq_handler exactly as for the tick.
 * Only when the reader is parked waiting for input (its last read was empty),
 * so type-ahead during a running command never churns the scheduler. */
void scheduler_console_rx(cpu_state_t *state) {
    if (cons_reader_pid == 0 || !cons_reader_waiting || !console_rx_pending()) {
        return;
    }
    if (process_get_generation(cons_reader_pid) != cons_reader_gen) {
        return;
    }
    if (cap_find_resource(cons_reader_pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_CONSOLE) !=
        CAP_SUCCESS) {
        return;
    }
    process_t *p = process_get_by_pid(cons_reader_pid);
    process_t *cur = process_get_current();
    if (!p || p == cur || p->state != PROCESS_STATE_READY || !p->context_valid) {
        return;
    }
    p->io_boost = 1;
    cons_reader_waiting = 0; /* one handover per wait; the next empty read re-arms */
    scheduler_reschedule(state);
}

/* COM2 RX boost check, run from scheduler_tick (IRQ context, IF=0). Cost at
 * rest: one io_inb of LSR (com2_rx_pending; a sticky latch makes COM2-less
 * boots free after the first read). The heavier validation below runs only
 * when bytes are actually pending. */
static void com2_rx_boost_check(void) {
    if (com2_holder_pid == 0 || !com2_rx_pending()) {
        return;
    }
    /* Generation guard (the ADR-0023 lesson): never boost whatever process
     * recycled the holder's pid. */
    if (process_get_generation(com2_holder_pid) != com2_holder_gen) {
        return;
    }
    /* Single source of truth: the registration is only an index hint — the
     * AUTHORITY is the live COM2 device cap. A revoked cap (teardown in
     * progress, or a future explicit revoke) must stop the boost with it. */
    if (cap_find_resource(com2_holder_pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_COM2) !=
        CAP_SUCCESS) {
        return;
    }
    process_t *p = process_get_by_pid(com2_holder_pid);
    if (!p) {
        return;
    }
    /* READY: normal case. RUNNING: the holder is the interrupted process
     * itself — flag it anyway so an expiry on this same tick re-picks it
     * instead of rotating it out with data waiting (saves one extra tick). */
    if (p->state == PROCESS_STATE_READY || p->state == PROCESS_STATE_RUNNING) {
        p->io_boost = 1;
    }
}
#endif

void scheduler_tick(cpu_state_t *state) {
#if SCHED_IO_BOOST
    /* ADR-0022: flag the COM2 holder the moment host bytes are waiting, so
     * the pick below (or the next reschedule from any path) delivers them at
     * tick granularity instead of one full rotation later. */
    com2_rx_boost_check();
#endif
    /* Manifest CPU accounting (epic #135): charge the interrupted process
     * one tick BEFORE the quantum early-return — after it, this would count
     * reschedules and undercount by SCHED_QUANTUM_TICKS x. IRQ context: IF
     * is already 0, and unbound pids (kernel, idle) hit a dead entry. */
    process_t *cur = process_get_current();
    if (cur) {
        manifest_tick(cur->pid);
        /* CPU-quota enforcement (epic #144): a ring-3 process that has exceeded
         * its DECLARED cpu budget is terminated — the manifest's cpu_ticks
         * becomes a real bound, not just accounting. The (state->cs & 3) guard
         * NEVER kills a process caught mid-syscall in ring 0: cli'd syscalls run
         * with a live kernel stack, and discarding it would corrupt state (a
         * held cli window, a half-built cap/manifest). cpu_ticks >= cpu_limit is
         * a LATCH, so a process skipped in ring 0 this tick is caught the next
         * tick it is interrupted back in ring 3. The pick_next pre-check keeps
         * this POLICY action from ever panicking (scheduler_kill_current
         * boot_panics if nothing is runnable) — if there is no target, skip and
         * let the latch retry. The frame-swap + EOI-after-callback is the exact
         * mechanism scheduler_reschedule already ships on every context switch. */
        if ((state->cs & 3) != 0 && manifest_over_budget(cur->pid) && pick_next(cur->pid)) {
            audit_record(AUDIT_CPUKILL, cur->pid, AUDIT_V_EPERM, CAP_RESOURCE_PROCESS, cur->pid, 0);
            early_console_write("CPUKILL: pid=");
            early_console_write_hex(cur->pid);
            early_console_write(" over CPU budget\r\n");
            /* Clear the manifest AT the kill (irqsave-safe, idempotent with the
             * reaper's later process_destroy clear) so the killed pid vanishes
             * from SYS_MANIFEST synchronously — not in a race with the idle
             * reaper. */
            manifest_clear(cur->pid);
            process_set_state(cur->pid, PROCESS_STATE_TERMINATED);
            scheduler_kill_current(state);
            return;
        }
    }
    if (++quantum_counter < SCHED_QUANTUM_TICKS) {
        return;
    }
    /* The quantum expired: this is a genuine timer preemption (the process ran a
     * full SCHED_QUANTUM_TICKS slice without yielding). Count it here, on the
     * expiry path ONLY, so preempt_count excludes the voluntary SYS_YIELD calls
     * that also route through scheduler_reschedule (ADR-0022 prereq-2). */
    preempt_count++;
    scheduler_reschedule(state);
}

void scheduler_reschedule(cpu_state_t *state) {
    quantum_counter = 0;

    process_t *cur = process_get_current();
    if (!cur) {
        return;
    }

    process_t *next = pick_next(cur->pid);
    if (!next || next == cur) {
        return;
    }

    /* Save the interrupted context into the current PCB */
    cur->context = *state;
    cur->context_valid = true;
    cur->rip = state->rip;
    cur->rsp = state->rsp;
    if (cur->state == PROCESS_STATE_RUNNING) {
        process_set_state(cur->pid, PROCESS_STATE_READY);
    }

    /* Bookkeeping: current_process pointer, statistics */
    process_switch_to(next);
    process_set_state(next->pid, PROCESS_STATE_RUNNING);
#if SCHED_IO_BOOST
    boost_commit(next); /* consume the flag exactly when the switch commits */
#endif

    /* Restore the next context onto the interrupt frame; the iretq at
     * the end of irq_common resumes it. Switch to the next process's
     * address space first — safe because the kernel half (this code,
     * the interrupt stack, the frame) is mapped identically in every
     * space. */
    vmspace_switch(next->cr3 ? next->cr3 : vmspace_kernel_cr3());
    *state = next->context;

    switch_count++;
    if (switch_count == 1) {
        boot_log("scheduler: first context switch");
    }
#ifdef SCHED_LOTTERY
    /* Periodically report the running total of quantum-derived bits the
     * lottery has consumed (the "reap/idle report"). */
    if ((switch_count & 0xFFu) == 0) {
        early_console_write("scheduler[lottery]: quantum bits consumed = ");
        early_console_write_hex(quantum_bits_consumed());
        early_console_write("\r\n");
    }
#endif
#ifdef SCHED_RESONANT
    /* Periodic live honesty report: the running order parameter r and the
     * per-process run-count spread (fairness) for the REAL workload — printed
     * whether resonant scheduling is winning or losing. */
    if ((switch_count & 0xFFu) == 0) {
        resonant_fixed_live_report(switch_count);
    }
#endif
}

void scheduler_kill_current(cpu_state_t *state) {
    process_t *cur = process_get_current();
    process_t *next = pick_next(cur ? cur->pid : 0);
    if (!next) {
        boot_panic("scheduler: nothing runnable after kill");
        return;
    }

    /* A fresh process starts a fresh quantum (symmetric with
     * scheduler_reschedule) — otherwise `next` inherits the dead process's
     * partial quantum_counter and gets a truncated first slice. */
    quantum_counter = 0;

    /* Do not save the dead process's context */
    process_switch_to(next);
    process_set_state(next->pid, PROCESS_STATE_RUNNING);
#if SCHED_IO_BOOST
    boost_commit(next); /* same commit-point consume as scheduler_reschedule */
#endif
    vmspace_switch(next->cr3 ? next->cr3 : vmspace_kernel_cr3());
    *state = next->context;
    switch_count++;
}

uint64_t scheduler_get_switches(void) {
    return switch_count;
}

uint64_t scheduler_get_preempts(void) {
    return preempt_count;
}

uint64_t scheduler_get_boosts(void) {
#if SCHED_IO_BOOST
    return boost_count;
#else
    return 0;
#endif
}

#if !SCHED_IO_BOOST
/* Flag-off stubs keep the call sites (ipc_send, start_slot, process_destroy)
 * unconditional — the revert-confirm build differs ONLY in pick behavior. */
void scheduler_boost_if_ready(uint32_t pid) {
    (void)pid;
}
void scheduler_com2_holder_set(uint32_t pid, uint32_t generation) {
    (void)pid;
    (void)generation;
}
void scheduler_com2_holder_clear(uint32_t pid) {
    (void)pid;
}
void scheduler_console_reader_note(uint32_t pid, uint32_t generation, uint32_t got) {
    (void)pid;
    (void)generation;
    (void)got;
}
void scheduler_console_reader_clear(uint32_t pid) {
    (void)pid;
}
void scheduler_console_rx(cpu_state_t *state) {
    (void)state;
}
#endif

/* ADR-0022 prereq-2 fairness/tail snapshot over the runnable ring-3 citizens
 * (state READY or RUNNING, excluding the kernel and idle processes):
 *   *max_gap    = max(now - last_scheduled)         -- worst reschedule-gap tail
 *   *spread     = max(sched_picks) - min(sched_picks) -- run-count fairness spread
 *   *runnable_n = count of such processes            -- the ~9 long-lived roster at rest
 * `now` is passed in (read once by the caller via timer_get_ticks) so the gap is
 * coherent with the tick count reported on the same line. Read-only: it inspects
 * process state and counters and changes no scheduling decision. */
void scheduler_get_fairness(uint64_t now, uint64_t *max_gap, uint64_t *spread,
                            uint32_t *runnable_n) {
    uint64_t gap = 0;
    uint64_t min_picks = (uint64_t)-1;
    uint64_t max_picks = 0;
    uint32_t n = 0;
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = process_get_by_pid(pid);
        if (!p) {
            continue;
        }
        if (p->state != PROCESS_STATE_READY && p->state != PROCESS_STATE_RUNNING) {
            continue;
        }
        if (pid == IDLE_PROCESS_ID || pid == KERNEL_PROCESS_ID) {
            continue;
        }
        n++;
        uint64_t g = now - p->last_scheduled; /* now is monotonic >= last_scheduled */
        if (g > gap) {
            gap = g;
        }
        if (p->sched_picks < min_picks) {
            min_picks = p->sched_picks;
        }
        if (p->sched_picks > max_picks) {
            max_picks = p->sched_picks;
        }
    }
    if (max_gap) {
        *max_gap = gap;
    }
    if (spread) {
        *spread = (n > 0) ? (max_picks - min_picks) : 0;
    }
    if (runnable_n) {
        *runnable_n = n;
    }
}

status_t kernel_thread_create(const char *name, void (*entry)(void), uint8_t priority,
                              uint32_t *pid_out) {
    process_create_params_t params = {.name = name,
                                      .type = PROCESS_TYPE_KERNEL,
                                      .priority = priority,
                                      .parent_pid = KERNEL_PROCESS_ID,
                                      .entry_point = (void *)entry,
                                      .stack_address = NULL, /* process_create kmallocs a stack */
                                      .stack_size = PROCESS_STACK_SIZE,
                                      .is_quantum_aware = false};

    process_t *thread = NULL;
    status_t result = process_create(&params, &thread);
    if (result != STATUS_SUCCESS) {
        return result;
    }

    if (pid_out) {
        *pid_out = thread->pid;
    }
    return STATUS_SUCCESS;
}

status_t scheduler_init(void) {
    timer_set_callback(scheduler_tick);
    boot_log("Scheduler attached to timer tick");
    return STATUS_SUCCESS;
}
