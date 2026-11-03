/**
 * QuantumOS User-Space Services Framework Implementation (#6)
 *
 * Central service manager: registry, dependency-ordered startup,
 * capability grants, heartbeat health monitoring with automatic
 * restart. Services run as PROCESS_TYPE_SERVICE kernel threads until
 * user mode lands.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/service.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/capability.h>
#include <kernel/manifest.h>
#include <kernel/quantum.h>
#include <kernel/com2_uart.h>
#include <kernel/console.h>
#include <kernel/ata.h>
#include <kernel/net.h>
#include <kernel/qpu.h>
#include <kernel/field.h>
#include <kernel/syscall.h>
#include <kernel/interrupts.h>
#include <kernel/boot.h>

/* ============================================================================
 * Internal State
 * ============================================================================ */

typedef struct {
    service_info_t info;
    service_definition_t def;
    uint64_t last_heartbeat; /* timer ticks */
    uint8_t registered;
    uint8_t monitored;
    uint8_t starting; /* dependency-cycle guard */
} service_slot_t;

static service_slot_t services[MAX_SERVICES];
static uint8_t manager_initialized = 0;
static uint32_t monitor_restarts = 0;

/* ============================================================================
 * String helpers (freestanding)
 * ============================================================================ */

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void str_copy_n(char *dest, const char *src, size_t max) {
    size_t i = 0;
    while (i < max - 1 && src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

/* ============================================================================
 * Lookup helpers
 * ============================================================================ */

static service_slot_t *slot_by_id(uint32_t service_id) {
    if (service_id == 0 || service_id > MAX_SERVICES) {
        return NULL;
    }
    service_slot_t *slot = &services[service_id - 1];
    return slot->registered ? slot : NULL;
}

static service_slot_t *slot_by_name(const char *name) {
    for (uint32_t i = 0; i < MAX_SERVICES; i++) {
        if (services[i].registered && str_eq(services[i].info.name, name)) {
            return &services[i];
        }
    }
    return NULL;
}

static service_slot_t *slot_by_pid(uint32_t pid) {
    for (uint32_t i = 0; i < MAX_SERVICES; i++) {
        /* Generation guard (epic #144): a service whose process was TERMINATED
         * by the CPU-quota kill (or any external death) leaves its slot RUNNING
         * with a now-recyclable pid. Without the generation check, a later
         * process handed that same pid would mis-match this stale slot. Require
         * the live process's generation to equal the one recorded at spawn
         * (service_stop already relies on this pairing). */
        if (services[i].registered && services[i].info.pid == pid &&
            services[i].info.state == SERVICE_STATE_RUNNING &&
            process_get_generation(pid) == services[i].info.pid_generation) {
            return &services[i];
        }
    }
    return NULL;
}

bool service_pid_is_monitored(uint32_t pid) {
    service_slot_t *slot = slot_by_pid(pid);
    return slot && slot->monitored;
}

/* ============================================================================
 * Manager API
 * ============================================================================ */

svc_result_t service_manager_init(void) {
    memset(services, 0, sizeof(services));
    manager_initialized = 1;
    boot_log("Service manager initialized");
    return SVC_SUCCESS;
}

svc_result_t service_register(const service_definition_t *def, uint32_t *service_id_out) {
    if (!manager_initialized || !def || !def->name || (!def->entry && !def->user_elf_start)) {
        return SVC_ERROR_INVALID_ARG;
    }
    if (slot_by_name(def->name)) {
        return SVC_ERROR_ALREADY_RUNNING;
    }

    for (uint32_t i = 0; i < MAX_SERVICES; i++) {
        if (!services[i].registered) {
            service_slot_t *slot = &services[i];
            memset(slot, 0, sizeof(*slot));
            slot->registered = 1;
            slot->def = *def;
            slot->info.service_id = i + 1;
            str_copy_n(slot->info.name, def->name, SERVICE_NAME_MAX);
            slot->info.state = SERVICE_STATE_STOPPED;
            slot->info.max_restarts =
                def->max_restarts ? def->max_restarts : SERVICE_DEFAULT_MAX_RESTARTS;
            slot->info.cpu_limit = def->cpu_limit;
            slot->info.memory_limit = def->memory_limit;
            slot->info.quantum_limit = def->quantum_limit;
            for (uint32_t d = 0; d < SERVICE_MAX_DEPS && def->dependencies[d]; d++) {
                str_copy_n(slot->info.dependencies[d], def->dependencies[d], SERVICE_NAME_MAX);
            }
            if (service_id_out) {
                *service_id_out = slot->info.service_id;
            }
            return SVC_SUCCESS;
        }
    }
    return SVC_ERROR_NO_SPACE;
}

/* Interrupt save/restore (single CPU): start_slot must commit the whole
 * spawn -> grant caps -> record pid/generation sequence atomically vs the
 * scheduler, or a watchdog-reborn service (spawned from the IF=1 health
 * monitor thread) could be preempted and SCHEDULED before its console /
 * quantum / spawn caps and slot->info.pid exist — it would then run with
 * no authority (console EPERM -> shell suicide) and, if the reaper freed
 * the half-built PCB first, the caps would be minted for a dead, recyclable
 * pid and never revoked. cli across the commit closes that window. */
static inline uint64_t svc_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void svc_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

/* ---- Declarative IPC peer wiring (ADR-0023) ------------------------------ */

/* A peer slot is safe to mint against only if it is RUNNING **and** its pid
 * still names the process we spawned. state==RUNNING alone is a STALE value:
 * a service can be TERMINATED, freed by the idle reaper, and have its pid
 * recycled first-fit up to ~2 s before the health monitor marks the slot
 * CRASHED — cap_create validates neither owner nor resource liveness, so an
 * unguarded mint would hand an IPC cap to whatever process recycled the pid,
 * reopening the exact leak cap_revoke_ipc_targets closes. Same guard as
 * slot_by_pid and service_stop. */
static int slot_peer_live(const service_slot_t *s) {
    return s->info.state == SERVICE_STATE_RUNNING && s->info.pid != 0 &&
           process_is_valid(s->info.pid) &&
           process_get_generation(s->info.pid) == s->info.pid_generation;
}

/* Mint one declared pair between declarer pid `a` and peer pid `b`:
 * a->b with to_perms, b->a with from_perms (either may be 0 = skip).
 * TRANSACTIONAL: an IPC pair is only meaningful whole — a half-minted
 * qsh<->ghostd pair would send fine and time out on the reply, a flake worse
 * than a hard fail — so if the second direction's mint fails, the first is
 * revoked and the pair is absent, attributably. */
static void mint_ipc_pair(const char *declarer_name, uint32_t a, uint32_t b, uint32_t to_perms,
                          uint32_t from_perms) {
    uint32_t to_cap = CAP_ID_INVALID;
    if (to_perms) {
        if (cap_create(a, CAP_RESOURCE_IPC, b, to_perms, 0, &to_cap) != CAP_SUCCESS) {
            boot_log("service: IPC peer pair mint failed (pair skipped)");
            boot_log(declarer_name);
            return;
        }
    }
    if (from_perms) {
        uint32_t from_cap = CAP_ID_INVALID;
        if (cap_create(b, CAP_RESOURCE_IPC, a, from_perms, 0, &from_cap) != CAP_SUCCESS) {
            if (to_cap != CAP_ID_INVALID) {
                cap_revoke(to_cap, a); /* roll back the half already minted */
            }
            boot_log("service: IPC peer pair mint failed (pair rolled back)");
            boot_log(declarer_name);
        }
    }
}

static svc_result_t start_slot(service_slot_t *slot) {
    if (slot->info.state == SERVICE_STATE_RUNNING || slot->info.state == SERVICE_STATE_STARTING) {
        return SVC_SUCCESS;
    }
    if (slot->starting) {
        return SVC_ERROR_DEPENDENCY_CYCLE;
    }
    slot->starting = 1;

    /* Start dependencies first */
    for (uint32_t d = 0; d < SERVICE_MAX_DEPS && slot->def.dependencies[d]; d++) {
        service_slot_t *dep = slot_by_name(slot->def.dependencies[d]);
        if (!dep) {
            slot->starting = 0;
            return SVC_ERROR_DEPENDENCY;
        }
        svc_result_t r = start_slot(dep);
        if (r != SVC_SUCCESS) {
            slot->starting = 0;
            return r;
        }
    }

    slot->info.state = SERVICE_STATE_STARTING;

    /* Commit the spawn atomically vs the scheduler (see svc_irq_save). The
     * process_create inside spawn enqueues the process as READY immediately;
     * without this the timer could preempt us and run it before its caps and
     * pid are committed below. */
    uint64_t irqflags = svc_irq_save();

    uint32_t pid = 0;
    if (slot->def.user_elf_start) {
        /* Isolated ring-3 user-process service, loaded from its ELF */
        if (user_process_spawn_elf(slot->info.name, slot->def.user_elf_start,
                                   slot->def.user_elf_end, &pid) != STATUS_SUCCESS) {
            svc_irq_restore(irqflags);
            slot->info.state = SERVICE_STATE_CRASHED;
            slot->starting = 0;
            return SVC_ERROR_SPAWN_FAILED;
        }
    } else {
        /* In-kernel thread service */
        if (kernel_thread_create(slot->info.name, slot->def.entry, PRIORITY_NORMAL, &pid) !=
            STATUS_SUCCESS) {
            /* Every return after svc_irq_save must restore: leaving here
             * with IF=0 forever (no timer, no scheduler) turns a graceful
             * SVC_ERROR_SPAWN_FAILED into a system hang. (Pre-existing
             * leak found by the epic #135 design panel.) */
            svc_irq_restore(irqflags);
            slot->info.state = SERVICE_STATE_CRASHED;
            slot->starting = 0;
            return SVC_ERROR_SPAWN_FAILED;
        }
        /* Kernel-thread services are tagged SERVICE; user-process
         * services keep PROCESS_TYPE_USER (their ring-3 context is
         * already built) */
        process_t *proc = process_get_by_pid(pid);
        if (proc) {
            proc->type = PROCESS_TYPE_SERVICE;
        }
    }
    uint32_t svc_cap = CAP_ID_INVALID;
    if (cap_create(pid, CAP_RESOURCE_SERVICE, slot->info.service_id, CAP_READ | CAP_WRITE, 0,
                   &svc_cap) == CAP_SUCCESS) {
        slot->info.capabilities = svc_cap;
    }

    /* Re-mint declared resource caps on every start (first spawn and every
     * watchdog restart alike). The caps are owned by the fresh pid, so a
     * reborn service regains the same authority instead of losing it — e.g.
     * ghostd honestly recovers its qseed-derived noise source rather than
     * degrading to a plain PRNG forever after a restart. */
    if (slot->def.grant_quantum_pool) {
        uint32_t qcap = CAP_ID_INVALID;
        if (quantum_grant(pid, CAP_QUANTUM | CAP_READ, &qcap) != QUANTUM_SUCCESS) {
            boot_log("service: quantum-pool cap grant failed");
            boot_log(slot->info.name);
        }
    }
    if (slot->def.grant_com2) {
        uint32_t dcap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_COM2, CAP_DEVICE | CAP_READ | CAP_WRITE,
                       0, &dcap) != CAP_SUCCESS) {
            boot_log("service: COM2 device cap grant failed");
            boot_log(slot->info.name);
        } else {
            /* Register the holder for the ADR-0022 COM2-RX boost: (pid,
             * generation) so a recycled pid can never inherit the boost
             * (the ADR-0023 lesson), overwrite semantics so every watchdog
             * rebirth re-registers structurally — this line re-executes
             * inside the same cli window as the mint. The registration is
             * only an index hint; the boost site re-validates the live cap. */
            scheduler_com2_holder_set(pid, process_get_generation(pid));
        }
    }
    if (slot->def.grant_console) {
        uint32_t ccap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_CONSOLE,
                       CAP_DEVICE | CAP_READ | CAP_WRITE, 0, &ccap) != CAP_SUCCESS) {
            boot_log("service: console device cap grant failed");
            boot_log(slot->info.name);
        }
    }
    /* QPU authority (epic #148): the two rights are DISJOINT by construction —
     * grant_qpu_submit mints WRITE only, grant_qpu_execute mints READ|EXECUTE
     * only (never WRITE). A submitter that also held EXECUTE could forge its
     * own results; an executor that also held WRITE could submit on quota it
     * does not own. Keep the masks separate. */
    if (slot->def.grant_qpu_submit) {
        uint32_t qcap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_QPU, CAP_DEVICE | CAP_WRITE, 0, &qcap) !=
            CAP_SUCCESS) {
            boot_log("service: QPU submit cap grant failed");
            boot_log(slot->info.name);
        }
    }
    if (slot->def.grant_qpu_execute) {
        uint32_t qcap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_QPU, CAP_DEVICE | CAP_READ | CAP_EXECUTE,
                       0, &qcap) != CAP_SUCCESS) {
            boot_log("service: QPU execute cap grant failed");
            boot_log(slot->info.name);
        }
    }
    if (slot->def.grant_spawn) {
        uint32_t scap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_PROCESS, SPAWN_RESOURCE_ID, CAP_EXECUTE, 0, &scap) !=
            CAP_SUCCESS) {
            boot_log("service: spawn cap grant failed");
            boot_log(slot->info.name);
        }
    }
    if (slot->def.grant_fswrite) {
        uint32_t fcap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_DISK, CAP_DEVICE | CAP_READ | CAP_WRITE,
                       0, &fcap) != CAP_SUCCESS) {
            boot_log("service: fs-write cap grant failed");
            boot_log(slot->info.name);
        }
    }
    if (slot->def.grant_net) {
        uint32_t ncap = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_DEVICE, DEVICE_ID_NET, CAP_DEVICE | CAP_READ, 0, &ncap) !=
            CAP_SUCCESS) {
            boot_log("service: network cap grant failed");
            boot_log(slot->info.name);
        }
    }
    /* Normalized field span (epic #177): 0/1 = the classic single region, so
     * every zero-initialized existing def keeps its exact behavior. The SAME
     * normalized range drives the cap-mint/scrub loop here AND the manifest
     * rows below — intent and authority must never diverge. */
    uint32_t field_span = slot->def.field_region_span ? slot->def.field_region_span : 1u;
    if (slot->def.grant_field) {
        if ((uint32_t)slot->def.field_region + field_span > FIELD_REGION_COUNT) {
            /* Fail the WHOLE grant closed (no caps, no rows): cap_create never
             * validates FIELD resource ids and field_region_scrub silently
             * no-ops out of range, so a partial span would burn cap slots on
             * nonexistent regions with no visible symptom. */
            boot_log("service: field region span out of range — caps NOT granted");
            boot_log(slot->info.name);
            field_span = 0; /* the manifest loop below appends no field rows */
        } else {
            for (uint32_t r = slot->def.field_region; r < slot->def.field_region + field_span;
                 r++) {
                /* Scrub BEFORE minting — for EVERY region in the span: a
                 * reborn or successor service must never inherit any of its
                 * regions' previous contents (epic #95). ONE exception (epic
                 * #96): a service that opted in via field_inherit may claim
                 * DISK-restored content at the first grant of a boot,
                 * consulted and consumed PER-REGION — the inherit mark is
                 * consumed only after the cap actually minted (a failed mint
                 * leaves it for the retry); every later grant scrubs. */
                int inherit = slot->def.field_inherit && field_region_inherit_peek(r);
                if (!inherit) {
                    field_region_scrub(r);
                }
                uint32_t fldcap = CAP_ID_INVALID;
                /* A delegable field cap also carries CAP_GRANT so this citizen
                 * may cap_derive a narrowed slice of the region to a sub-agent
                 * (epic #137). Only the delegator sets grant_field_delegable —
                 * one auditable delegator, now over its whole span. */
                uint32_t fldperms = CAP_READ | CAP_WRITE;
                if (slot->def.grant_field_delegable) {
                    fldperms |= CAP_GRANT;
                }
                if (cap_create(pid, CAP_RESOURCE_FIELD, r, fldperms, 0, &fldcap) != CAP_SUCCESS) {
                    /* Attributable mid-span failure: name the region. */
                    char fline[48] = "service: field cap grant failed (region 0)";
                    fline[41] = (char)('0' + r);
                    boot_log(fline);
                    boot_log(slot->info.name);
                } else if (inherit) {
                    field_region_inherit_consume(r);
                    /* The region digit keeps the audit line unambiguous;
                     * only single-digit region counts exist. */
                    char iline[64] = "service: field region 0 inherited from disk (scrub skipped)";
                    iline[22] = (char)('0' + r);
                    boot_log(iline);
                }
            }
        }
    }

    /* Bind the intent manifest from the SAME grant flags that minted the
     * caps above, inside the SAME cli window (epic #135): intent and
     * authority are never half-committed, and a watchdog rebirth re-binds
     * exactly like it re-mints. This OVERWRITES the restrictive default
     * finalize_user_process bound (manifest_bind is last-write-wins). */
    {
        manifest_t man;
        memset(&man, 0, sizeof(man));
        man.bound = 1;
        man.spawn_max = slot->def.spawn_max;
        man.spawn_channel = slot->def.grant_spawn_channel; /* spawn-time IPC pairs (epic #175) */
        man.cpu_limit = slot->def.cpu_limit; /* CPU quota (epic #144); 0 = unlimited */
        man.qsub_max = slot->def.qsub_max;   /* QPU submit quota (epic #148); 0 = unlimited */
        if (slot->def.grant_quantum_pool) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_QUANTUM;
            man.entries[man.entry_count].resource_id = QUANTUM_POOL_RESOURCE_ID;
            man.entries[man.entry_count].permissions = CAP_QUANTUM | CAP_READ;
            man.entry_count++;
        }
        if (slot->def.grant_com2) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_DEVICE;
            man.entries[man.entry_count].resource_id = DEVICE_ID_COM2;
            man.entries[man.entry_count].permissions = CAP_DEVICE | CAP_READ | CAP_WRITE;
            man.entry_count++;
        }
        if (slot->def.grant_console) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_DEVICE;
            man.entries[man.entry_count].resource_id = DEVICE_ID_CONSOLE;
            man.entries[man.entry_count].permissions = CAP_DEVICE | CAP_READ | CAP_WRITE;
            man.entry_count++;
        }
        if (slot->def.grant_spawn) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_PROCESS;
            man.entries[man.entry_count].resource_id = SPAWN_RESOURCE_ID;
            man.entries[man.entry_count].permissions = CAP_EXECUTE;
            man.entry_count++;
        }
        if (slot->def.grant_fswrite) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_DEVICE;
            man.entries[man.entry_count].resource_id = DEVICE_ID_DISK;
            man.entries[man.entry_count].permissions = CAP_DEVICE | CAP_READ | CAP_WRITE;
            man.entry_count++;
        }
        if (slot->def.grant_net) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_DEVICE;
            man.entries[man.entry_count].resource_id = DEVICE_ID_NET;
            man.entries[man.entry_count].permissions = CAP_DEVICE | CAP_READ;
            man.entry_count++;
        }
        /* Field rows: one per region in the SAME normalized span the cap
         * loop minted (field_span is 0 when the span failed bounds — no rows
         * then either). Guarded per-append like the QPU rows below: a span
         * makes the total row count def-dependent, so an unguarded append
         * could write past entries[] into start_slot's stack frame — the OOB
         * the manifest_bind clamp cannot undo. Fail closed (drop the row). */
        if (slot->def.grant_field) {
            for (uint32_t r = slot->def.field_region;
                 r < slot->def.field_region + field_span && r < FIELD_REGION_COUNT; r++) {
                if (man.entry_count >= MANIFEST_MAX_ENTRIES) {
                    boot_log("service: manifest full — field row dropped (fail closed)");
                    boot_log(slot->info.name);
                    break;
                }
                man.entries[man.entry_count].resource_type = CAP_RESOURCE_FIELD;
                man.entries[man.entry_count].resource_id = r;
                man.entries[man.entry_count].permissions =
                    CAP_READ | CAP_WRITE | (slot->def.grant_field_delegable ? CAP_GRANT : 0);
                man.entry_count++;
            }
        }
        /* The QPU rows are last; guard the append against MANIFEST_MAX_ENTRIES
         * so a future service that adds grant flags past the current 8-row max
         * fails closed (drops the row) instead of writing past entries[] — the
         * OOB write manifest_bind's post-hoc clamp cannot undo. */
        if (slot->def.grant_qpu_submit && man.entry_count < MANIFEST_MAX_ENTRIES) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_DEVICE;
            man.entries[man.entry_count].resource_id = DEVICE_ID_QPU;
            man.entries[man.entry_count].permissions = CAP_DEVICE | CAP_WRITE;
            man.entry_count++;
        }
        if (slot->def.grant_qpu_execute && man.entry_count < MANIFEST_MAX_ENTRIES) {
            man.entries[man.entry_count].resource_type = CAP_RESOURCE_DEVICE;
            man.entries[man.entry_count].resource_id = DEVICE_ID_QPU;
            man.entries[man.entry_count].permissions = CAP_DEVICE | CAP_READ | CAP_EXECUTE;
            man.entry_count++;
        }
        manifest_bind(pid, &man);
    }

    /* Declarative IPC peer wiring (ADR-0023 Part 2), inside the SAME cli
     * window as the grant_* mints and the manifest bind. On a single CPU with
     * IF=0 the health monitor and reaper cannot run, so services[] is a
     * frozen snapshot during the scan — but a frozen snapshot can still be
     * STALE, hence the slot_peer_live generation guard on every mint.
     *
     * Pass 1 MUST run entirely before Pass 2, with no cap frees in between:
     * a service holding several outbound IPC caps (swarm-svc: ghostd + the
     * fieldsyncd key cap) relies on its declared (Pass-1) primary peer
     * landing at the lowest first-fit cap slot, where untargeted send_msg's
     * first-match finds it. */

    /* Pass 1: for each of MY declared peers currently running, mint the pair
     * as declared — covers first boot AND my own rebirth. A peer that is not
     * up (or stale) is skipped; its own start's Pass 2 wires us then. */
    for (uint32_t p = 0; p < SERVICE_MAX_IPC_PEERS && slot->def.ipc_peers[p].peer; p++) {
        service_slot_t *other = slot_by_name(slot->def.ipc_peers[p].peer);
        if (!other || other == slot || !slot_peer_live(other)) {
            continue;
        }
        mint_ipc_pair(slot->info.name, pid, other->info.pid, slot->def.ipc_peers[p].to_perms,
                      slot->def.ipc_peers[p].from_perms);
    }

    /* Pass 2: for each running service whose declaration names ME, mint its
     * declared pair against my fresh pid — covers MY rebirth when the OTHER
     * side owns the declaration, and boot-order gaps (fieldsyncd declares the
     * swarm-svc key cap before swarm-svc exists; the pair mints here when
     * swarm-svc starts). Single-sided declaration means a pair mints in
     * exactly one pass per start — never twice (#176 singleton invariant). */
    for (uint32_t i = 0; i < MAX_SERVICES; i++) {
        service_slot_t *other = &services[i];
        if (!other->registered || other == slot || !slot_peer_live(other)) {
            continue;
        }
        for (uint32_t p = 0; p < SERVICE_MAX_IPC_PEERS && other->def.ipc_peers[p].peer; p++) {
            if (!str_eq(other->def.ipc_peers[p].peer, slot->info.name)) {
                continue;
            }
            mint_ipc_pair(other->info.name, other->info.pid, pid, other->def.ipc_peers[p].to_perms,
                          other->def.ipc_peers[p].from_perms);
        }
    }

    slot->info.pid = pid;
    slot->info.pid_generation = process_get_generation(pid);
    slot->info.start_time = timer_get_ticks();
    slot->last_heartbeat = timer_get_ticks();
    slot->info.state = SERVICE_STATE_RUNNING;
    slot->starting = 0;

    /* All caps minted, manifest bound, pid/generation recorded — the reborn
     * process may now be scheduled with full authority. */
    svc_irq_restore(irqflags);

    if (!boot_is_quiet()) {
        boot_log("service started:");
        boot_log(slot->info.name);
    }
    return SVC_SUCCESS;
}

svc_result_t service_start(const char *name, const char *args) {
    (void)args; /* kernel-thread services take no argv yet */
    if (!manager_initialized || !name) {
        return SVC_ERROR_INVALID_ARG;
    }
    service_slot_t *slot = slot_by_name(name);
    if (!slot) {
        return SVC_ERROR_NOT_FOUND;
    }
    return start_slot(slot);
}

svc_result_t service_stop(uint32_t service_id) {
    service_slot_t *slot = slot_by_id(service_id);
    if (!slot) {
        return SVC_ERROR_NOT_FOUND;
    }
    if (slot->info.state != SERVICE_STATE_RUNNING && slot->info.state != SERVICE_STATE_CRASHED) {
        return SVC_SUCCESS;
    }

    slot->info.state = SERVICE_STATE_STOPPING;

    /* Retire the service process: block it out of the scheduler's rotation,
     * then tear it down (revokes its capabilities too). But ONLY if the pid
     * still names the process we spawned — a service can reach TERMINATED and
     * be freed by the idle reaper up to ~2 s before the watchdog trips, and
     * pids are recycled first-fit, so an unguarded destroy here would remove
     * an innocent process that landed in the freed slot. The generation check
     * makes this a no-op when the slot has been recycled (the process is
     * already gone, which is what we wanted anyway).
     *
     * The whole guard->retire sequence runs cli'd, mirroring start_slot's
     * spawn-half bracket. service_stop is reached from the IF=1 health-monitor
     * thread, so without this: (1) the guard is a TOCTOU — a timer IRQ between
     * the generation check and process_destroy lets the idle reaper free the
     * slot and a cli'd SYS_SPAWN recycle it, so we would BLOCK+destroy an
     * innocent recycled process; and (2) process_set_state's remove_from_ready
     * _queue and process_destroy's kfree(stack)/PCB-recycle race the timer
     * scheduler mid-unlink on the un-cli'd ready_queue[], a use-after-free.
     * cli across the commit closes both windows. */
    uint64_t irqflags = svc_irq_save();
    if (slot->info.pid != 0 && process_is_valid(slot->info.pid) &&
        process_get_generation(slot->info.pid) == slot->info.pid_generation) {
        process_set_state(slot->info.pid, PROCESS_STATE_BLOCKED);
        process_destroy(slot->info.pid);
    }
    slot->info.pid = 0;
    svc_irq_restore(irqflags);

    slot->info.capabilities = CAP_ID_INVALID;
    slot->info.state = SERVICE_STATE_STOPPED;
    return SVC_SUCCESS;
}

svc_result_t service_restart(uint32_t service_id) {
    service_slot_t *slot = slot_by_id(service_id);
    if (!slot) {
        return SVC_ERROR_NOT_FOUND;
    }
    if (slot->info.restart_count >= slot->info.max_restarts) {
        /* Budget exhausted: reclaim the confirmed-bad process instead of leaving
         * it behind. A CRASHED-but-DEAD service is already TERMINATED and gets
         * reaped, but a HUNG one (alive yet not heart-beating — exactly what the
         * watchdog exists to catch) would otherwise stay RUNNING forever: never
         * re-scanned (state != RUNNING), never reaped (not TERMINATED), never
         * stopped, holding its PCB slot and a scheduler slice. service_stop is
         * generation-guarded and idempotent, so it destroys a still-live process
         * or is a no-op if the pid already died and was reaped. */
        service_stop(service_id);
        return SVC_ERROR_RESTART_LIMIT;
    }

    service_stop(service_id);
    slot->info.restart_count++;
    return start_slot(slot);
}

svc_result_t service_status(uint32_t service_id, service_info_t *info) {
    service_slot_t *slot = slot_by_id(service_id);
    if (!slot || !info) {
        return SVC_ERROR_NOT_FOUND;
    }
    *info = slot->info;
    return SVC_SUCCESS;
}

svc_result_t service_list(service_info_t *out, size_t max_count, uint32_t *count) {
    if (!out || !count) {
        return SVC_ERROR_INVALID_ARG;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_SERVICES && n < max_count; i++) {
        if (services[i].registered) {
            out[n++] = services[i].info;
        }
    }
    *count = n;
    return SVC_SUCCESS;
}

svc_result_t service_monitor(uint32_t service_id, bool enable) {
    service_slot_t *slot = slot_by_id(service_id);
    if (!slot) {
        return SVC_ERROR_NOT_FOUND;
    }
    /* A cpu_limit service must NOT be monitored (epic #144): the CPU-quota kill
     * terminates it, then the watchdog would respawn it, reset cpu_ticks, and
     * re-kill it every ~cpu_limit ticks — a restart-bounded churn. Refuse the
     * combination loudly (mirrors SYS_CAP_DERIVE refusing a monitored target). */
    if (enable && slot->def.cpu_limit != 0) {
        boot_log("service: refusing to monitor a cpu_limit service (would respawn-kill churn)");
        boot_log(slot->info.name);
        return SVC_ERROR_INVALID_ARG;
    }
    slot->monitored = enable ? 1 : 0;
    return SVC_SUCCESS;
}

void service_heartbeat(void) {
    process_t *self = process_get_current();
    if (!self) {
        return;
    }
    service_slot_t *slot = slot_by_pid(self->pid);
    if (slot) {
        slot->last_heartbeat = timer_get_ticks();
    }
}

int32_t service_current_restart_count(void) {
    process_t *self = process_get_current();
    if (!self) {
        return -1;
    }
    service_slot_t *slot = slot_by_pid(self->pid);
    return slot ? (int32_t)slot->info.restart_count : -1;
}

svc_result_t service_find(const char *name, uint32_t *service_id_out) {
    service_slot_t *slot = name ? slot_by_name(name) : NULL;
    if (!slot || !service_id_out) {
        return SVC_ERROR_NOT_FOUND;
    }
    *service_id_out = slot->info.service_id;
    return SVC_SUCCESS;
}

/* ============================================================================
 * Health monitor (runs as its own kernel thread)
 * ============================================================================ */

static void health_monitor_scan(void) {
    for (uint32_t i = 0; i < MAX_SERVICES; i++) {
        service_slot_t *slot = &services[i];
        if (!slot->registered || !slot->monitored || slot->info.state != SERVICE_STATE_RUNNING) {
            continue;
        }
        /* Read the clock per slot (#370). Restarting an earlier slot in this
         * scan takes ticks (destroy, create, console lines), and live services
         * keep heartbeating meanwhile; against a `now` read once at the top,
         * their newer last_heartbeat made the unsigned difference wrap, and
         * every live service after the restarted one was killed in turn. The
         * explicit order check keeps that impossible even if this moves. */
        uint64_t now = timer_get_ticks();
        uint64_t last = slot->last_heartbeat;
        if (now > last && now - last > SERVICE_HEARTBEAT_TIMEOUT) {
            slot->info.state = SERVICE_STATE_CRASHED;
            if (!boot_is_quiet()) {
                boot_log("health monitor: service unresponsive:");
                boot_log(slot->info.name);
            }

            if (service_restart(slot->info.service_id) == SVC_SUCCESS) {
                monitor_restarts++;
                if (!boot_is_quiet()) {
                    boot_log("health monitor: service restarted:");
                    boot_log(slot->info.name);
                }
            } else {
                if (!boot_is_quiet()) {
                    boot_log("health monitor: restart limit reached:");
                    boot_log(slot->info.name);
                }
            }
        }
    }
}

static void health_monitor_thread(void) {
    boot_log("service health monitor: online");
    uint64_t last_scan = timer_get_ticks();
    while (1) {
        __asm__ volatile("hlt");
        uint64_t now = timer_get_ticks();
        if (now - last_scan >= SERVICE_MONITOR_INTERVAL) {
            last_scan = now;
            health_monitor_scan();
        }
    }
}

/* ============================================================================
 * Built-in demo services
 * ============================================================================ */

static void entropy_manager_service(void) {
    boot_log("svc entropy-manager: online");
    while (1) {
        __asm__ volatile("hlt");
        service_heartbeat();
    }
}

static void quantum_scheduler_service(void) {
    boot_log("svc quantum-scheduler: online");
    while (1) {
        __asm__ volatile("hlt");
        service_heartbeat();
    }
}

static void device_manager_service(void) {
    boot_log("svc device-manager: online");
    while (1) {
        __asm__ volatile("hlt");
        service_heartbeat();
    }
}

/* Deliberately stops heartbeating after ~1.5 s to demonstrate crash
 * detection and automatic restart */
static void flaky_demo_service(void) {
    boot_log_v("svc flaky-demo: online");
    uint64_t born = timer_get_ticks();
    while (1) {
        __asm__ volatile("hlt");
        if (timer_get_ticks() - born < 150) {
            service_heartbeat();
        }
        /* after that: alive but silent — the watchdog should trip */
    }
}

/* ============================================================================
 * Boot wiring + self-test
 * ============================================================================ */

#define SV_ASSERT(cond, msg)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            boot_log("service-selftest FAIL: " msg);                                               \
            return SVC_ERROR_INVALID_ARG;                                                          \
        }                                                                                          \
    } while (0)

svc_result_t service_selftest(void) {
    /* Register built-ins: quantum-scheduler depends on entropy-manager,
     * exercising dependency-ordered startup */
    service_definition_t defs[] = {
        {.name = "entropy-manager",
         .entry = entropy_manager_service,
         .dependencies = {NULL},
         .max_restarts = 3},
        {.name = "quantum-scheduler",
         .entry = quantum_scheduler_service,
         .dependencies = {"entropy-manager", NULL},
         .max_restarts = 3,
         .quantum_limit = 8},
        {.name = "device-manager",
         .entry = device_manager_service,
         .dependencies = {NULL},
         .max_restarts = 3},
        {.name = "flaky-demo",
         .entry = flaky_demo_service,
         .dependencies = {NULL},
         .max_restarts = 2},
    };

    for (uint32_t i = 0; i < ARRAY_SIZE(defs); i++) {
        SV_ASSERT(service_register(&defs[i], NULL) == SVC_SUCCESS, "register");
    }

    /* Unknown service refused */
    SV_ASSERT(service_start("no-such-service", NULL) == SVC_ERROR_NOT_FOUND,
              "unknown service refused");

    /* Starting quantum-scheduler must start entropy-manager first */
    SV_ASSERT(service_start("quantum-scheduler", NULL) == SVC_SUCCESS, "dependency-ordered start");
    uint32_t dep_id = 0;
    SV_ASSERT(service_find("entropy-manager", &dep_id) == SVC_SUCCESS, "find dep");
    service_info_t info;
    SV_ASSERT(service_status(dep_id, &info) == SVC_SUCCESS, "dep status");
    SV_ASSERT(info.state == SERVICE_STATE_RUNNING, "dependency auto-started");
    SV_ASSERT(info.capabilities != CAP_ID_INVALID, "service capability granted");

    SV_ASSERT(service_start("device-manager", NULL) == SVC_SUCCESS, "start standalone");
    SV_ASSERT(service_start("flaky-demo", NULL) == SVC_SUCCESS, "start flaky demo");

    /* Enable the watchdog for everything running */
    service_info_t all[MAX_SERVICES];
    uint32_t count = 0;
    SV_ASSERT(service_list(all, MAX_SERVICES, &count) == SVC_SUCCESS, "list");
    SV_ASSERT(count == 4, "list count");
    for (uint32_t i = 0; i < count; i++) {
        service_monitor(all[i].service_id, true);
    }

    /* Spawn the health monitor thread */
    SV_ASSERT(kernel_thread_create("svc-monitor", health_monitor_thread, PRIORITY_HIGH, NULL) ==
                  STATUS_SUCCESS,
              "monitor thread");

    /* Watchdog give-up must RECLAIM the process, not leak a hung one (bug-hunt:
     * a restart-budget-exhausted service was left RUNNING forever — never
     * re-scanned, never reaped, never stopped). max_restarts=1 (service_register
     * replaces a 0 with the default, so 1 is the smallest usable budget): the
     * first restart consumes it, the SECOND hits the limit and must stop the
     * process. No watchdog-timing dependence. Runs pre-scheduler, so the started
     * thread never executes — this exercises only the FSM give-up edge. */
    {
        service_definition_t giveup = {.name = "giveup-test",
                                       .entry = device_manager_service,
                                       .dependencies = {NULL},
                                       .max_restarts = 1};
        uint32_t gid = 0;
        SV_ASSERT(service_register(&giveup, &gid) == SVC_SUCCESS, "register giveup");
        SV_ASSERT(service_start("giveup-test", NULL) == SVC_SUCCESS, "start giveup");
        /* First restart consumes the budget (count 0 -> 1) and respawns. */
        SV_ASSERT(service_restart(gid) == SVC_SUCCESS, "giveup first restart");
        service_info_t gi;
        SV_ASSERT(service_status(gid, &gi) == SVC_SUCCESS, "giveup status");
        uint32_t gpid = gi.pid;
        SV_ASSERT(gpid != 0 && process_is_valid(gpid), "giveup process live before give-up");
        /* Second restart: count 1 >= max 1 -> give up, and MUST stop the process. */
        SV_ASSERT(service_restart(gid) == SVC_ERROR_RESTART_LIMIT, "giveup hits restart limit");
        SV_ASSERT(!process_is_valid(gpid), "giveup process reclaimed on give-up");
        boot_log("SVCGIVEUP: restart budget exhausted reclaims the process");
    }

    boot_log("service self-test: PASS");
    return SVC_SUCCESS;
}
