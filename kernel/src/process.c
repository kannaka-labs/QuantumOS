/**
 * QuantumOS Process Management System Implementation
 *
 * Process lifecycle management for the QuantumOS microkernel.
 * Implements process creation, scheduling, and destruction with
 * capability-based security integration.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/ipc.h>
#include <kernel/boot.h>
#include <kernel/types.h>
#include <kernel/capability.h>
#include <kernel/manifest.h>
#include <kernel/interrupts.h> /* timer_get_ticks() for the ADR-0022 sched timing seam */
#include <kernel/scheduler.h>  /* scheduler_com2_holder_clear (ADR-0022 RX boost) */
#include <kernel/qpu.h>
#include <kernel/quantum.h>
#include <kernel/gdt.h>
#include <kernel/vmspace.h>
#include <kernel/net.h>

/* Local strncpy implementation (no libc in freestanding kernel) */
static char *strncpy_local(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/* PROCESS_STACK_SIZE lives in process.h */

/* ============================================================================
 * Internal State
 * ============================================================================ */

/* Process table - all processes in the system */
static process_t process_table[MAX_PROCESSES];
static bool process_table_initialized = false;

/* Per-slot generation counter, bumped every time a slot is (re)used.
 * PIDs are slot indices allocated first-fit, so a freed slot's pid can
 * be handed to an unrelated process; a stale holder of (pid) can be
 * disambiguated from the current occupant by comparing generations.
 * Lives outside the PCB so init_process_pcb's memset can't reset it. */
static uint32_t process_generation[MAX_PROCESSES];

/* Current running process */
static process_t *current_process = NULL;
static uint32_t current_pid = KERNEL_PROCESS_ID;

/* Next available PID (TODO: use for sequential PID allocation) */
static uint32_t next_pid __attribute__((unused)) = INIT_PROCESS_ID;

/* Scheduling queues */
static process_t *ready_queue[PRIORITY_KERNEL + 1];
static process_t *current_queue __attribute__((unused)) = NULL;

/* Process statistics */
static process_stats_t process_statistics = {0};

/* Kernel process stack */
static uint8_t kernel_stack[PROCESS_STACK_SIZE] ALIGNED(PAGE_SIZE);

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Find an unused process slot
 */
static uint32_t find_free_pid(void) {
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == PROCESS_STATE_UNUSED) {
            return i;
        }
    }
    return MAX_PROCESSES; /* No free slots */
}

/**
 * Validate process parameters
 */
static status_t validate_process_params(const process_create_params_t *params) {
    if (!params) {
        return STATUS_INVALID_ARG;
    }

    if (!params->name || strlen(params->name) == 0) {
        return STATUS_INVALID_ARG;
    }

    if (strlen(params->name) >= PROCESS_NAME_MAX_LEN) {
        return STATUS_INVALID_ARG;
    }

    if (params->priority > PRIORITY_KERNEL) {
        return STATUS_INVALID_ARG;
    }

    if (params->stack_size == 0) {
        return STATUS_INVALID_ARG;
    }

    /* Validate parent process exists (unless it's the kernel) */
    if (params->parent_pid != KERNEL_PROCESS_ID) {
        if (!process_is_valid(params->parent_pid)) {
            return PROCESS_ERROR_INVALID_PARENT;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * Initialize process control block
 */
static status_t init_process_pcb(process_t *process, const process_create_params_t *params,
                                 uint32_t pid) {
    if (!process || !params) {
        return STATUS_INVALID_ARG;
    }

    /* Clear the PCB */
    memset(process, 0, sizeof(process_t));

    /* Basic information */
    process->pid = pid;
    process->parent_pid = params->parent_pid;
    strncpy_local(process->name, params->name, PROCESS_NAME_MAX_LEN - 1);
    process->name[PROCESS_NAME_MAX_LEN - 1] = '\0';
    process->type = params->type;
    process->state = PROCESS_STATE_CREATED;
    process->priority = params->priority;

    /* Execution context */
    process->rip = (uint64_t)params->entry_point;
    process->rsp = (uint64_t)params->stack_address + params->stack_size - sizeof(uint64_t);
    process->rbp = process->rsp;

    /* Memory management */
    process->memory_size = params->stack_size;
    process->stack_top = params->stack_address;
    process->stack_bottom = process->stack_top;

    /* Timing */
    process->creation_time = 0; /* TODO: Get system time */
    process->runtime_total = 0;
    process->runtime_last = 0;
    process->last_scheduled = 0;

    /* Quantum awareness */
    process->quantum.is_quantum_aware = params->is_quantum_aware;
    process->quantum.qubit_allocation = 0;
    process->quantum.quantum_runtime = 0;

    /* Initialize relationships */
    process->child_count = 0;
    process->has_exited = false;
    process->exit_code = 0;

    /* Set magic number */
    process->magic = PROCESS_MAGIC;

    return STATUS_SUCCESS;
}

/**
 * Add process to ready queue
 */
static void add_to_ready_queue(process_t *process) {
    if (!process || process->priority > PRIORITY_KERNEL) {
        return;
    }

    process->next = ready_queue[process->priority];
    process->prev = NULL;

    if (ready_queue[process->priority]) {
        ready_queue[process->priority]->prev = process;
    }

    ready_queue[process->priority] = process;
}

/**
 * Remove process from ready queue
 */
static void remove_from_ready_queue(process_t *process) {
    if (!process) {
        return;
    }

    if (process->prev) {
        process->prev->next = process->next;
    } else {
        /* Process is at head of queue */
        for (int priority = 0; priority <= PRIORITY_KERNEL; priority++) {
            if (ready_queue[priority] == process) {
                ready_queue[priority] = process->next;
                break;
            }
        }
    }

    if (process->next) {
        process->next->prev = process->prev;
    }

    process->next = NULL;
    process->prev = NULL;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize process management system
 */
status_t process_init(void) {
    if (process_table_initialized) {
        return STATUS_SUCCESS;
    }

    boot_log("Initializing process management system...");

    /* Clear process table */
    memset(&process_table, 0, sizeof(process_table));

    /* Initialize ready queues */
    for (int i = 0; i <= PRIORITY_KERNEL; i++) {
        ready_queue[i] = NULL;
    }

    /* Initialize statistics */
    memset(&process_statistics, 0, sizeof(process_statistics));

    /* Mark the table initialized before creating the bootstrap
     * processes -- process_create() refuses to run otherwise */
    process_table_initialized = true;

    /* Create kernel process */
    status_t result = process_init_kernel_process();
    if (result != STATUS_SUCCESS) {
        process_table_initialized = false;
        boot_panic("Failed to create kernel process");
        return result;
    }

    /* Create idle process */
    result = process_init_idle_process();
    if (result != STATUS_SUCCESS) {
        process_table_initialized = false;
        boot_panic("Failed to create idle process");
        return result;
    }

    current_process = &process_table[KERNEL_PROCESS_ID];
    current_pid = KERNEL_PROCESS_ID;

    boot_log("Process management system initialized");
    return STATUS_SUCCESS;
}

/**
 * Create a new process
 */
/* One-shot fault-injection seam (test-only): when armed, the next
 * process_create fails as if the process table were full, WITHOUT creating a
 * process. Lets the spawn-leak boot self-test deterministically drive
 * finalize_user_process's create-failure path (which must reclaim the
 * address space) instead of having to fill all MAX_PROCESSES slots. */
static int g_test_fail_next_create = 0;
void process_test_fail_next_create(void) {
    g_test_fail_next_create = 1;
}

status_t process_create(const process_create_params_t *params, process_t **process) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }
    if (g_test_fail_next_create) {
        g_test_fail_next_create = 0;
        return PROCESS_ERROR_TOO_MANY_PROCESSES;
    }

    status_t result = validate_process_params(params);
    if (result != STATUS_SUCCESS) {
        return result;
    }

    /* Find free PID */
    uint32_t pid = find_free_pid();
    if (pid >= MAX_PROCESSES) {
        return PROCESS_ERROR_TOO_MANY_PROCESSES;
    }

    /* New occupant of this slot — bump its generation so any stale holder
     * of the old (pid) can detect the recycling (process_get_generation). */
    process_generation[pid]++;

    /* Allocate memory for process stack */
    void *stack_memory = params->stack_address;
    bool heap_stack = false;
    if (!stack_memory) {
        if (params->type == PROCESS_TYPE_KERNEL) {
            stack_memory = kmalloc(params->stack_size);
            if (!stack_memory) {
                return PROCESS_ERROR_NO_MEMORY;
            }
            heap_stack = true; /* kfree'd in process_destroy */
        } else {
            /* TODO: Allocate from user memory pool */
            stack_memory = (void *)((uintptr_t)0x400000 + (pid * PROCESS_STACK_SIZE));
        }
    }

    /* Initialize PCB */
    result = init_process_pcb(&process_table[pid], params, pid);
    if (result != STATUS_SUCCESS) {
        return result;
    }

    /* Stack bookkeeping */
    process_table[pid].stack_bottom = stack_memory;
    process_table[pid].stack_top = (uint8_t *)stack_memory + params->stack_size;
    process_table[pid].owns_kernel_stack = heap_stack;

    /* Build a runnable register state so the scheduler can iretq into
     * the process: kernel threads run ring 0, user processes ring 3
     * (iretq drops privilege automatically from the frame selectors).
     * The kernel process (PID 0) is the boot flow itself — its
     * context is captured at first preemption. */
    if (params->entry_point && pid != KERNEL_PROCESS_ID &&
        (params->type == PROCESS_TYPE_KERNEL || params->type == PROCESS_TYPE_USER)) {
        bool is_user = (params->type == PROCESS_TYPE_USER);
        cpu_state_t *ctx = &process_table[pid].context;
        memset(ctx, 0, sizeof(*ctx));
        ctx->rip = (uint64_t)params->entry_point;
        ctx->cs = is_user ? USER_CS : GDT_KERNEL_CS;
        ctx->ss = is_user ? USER_DS : GDT_KERNEL_DS;
        ctx->eflags = 0x202; /* IF set — processes run preemptible */
        /* 16-align, then bias by 8 to mimic the post-call alignment the
         * SysV ABI guarantees at function entry */
        ctx->rsp = (((uint64_t)process_table[pid].stack_top) & ~0xFULL) - 8;
        process_table[pid].rsp = ctx->rsp;
        process_table[pid].context_valid = true;
    }

    /* Set up memory management */
    if (params->type != PROCESS_TYPE_KERNEL) {
        /* TODO: Create page directory for user process */
        process_table[pid].cr3 = 0; /* Physical address of page directory */
    } else {
        process_table[pid].cr3 = 0; /* Use kernel page directory */
    }

    /* Add to parent's children list */
    if (params->parent_pid != KERNEL_PROCESS_ID) {
        process_add_child(params->parent_pid, pid);
    }

    /* Initialize IPC for this process */
    ipc_result_t ipc_result = ipc_process_init(pid);
    if (ipc_result != IPC_SUCCESS) {
        /* Clean up */
        process_table[pid].state = PROCESS_STATE_UNUSED;
        return STATUS_ERROR;
    }

    /* Grant the process its root capability: full authority over
     * itself. Everything else must be derived or explicitly granted
     * (no ambient authority). */
    uint32_t root_cap = CAP_ID_INVALID;
    if (cap_create(pid, CAP_RESOURCE_PROCESS, pid, CAP_PERM_ALL, 0, &root_cap) == CAP_SUCCESS) {
        process_table[pid].capability_root = root_cap;
        process_table[pid].capability_count = 1;
    }

    /* Quantum-aware processes additionally get a capability for the
     * shared qubit pool */
    process_table[pid].quantum.quantum_cap = CAP_ID_INVALID;
    if (params->is_quantum_aware) {
        uint32_t qcap = CAP_ID_INVALID;
        if (quantum_grant(pid, CAP_QUANTUM | CAP_READ | CAP_WRITE | CAP_EXECUTE, &qcap) ==
            QUANTUM_SUCCESS) {
            process_table[pid].quantum.quantum_cap = qcap;
            process_table[pid].capability_count++;
        }
    }

    /* Update statistics */
    process_statistics.total_processes++;
    process_statistics.active_processes++;

    /* Set process state to ready */
    process_table[pid].state = PROCESS_STATE_READY;
    add_to_ready_queue(&process_table[pid]);

    *process = &process_table[pid];

    boot_log_v("Process created successfully");
    return STATUS_SUCCESS;
}

/**
 * Destroy a process
 */
status_t process_destroy(uint32_t pid) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }

    if (!process_is_valid(pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }

    process_t *process = &process_table[pid];

    /* Cannot destroy running process */
    if (process == current_process) {
        return PROCESS_ERROR_INVALID_STATE;
    }

    /* Remove from ready queue */
    remove_from_ready_queue(process);

    /* Release any VFS fds the process still held (epic #71): a dead
     * process can never issue another read/write, and leaving fds[].used
     * set would make an overlay file look permanently open, blocking its
     * unlink until the slot is recycled. */
    for (uint32_t fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        process->fds[fd].used = false;
        process->fds[fd].data = NULL;
        process->fds[fd].writable = false;
        process->fds[fd].ram_idx = -1;
    }

    /* Clean up IPC resources */
    ipc_process_cleanup(process->pid);

    /* Release any UDP sockets the process still held (epic #80): same
     * dead-PCB lesson as the fd sweep above — a dead process must not
     * pin socket slots. Marks slots CLOSING; the net thread retires
     * them at its next wake (never mid-copy). */
    net_udp_cleanup(process->pid);

    /* Abort a TCP connection the process still owned (epic #82): posts
     * an abort the net thread services (RST + retire). Guarded on
     * ownership, so an unrelated process's death never tears down qsh's
     * live connection. */
    net_tcp_cleanup(process->pid);

    /* Revoke every capability the process owns (cascades to anything
     * it granted onward) */
    cap_revoke_all_for_process(pid);
    /* Free the SURVIVING peers' IPC caps targeting this pid (epic #175,
     * generalized to ALL IPC caps by ADR-0023). INVARIANT: this must run
     * HERE, before the state=UNUSED store below — pids are first-fit slot
     * indices, and every pid-reuse path funnels through process_destroy
     * (both the idle-loop reaper and service_stop), so unlink-here is what
     * keeps a recycled pid from inheriting a live inbound channel minted for
     * its predecessor. Do not hoist this below the UNUSED store, and do not
     * move it to any single caller of process_destroy. Declared service
     * pairs come back on restart via ipc_peers[] (service.c start_slot). */
    cap_revoke_ipc_targets(pid);
    process->capability_root = CAP_ID_INVALID;
    process->capability_count = 0;

    /* Drop the ADR-0022 COM2-RX boost registration if this pid held it —
     * same pid-reuse funnel argument as the unlink above: a recycled pid
     * must never inherit the holder's scheduling boost. (No-op for every
     * process but the COM2 holder; start_slot re-registers on rebirth.) */
    scheduler_com2_holder_clear(pid);
    scheduler_console_reader_clear(pid); /* same rule for the console reader */

    /* Clear the intent manifest with the caps (epic #135). INVARIANT: this
     * must run before state=UNUSED below — every pid-reuse path funnels
     * through process_destroy, so clear-here + bind-before-schedule is what
     * keeps a recycled pid from inheriting its predecessor's intent. Do not
     * hoist this below the UNUSED store. */
    manifest_clear(pid);

    /* Sweep the QPU job broker (epic #148): free the dead owner's queued/
     * finished jobs and fail-close any job this pid was EXECUTING — job
     * slots are a per-process kernel resource like fds/sockets/caps, and a
     * dead process can never release its own. */
    qpu_on_process_destroy(pid);

    /* Free the private address space (page tables + user frames). Safe
     * here because process_destroy never runs on the current process,
     * so this is never the address space loaded in CR3. */
    if (process->virtual_address_space) {
        vmspace_destroy((uint64_t *)process->virtual_address_space);
        process->virtual_address_space = NULL;
        process->cr3 = 0;
    }

    /* Return a heap-allocated kernel-thread stack to the heap (kfree is
     * now a real free), so kernel-thread service restarts don't leak */
    if (process->owns_kernel_stack && process->stack_bottom) {
        kfree(process->stack_bottom);
        process->owns_kernel_stack = false;
        process->stack_bottom = NULL;
        process->stack_top = NULL;
        if (!boot_is_quiet()) {
            boot_log("freed kernel-thread stack; heap free bytes: ");
            early_console_write_hex(kheap_free_bytes());
        }
    }

    /* Remove from parent's children list */
    if (process->parent_pid != KERNEL_PROCESS_ID) {
        process_remove_child(process->parent_pid, pid);
    }

    /* Update statistics */
    if (process->state != PROCESS_STATE_ZOMBIE) {
        process_statistics.active_processes--;
    }

    /* Mark process as unused */
    process->state = PROCESS_STATE_UNUSED;
    process->magic = 0;

    boot_log_v("Process destroyed");
    return STATUS_SUCCESS;
}

/**
 * Reap terminated processes: free each dead process's private address
 * space (page tables + user frames) back to the physical allocator and
 * release its PCB slot. Runs from the kernel idle loop, so it never
 * touches the address space currently loaded in CR3.
 */
void process_reap(void) {
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = &process_table[pid];
        if (p->state != PROCESS_STATE_TERMINATED || p == current_process) {
            continue;
        }

        /* process_destroy frees the private address space back to the
         * pmm; the slot returns to UNUSED */
        process_destroy(pid);

        if (!boot_is_quiet()) {
            boot_log("reaped process; free frames: ");
            early_console_write_hex(pmm_get_free_frames());
        }
    }
}

/**
 * Exit a process
 */
status_t process_exit(uint32_t pid, int32_t exit_code) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }

    if (!process_is_valid(pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }

    process_t *process = &process_table[pid];

    /* Set exit information */
    process->exit_code = exit_code;
    process->has_exited = true;

    /* Change state to zombie */
    process_set_state(pid, PROCESS_STATE_ZOMBIE);

    /* Remove from ready queue */
    remove_from_ready_queue(process);

    /* Update statistics */
    process_statistics.active_processes--;
    process_statistics.zombie_processes++;

    (void)exit_code; /* Exit code stored in PCB */
    boot_log_v("Process exited");
    return STATUS_SUCCESS;
}

/**
 * Set process state
 */
status_t process_set_state(uint32_t pid, process_state_t new_state) {
    if (!process_is_valid(pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }

    process_t *process = &process_table[pid];
    process_state_t old_state = process->state;

    /* Handle queue transitions */
    if (old_state == PROCESS_STATE_READY) {
        remove_from_ready_queue(process);
    }

    process->state = new_state;

    if (new_state == PROCESS_STATE_READY) {
        add_to_ready_queue(process);
    }

    return STATUS_SUCCESS;
}

/**
 * Get process state
 */
process_state_t process_get_state(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return PROCESS_STATE_UNUSED;
    }

    return process_table[pid].state;
}

/**
 * Get process by PID
 */
process_t *process_get_by_pid(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return NULL;
    }

    return &process_table[pid];
}

/**
 * Get current process
 */
process_t *process_get_current(void) {
    return current_process;
}

/**
 * Get next ready process for scheduling
 */
process_t *process_get_next_ready(void) {
    /* Find highest priority ready process */
    for (int priority = PRIORITY_KERNEL; priority >= 0; priority--) {
        if (ready_queue[priority]) {
            return ready_queue[priority];
        }
    }

    /* No ready processes, return idle process */
    return &process_table[KERNEL_PROCESS_ID + 1]; /* Assume PID 1 is idle */
}

/**
 * Schedule next process
 */
status_t process_schedule_next(void) {
    if (!process_table_initialized) {
        return STATUS_ERROR;
    }

    process_t *next = process_get_next_ready();
    if (!next) {
        return STATUS_ERROR;
    }

    if (next == current_process) {
        return STATUS_SUCCESS; /* Already running */
    }

    return process_switch_to(next);
}

/**
 * Switch to specific process
 */
status_t process_switch_to(process_t *process) {
    if (!process) {
        return STATUS_INVALID_ARG;
    }

    if (!process_is_valid(process->pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }

    /* TODO: Implement actual context switch */
    /* This would involve:
     1. Save current process context
     2. Switch page tables (CR3)
     3. Restore new process context
     4. Update current_process pointer
    */

    process_t *old_process = current_process;
    current_process = process;
    current_pid = process->pid;

    /* Update statistics */
    process_statistics.context_switches++;

    /* Update timing. `now` is the PIT tick count (10 ms/tick at TIMER_DEFAULT_HZ);
     * this was a dead `0` stub, so runtime_last/runtime_total/last_scheduled were
     * always 0. Making it live is the ADR-0022 prerequisite-2 fairness/tail seam:
     * last_scheduled feeds max(now - last_scheduled) (the reschedule-gap tail) and
     * sched_picks the run-count spread. Measurement-only — no scheduling decision
     * depends on these; they only record when/how often a process was picked. */
    uint64_t now = timer_get_ticks();
    if (old_process) {
        old_process->runtime_last = now - old_process->last_scheduled;
        old_process->runtime_total += old_process->runtime_last;
    }
    process->last_scheduled = now;
    process->sched_picks++;

    return STATUS_SUCCESS;
}

/**
 * Check if process is valid
 */
bool process_is_valid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return false;
    }

    process_t *process = &process_table[pid];
    return (process->magic == PROCESS_MAGIC && process->state != PROCESS_STATE_UNUSED);
}

/**
 * Check if process is ready to run
 */
bool process_is_ready(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return false;
    }

    return process_table[pid].state == PROCESS_STATE_READY;
}

/**
 * Check if process is currently running
 */
bool process_is_running(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return false;
    }

    return process_table[pid].state == PROCESS_STATE_RUNNING;
}

/**
 * Check if process is terminated
 */
bool process_is_terminated(uint32_t pid) {
    if (!process_is_valid(pid)) {
        return false;
    }

    return process_table[pid].state == PROCESS_STATE_TERMINATED ||
           process_table[pid].state == PROCESS_STATE_ZOMBIE;
}

/**
 * Add child to parent
 */
status_t process_add_child(uint32_t parent_pid, uint32_t child_pid) {
    if (!process_is_valid(parent_pid) || !process_is_valid(child_pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }

    process_t *parent = &process_table[parent_pid];

    if (parent->child_count >= MAX_PROCESSES) {
        return STATUS_ERROR;
    }

    parent->children[parent->child_count++] = child_pid;
    return STATUS_SUCCESS;
}

/**
 * Remove child from parent
 */
status_t process_remove_child(uint32_t parent_pid, uint32_t child_pid) {
    if (!process_is_valid(parent_pid) || !process_is_valid(child_pid)) {
        return PROCESS_ERROR_INVALID_PID;
    }

    process_t *parent = &process_table[parent_pid];

    /* Find and remove child */
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child_pid) {
            /* Shift remaining children */
            for (uint32_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            return STATUS_SUCCESS;
        }
    }

    return PROCESS_ERROR_NOT_FOUND;
}

/**
 * Initialize kernel process
 */
status_t process_init_kernel_process(void) {
    process_create_params_t params = {.name = "kernel",
                                      .type = PROCESS_TYPE_KERNEL,
                                      .priority = PRIORITY_KERNEL,
                                      .parent_pid = KERNEL_PROCESS_ID,
                                      .entry_point =
                                          (void *)0xFFFFFFFF80000000UL, /* Kernel entry */
                                      .stack_address = &kernel_stack,
                                      .stack_size = PROCESS_STACK_SIZE,
                                      .is_quantum_aware = true};

    process_t *kernel_process;
    status_t result = process_create(&params, &kernel_process);
    if (result != STATUS_SUCCESS) {
        return result;
    }

    /* Set kernel process as running (via set_state so it is removed
     * from the ready queue) */
    process_set_state(kernel_process->pid, PROCESS_STATE_RUNNING);

    return STATUS_SUCCESS;
}

/**
 * Initialize idle process
 */
status_t process_init_idle_process(void) {
    process_create_params_t params = {.name = "idle",
                                      .type = PROCESS_TYPE_KERNEL,
                                      .priority = PRIORITY_IDLE,
                                      .parent_pid = KERNEL_PROCESS_ID,
                                      .entry_point = (void *)process_idle_task,
                                      .stack_address = NULL, /* process_create kmallocs a stack */
                                      .stack_size = PROCESS_STACK_SIZE,
                                      .is_quantum_aware = false};

    process_t *idle_process;
    return process_create(&params, &idle_process);
}

/**
 * Idle task - runs when no other processes are ready
 */
void process_idle_task(void) {
    while (1) {
        __asm__ volatile("hlt");
    }
}

/**
 * Get process statistics
 */
status_t process_get_stats(process_stats_t *stats) {
    if (!stats) {
        return STATUS_INVALID_ARG;
    }

    *stats = process_statistics;
    return STATUS_SUCCESS;
}

/**
 * Format the live process table as text (backs SYS_SYSINFO's PS op,
 * epic #62 phase 1). One line per non-UNUSED slot:
 *
 *   PS: <pid> <name> <STATE>\n
 *
 * The kernel formats so the user side needs no struct ABI, and this file
 * formats because it owns the table. Bounded by `max` (always
 * NUL-terminated); returns the byte count written, excluding the NUL.
 */
uint32_t process_get_generation(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return 0;
    }
    return process_generation[pid];
}

bool process_any_fd_references(const void *data) {
    if (!data) {
        return false;
    }
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = &process_table[pid];
        /* Only a LIVE process can still act on an fd. A terminated (or
         * unreaped) process's stale fds must not pin an overlay file —
         * counting them would refuse the unlink forever. */
        if (p->magic != PROCESS_MAGIC || !PROCESS_IS_ALIVE(p)) {
            continue;
        }
        for (uint32_t fd = 0; fd < PROCESS_MAX_FDS; fd++) {
            if (p->fds[fd].used && p->fds[fd].data == data) {
                return true;
            }
        }
    }
    return false;
}

size_t process_format_ps(char *buf, size_t max) {
    static const char *const state_names[] = {"UNUSED",  "CREATED",    "READY", "RUNNING",
                                              "BLOCKED", "TERMINATED", "ZOMBIE"};
    if (!buf || max == 0) {
        return 0;
    }

    size_t o = 0;
    for (uint32_t pid = 0; pid < MAX_PROCESSES; pid++) {
        process_t *p = &process_table[pid];
        if (p->state == PROCESS_STATE_UNUSED) {
            continue;
        }

        /* Build one row into a small local first so a row never lands
         * truncated in the output. */
        char row[64];
        size_t r = 0;
        const char *pfx = "PS: ";
        while (*pfx && r < sizeof(row) - 1) {
            row[r++] = *pfx++;
        }
        /* pid (decimal) */
        char t[10];
        int n = 0;
        uint32_t v = p->pid;
        if (v == 0) {
            t[n++] = '0';
        }
        while (v && n < (int)sizeof(t)) {
            t[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (n > 0 && r < sizeof(row) - 1) {
            row[r++] = t[--n];
        }
        if (r < sizeof(row) - 1) {
            row[r++] = ' ';
        }
        for (const char *s = p->name; *s && r < sizeof(row) - 1; s++) {
            row[r++] = *s;
        }
        if (r < sizeof(row) - 1) {
            row[r++] = ' ';
        }
        const char *st =
            (p->state < sizeof(state_names) / sizeof(state_names[0])) ? state_names[p->state] : "?";
        while (*st && r < sizeof(row) - 1) {
            row[r++] = *st++;
        }
        /* Console line ending — the consumer is a serial terminal. */
        if (r < sizeof(row) - 2) {
            row[r++] = '\r';
            row[r++] = '\n';
        }

        if (o + r + 1 > max) {
            break; /* out of room — stop at a whole row */
        }
        for (size_t i = 0; i < r; i++) {
            buf[o++] = row[i];
        }
    }
    buf[o] = '\0';
    return o;
}

/**
 * Dump process information for debugging
 */
void process_dump_info(uint32_t pid) {
    if (!process_is_valid(pid)) {
        boot_log("Invalid PID for dump");
        return;
    }

    process_t *process = &process_table[pid];

    boot_log("=== Process Info ===");
    boot_log(process->name);
    boot_log("State: ");
    early_console_write_hex(process->state);
    boot_log("Priority: ");
    early_console_write_hex(process->priority);
    boot_log("RIP: ");
    early_console_write_hex(process->rip);
}

/**
 * Dump all processes
 */
void process_dump_all(void) {
    boot_log("=== Process Table ===");
    boot_log("Total processes: ");
    early_console_write_hex(process_statistics.total_processes);
    boot_log("Active: ");
    early_console_write_hex(process_statistics.active_processes);

    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        if (process_is_valid(i)) {
            process_dump_info(i);
        }
    }
}
