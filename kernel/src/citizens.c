/**
 * QuantumOS citizen roster — the ring-3 boot wiring (extracted from syscall.c).
 *
 * Every user_*_init below registers, starts, and capability-wires ONE ring-3
 * citizen (or demo pair) of the boot roster; user_init() is the single
 * dispatcher main.c calls after the syscall layer is up. This file owns the
 * embedded ELF blob externs and NOTHING syscall-shaped: the syscall handlers,
 * the ELF loader, and the spawn path stay in syscall.c (citizens reach them
 * only through the public user_process_spawn_elf/service_* APIs) — so the
 * WIRING of authority (who gets which grants at boot) reads in one place,
 * separate from the MECHANISM that enforces it.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/capability.h>
#include <kernel/field.h>
#include <kernel/manifest.h>
#include <kernel/process.h>
#include <kernel/quantum.h>
#include <kernel/service.h>
#include <kernel/syscall.h>

/* Hand-written blobs for the isolation + fault-containment demos (user_init). */
extern const uint8_t user_canary_start[], user_canary_end[];
extern const uint8_t user_rogue_start[], user_rogue_end[];

/* Embedded ELF user programs (build/x86_64/user ELFs via objcopy) */
extern const uint8_t _binary_init_elf_start[], _binary_init_elf_end[];
extern const uint8_t _binary_echo_elf_start[], _binary_echo_elf_end[];
extern const uint8_t _binary_client_elf_start[], _binary_client_elf_end[];
extern const uint8_t _binary_hbsvc_elf_start[], _binary_hbsvc_elf_end[];
extern const uint8_t _binary_ghostd_elf_start[], _binary_ghostd_elf_end[];
extern const uint8_t _binary_ghost_test_elf_start[], _binary_ghost_test_elf_end[];
extern const uint8_t _binary_paradoxd_elf_start[], _binary_paradoxd_elf_end[];
extern const uint8_t _binary_paradox_test_elf_start[], _binary_paradox_test_elf_end[];
extern const uint8_t _binary_swarm_svc_elf_start[], _binary_swarm_svc_elf_end[];
extern const uint8_t _binary_qsh_elf_start[], _binary_qsh_elf_end[];
extern const uint8_t _binary_quantumd_elf_start[], _binary_quantumd_elf_end[];
extern const uint8_t _binary_kannakad_elf_start[], _binary_kannakad_elf_end[];
extern const uint8_t _binary_fieldsyncd_elf_start[], _binary_fieldsyncd_elf_end[];
extern const uint8_t _binary_httpd_elf_start[], _binary_httpd_elf_end[];
extern const uint8_t _binary_modbusd_elf_start[], _binary_modbusd_elf_end[];
extern const uint8_t _binary_quota_test_elf_start[], _binary_quota_test_elf_end[];
extern const uint8_t _binary_delegation_test_elf_start[], _binary_delegation_test_elf_end[];
extern const uint8_t _binary_subagentd_elf_start[], _binary_subagentd_elf_end[];
extern const uint8_t _binary_cpu_hog_elf_start[], _binary_cpu_hog_elf_end[];
extern const uint8_t _binary_qsv_elf_start[], _binary_qsv_elf_end[];
extern const uint8_t _binary_qpud_elf_start[], _binary_qpud_elf_end[];
extern const uint8_t _binary_qpu_test_elf_start[], _binary_qpu_test_elf_end[];
extern const uint8_t _binary_agentd_elf_start[], _binary_agentd_elf_end[];
/* agentsub is NOT kernel-embedded: it ships on the initrd as /bin/agentsub and
 * agentd spawns its own society from there (epic #175). */

void user_ipc_demo_init(void);
void user_ghost_demo_init(void);
void user_paradox_demo_init(uint32_t ghostd_pid);
void user_swarm_demo_init(uint32_t ghostd_pid);
void user_shell_init(uint32_t ghostd_pid);
void user_quantum_demo_init(void);
void user_fieldsync_demo_init(uint32_t ghostd_pid);
void user_httpd_init(void);
void user_modbusd_init(void);
void user_kannaka_demo_init(void);
void user_quota_test_init(void);
void user_delegation_demo_init(void);
void user_cpu_hog_init(void);
void user_qsv_init(void);
void user_qpud_init(void);
void user_qpu_test_init(void);
void user_agent_demo_init(void);

void user_init(void) {
    boot_log("Per-process address spaces enabled (private CR3 per user proc)");

    uint32_t pid = 0;

    /* A real compiled-C program, loaded from its embedded ELF image */
    if (user_process_spawn_elf("init", _binary_init_elf_start, _binary_init_elf_end, &pid) !=
        STATUS_SUCCESS) {
        boot_log("Warning: failed to load init ELF");
    }

    /* Hand-written blobs for the isolation + fault-containment demos */
    if (user_process_spawn("user-canary-1", user_canary_start, user_canary_end, &pid) !=
            STATUS_SUCCESS ||
        user_process_spawn("user-canary-2", user_canary_start, user_canary_end, &pid) !=
            STATUS_SUCCESS ||
        user_process_spawn("user-rogue", user_rogue_start, user_rogue_end, &pid) !=
            STATUS_SUCCESS) {
        boot_log("Warning: failed to spawn user processes");
        return;
    }
    boot_log("User processes spawned (init ELF, 2x canary, 1x rogue)");

    /* CPU-quota enforcement proof (epic #144): brought up EARLY so its budget
     * kill lands well within the boot window (it accrues cpu_ticks only while
     * scheduled, diluted across the roster — starting first gives it margin). */
    user_cpu_hog_init();

    user_ipc_demo_init();
    user_ghost_demo_init();
    user_quantum_demo_init();
    user_kannaka_demo_init();
    user_qsv_init();
    /* qpud (the QPU executor) BEFORE qpu_test (the submitter): qpu_test
     * declares a dependency on qpud AND its bounded poll needs the executor
     * already fetch-ready (epic #148). */
    user_qpud_init();
    user_qpu_test_init();
    /* The agent-native end-to-end demo — after qpud so its QPU step can execute.
     * Its own sub-agent is started + IPC-wired inside. */
    user_agent_demo_init();
}

/* Bring up quantumd — a quantum-pool service (kannaka-quantum, a fourth
 * ghostOS citizen). grant_quantum_pool=1 mints its SYS_QRAND/SYS_QSEED read
 * cap on every start, so it draws real collapse-derived entropy and refuses to
 * fall back to a PRNG. Monitored, so the watchdog keeps it resident after it
 * prints its one-shot quantum demo. */
void user_quantum_demo_init(void) {
    service_definition_t quantumd_def = {
        .name = "quantumd",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_quantumd_elf_start,
        .user_elf_end = _binary_quantumd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .grant_quantum_pool = 1,
    };
    uint32_t sid = 0;
    if (service_register(&quantumd_def, &sid) == SVC_SUCCESS &&
        service_start("quantumd", NULL) == SVC_SUCCESS) {
        service_monitor(sid, true);
        boot_log("kannaka-quantum: quantumd (ring 3) drawing real quantum entropy");
    } else {
        boot_log("Warning: quantumd service failed to start");
    }
}

/* Bring up kannakad — the kannaka-memory citizen, rebased onto the KERNEL
 * holographic field (epic #95 phase 2). grant_field mints its region-1
 * CAP_RESOURCE_FIELD cap (scrubbing the region) on every start, so a
 * watchdog rebirth re-seeds a clean field rather than colliding with its
 * own stale imprints. Monitored: it heartbeats after its one-shot demo. */
void user_kannaka_demo_init(void) {
    service_definition_t kannakad_def = {
        .name = "kannakad",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_kannakad_elf_start,
        .user_elf_end = _binary_kannakad_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .grant_field = 1,
        .field_region = 1,
    };
    uint32_t sid = 0;
    if (service_register(&kannakad_def, &sid) == SVC_SUCCESS &&
        service_start("kannakad", NULL) == SVC_SUCCESS) {
        service_monitor(sid, true);
        boot_log("kannaka-memory: kannakad (ring 3) recalling via the kernel field");
    } else {
        boot_log("Warning: kannakad service failed to start");
    }
}

/* Bring up a user-space service (echo) via the service framework and a
 * client that talks to it over capability-checked IPC. Demonstrates the
 * microkernel model: a system service running as an isolated ring-3
 * process, reachable only by a process holding the right capability. */
void user_ipc_demo_init(void) {
    service_definition_t echo_def = {
        .name = "echo",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_echo_elf_start,
        .user_elf_end = _binary_echo_elf_end,
        .dependencies = {NULL},
        .max_restarts = 1,
    };

    uint32_t sid = 0;
    uint32_t echo_pid = 0, client_pid = 0;

    if (service_register(&echo_def, &sid) == SVC_SUCCESS &&
        service_start("echo", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            echo_pid = info.pid;
        }
    }
    if (echo_pid == 0) {
        boot_log("Warning: echo user-service failed to start");
        return;
    }

    if (user_process_spawn_elf("ipc-client", _binary_client_elf_start, _binary_client_elf_end,
                               &client_pid) != STATUS_SUCCESS) {
        boot_log("Warning: ipc-client failed to spawn");
        return;
    }

    /* Capability-as-address: grant each side a single IPC send-cap to
     * the other. These are the only IPC capabilities either holds, so
     * they can talk to each other and nothing else. */
    uint32_t cap = CAP_ID_INVALID;
    cap_create(client_pid, CAP_RESOURCE_IPC, echo_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(echo_pid, CAP_RESOURCE_IPC, client_pid, CAP_READ | CAP_WRITE, 0, &cap);

    boot_log("IPC demo: echo service (ring 3) + client wired via capabilities");

    /* A monitored user-process service: it heartbeats via SYS_HEARTBEAT
     * and the watchdog restarts it when it goes silent — demonstrating
     * that ring-3 services are first-class in the health monitor. */
    service_definition_t watched_def = {
        .name = "watched-svc",
        .entry = NULL,
        .user_elf_start = _binary_hbsvc_elf_start,
        .user_elf_end = _binary_hbsvc_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
    };
    uint32_t wsid = 0;
    if (service_register(&watched_def, &wsid) == SVC_SUCCESS &&
        service_start("watched-svc", NULL) == SVC_SUCCESS) {
        service_monitor(wsid, true);
        boot_log("Watchdog now monitoring a ring-3 user service (watched-svc)");
    }
}

/* Bring up ghostd — a Hopfield–Kuramoto associative-memory service
 * (ghostOS phase 1) — as a monitored ring-3 user-process service, and a
 * ghost_test client that drives the boot self-test / merge gate over
 * capability-checked IPC. Same shape as the echo demo: register + start
 * the service, spawn the client, then grant each side exactly one IPC
 * send-capability to the other (capability-as-address). */
void user_ghost_demo_init(void) {
    service_definition_t ghostd_def = {
        .name = "ghostd",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_ghostd_elf_start,
        .user_elf_end = _binary_ghostd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        /* ghostd's perturbation noise reads the quantum pool; declaring the
         * cap here means the service framework re-mints it on every start,
         * so a watchdog-reborn ghostd regains qseed-derived noise instead of
         * degrading permanently to a PRNG. */
        .grant_quantum_pool = 1,
    };

    uint32_t sid = 0;
    uint32_t ghostd_pid = 0, test_pid = 0;

    if (service_register(&ghostd_def, &sid) == SVC_SUCCESS &&
        service_start("ghostd", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            ghostd_pid = info.pid;
        }
    }
    if (ghostd_pid == 0) {
        boot_log("Warning: ghostd service failed to start");
        return;
    }

    if (user_process_spawn_elf("ghost-test", _binary_ghost_test_elf_start,
                               _binary_ghost_test_elf_end, &test_pid) != STATUS_SUCCESS) {
        boot_log("Warning: ghost-test failed to spawn");
        return;
    }

    uint32_t cap = CAP_ID_INVALID;
    cap_create(test_pid, CAP_RESOURCE_IPC, ghostd_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(ghostd_pid, CAP_RESOURCE_IPC, test_pid, CAP_READ | CAP_WRITE, 0, &cap);

    /* ghostd's quantum-pool read cap is (re-)minted by the service framework
     * from ghostd_def.grant_quantum_pool on every start, so it survives a
     * watchdog restart. ghost-test is deliberately left without one: its
     * capless SYS_QRAND attempt is the proof-by-attack the gate denies
     * (EPERM). */

    /* Let the watchdog keep ghostd alive; it heartbeats via SYS_HEARTBEAT
     * and reprints "GHOSTD: FIELD REBORN" if it is ever restarted. */
    service_monitor(sid, true);

    boot_log("ghostOS: ghostd (ring 3) + ghost-test wired via capabilities");

    /* ghostOS phase 3: bring up paradoxd, coupled to ghostd's field. */
    user_paradox_demo_init(ghostd_pid);
}

/* Bring up paradoxd — a fixed-point paradox-resolution service (ghostOS
 * phase 3, #50) — as a monitored ring-3 user-process service coupled to
 * ghostd's field, plus a paradox-test client that hands it a canned
 * contradiction over capability-checked IPC.
 *
 * Wiring (all capability-as-address):
 *   - paradox-test -> paradoxd  : one IPC send-cap, to hand over the gate's
 *                                 canned contradiction (hand-minted below —
 *                                 the one-shot test citizen stays outside
 *                                 the declarative table, ADR-0023).
 *   - paradoxd    <-> ghostd    : declared via ipc_peers (ADR-0023), so a
 *                                 watchdog rebirth of EITHER end re-mints
 *                                 the pair — paradoxd polls ghostd STATUS,
 *                                 ghostd replies to the sender (SYS_SEND_TO).
 * paradoxd is given NO cap back to paradox-test: the merge gate is paradoxd's
 * own printed RESOLVED line, so it never needs to answer the client. */
void user_paradox_demo_init(uint32_t ghostd_pid) {
    (void)ghostd_pid; /* wiring is declarative now (ipc_peers, ADR-0023) */
    service_definition_t paradoxd_def = {
        .name = "paradoxd",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_paradoxd_elf_start,
        .user_elf_end = _binary_paradoxd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .ipc_peers = {{"ghostd", CAP_READ | CAP_WRITE, CAP_READ | CAP_WRITE}},
    };

    uint32_t sid = 0;
    uint32_t paradoxd_pid = 0, test_pid = 0;

    if (service_register(&paradoxd_def, &sid) == SVC_SUCCESS &&
        service_start("paradoxd", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            paradoxd_pid = info.pid;
        }
    }
    if (paradoxd_pid == 0) {
        boot_log("Warning: paradoxd service failed to start");
        return;
    }

    if (user_process_spawn_elf("paradox-test", _binary_paradox_test_elf_start,
                               _binary_paradox_test_elf_end, &test_pid) != STATUS_SUCCESS) {
        boot_log("Warning: paradox-test failed to spawn");
        return;
    }

    uint32_t cap = CAP_ID_INVALID;
    /* client -> paradoxd (hand over the contradiction). paradox-test is a
     * one-shot citizen, deliberately NOT in the declarative table. The
     * paradoxd<->ghostd pair is minted by start_slot from paradoxd_def's
     * ipc_peers (ADR-0023) — the two services are coupled through ghostd's
     * field over cap-checked IPC, and a rebirth of either end re-wires. */
    cap_create(test_pid, CAP_RESOURCE_IPC, paradoxd_pid, CAP_READ | CAP_WRITE, 0, &cap);

    service_monitor(sid, true);

    boot_log("ghostOS: paradoxd (ring 3) coupled to ghostd via capabilities");

    /* epic #97: bring up fieldsyncd, the UDP field-coupling bridge. */
    user_fieldsync_demo_init(ghostd_pid);

    /* epic #98: bring up httpd, the TCP status-page server. */
    user_httpd_init();

    /* 0xSCADA stage 4: bring up modbusd, the Modbus/TCP agent. */
    user_modbusd_init();

    /* ghostd phase 4: bring up swarm_svc, the COM2 serial swarm bridge. */
    user_swarm_demo_init(ghostd_pid);
}

/* Bring up httpd — the HTTP status-page server (epic #98). It holds ONLY
 * the network cap. That grant is honestly coarser than the job: grant_net
 * also gates SYS_UDP / SYS_RESOLVE / outbound TCP connects, so httpd's
 * code deliberately contains no outbound operation and discards the
 * request bytes unparsed — an audit of user/httpd.c should keep it that
 * way. Needs no IPC wiring (the body is built from uncapped SYS_TICKS /
 * SYS_SYSINFO). Without a NIC it logs once and idles; the default boot
 * is unchanged. Monitored. */
/* modbusd — a Modbus/TCP server on :502 (0xSCADA stage 4). Reads serve
 * live kernel telemetry; CONTROL WRITES ARE REFUSED and the refusal is
 * counted into a register any poller can read.
 *
 * The grant is grant_net, the same coarse cap httpd holds, and the same
 * caveat applies with more force here: it also gates SYS_UDP /
 * SYS_RESOLVE / outbound TCP connects, so user/modbusd.c deliberately
 * contains no outbound operation and an audit should keep it that way.
 *
 * Worth stating where the grant is made rather than only in the program:
 * the write refusal in modbusd is enforced BY THE PROGRAM, not by this
 * capability. There is no cap meaning "may write holding register N".
 * That gap is the point of the exercise, not an oversight — see
 * 0xSCADA ADR-0028. Without a NIC it logs once and idles; the default
 * boot is unchanged. Monitored. */
void user_modbusd_init(void) {
    service_definition_t modbusd_def = {
        .name = "modbusd",
        .entry = NULL,
        .user_elf_start = _binary_modbusd_elf_start,
        .user_elf_end = _binary_modbusd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .grant_net = 1,
    };
    uint32_t sid = 0;
    if (service_register(&modbusd_def, &sid) != SVC_SUCCESS ||
        service_start("modbusd", NULL) != SVC_SUCCESS) {
        boot_log("Warning: modbusd service failed to start");
        return;
    }
    boot_log("ghostOS: modbusd (ring 3) listening on Modbus/TCP :502");
}

void user_httpd_init(void) {
    service_definition_t httpd_def = {
        .name = "httpd",
        .entry = NULL,
        .user_elf_start = _binary_httpd_elf_start,
        .user_elf_end = _binary_httpd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .grant_net = 1,
    };
    uint32_t sid = 0;
    if (service_register(&httpd_def, &sid) != SVC_SUCCESS ||
        service_start("httpd", NULL) != SVC_SUCCESS) {
        boot_log("Warning: httpd service failed to start");
        return;
    }
    service_monitor(sid, true);
    boot_log("epic98: httpd (ring 3) serving the status page on :8080");
}

/* fieldsyncd's pid, remembered for the agent demo (epic #178): agentd hands
 * its society's aggregate to fieldsyncd over an IPC pair minted in
 * user_agent_demo_init (agentdemo boots only). */
static uint32_t g_fieldsyncd_pid;

/* Bring up fieldsyncd — the UDP field-coupling bridge (epic #97). It holds
 * the network cap (grant_net, for SYS_UDP) and declares its IPC wiring
 * (ADR-0023): a ghostd pair (pull phase snapshots, get replies) and the
 * ONE-WAY swarm-svc->fieldsyncd key-delivery cap (ADR-0019) — from-only,
 * to_perms 0, because a second outbound cap on fieldsyncd's side would make
 * its untargeted send to ghostd first-match ambiguously. swarm-svc does not
 * exist yet when fieldsyncd starts; start_slot's Pass 2 mints the key cap
 * when it does, and every watchdog rebirth of either end re-mints both
 * pairs. With no `peer=` configured it idles; the default boot is
 * unchanged. Monitored. */
void user_fieldsync_demo_init(uint32_t ghostd_pid) {
    (void)ghostd_pid; /* wiring is declarative now (ipc_peers, ADR-0023) */
    service_definition_t fieldsyncd_def = {
        .name = "fieldsyncd",
        .entry = NULL,
        .user_elf_start = _binary_fieldsyncd_elf_start,
        .user_elf_end = _binary_fieldsyncd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        .grant_net = 1,
        .ipc_peers = {{"ghostd", CAP_READ | CAP_WRITE, CAP_READ | CAP_WRITE},
                      {"swarm-svc", 0, CAP_WRITE}},
    };
    uint32_t sid = 0, fs_pid = 0;
    if (service_register(&fieldsyncd_def, &sid) == SVC_SUCCESS &&
        service_start("fieldsyncd", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            fs_pid = info.pid;
        }
    }
    if (fs_pid == 0) {
        boot_log("Warning: fieldsyncd service failed to start");
        return;
    }
    service_monitor(sid, true);
    g_fieldsyncd_pid = fs_pid;
    boot_log("epic97: fieldsyncd (ring 3) bridges ghostd's field to a UDP peer");
}

/* Bring up swarm_svc — the COM2 serial swarm bridge (ghostd phase 4, #51) —
 * as a monitored ring-3 user-process service. It is declaratively granted the
 * COM2 device cap and a quantum-pool read cap, and declares its ghostd IPC
 * pair (ADR-0023) so a remote DATA request can be routed to the field over
 * capability-checked IPC and ghostd can reply to the sender (SYS_SEND_TO) —
 * all re-minted on every start, so a rebirth of either end re-wires.
 * swarm_svc holds no other outbound authority beyond the ONE-WAY
 * swarm-svc->fieldsyncd key cap that FIELDSYNCD declares (from-peer, minted
 * by start_slot's Pass 2 when swarm-svc starts): it can drive COM2, draw
 * quantum bytes, talk to ghostd, and deliver the session key — nothing else.
 * Pass 1 (ghostd) minting before Pass 2 (the key cap) also keeps the ghostd
 * cap at the lower first-fit slot, which swarm_svc's one-shot untargeted
 * discovery send relies on. */
void user_swarm_demo_init(uint32_t ghostd_pid) {
    (void)ghostd_pid; /* wiring is declarative now (ipc_peers, ADR-0023) */
    service_definition_t swarm_def = {
        .name = "swarm-svc",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_swarm_svc_elf_start,
        .user_elf_end = _binary_swarm_svc_elf_end,
        .dependencies = {NULL},
        .max_restarts = 2,
        /* Lamport key material is drawn from the qseed-mixed quantum pool. */
        .grant_quantum_pool = 1,
        /* Sole ring-3 holder of the COM2 device capability. */
        .grant_com2 = 1,
        /* QPU SUBMIT right (epic #149 B1): the bridge forwards a host-framed
         * opaque circuit to the SYS_QPU broker and returns the exact result
         * over COM2. qsub_max bounds wire-driven submissions per incarnation.
         * 5: the CI gate charges 5 (bell + grover3 + a large circuit + a
         * malformed one + an int32-overflowing one — a submission that reaches
         * the broker consumes quota even if the executor later rejects the
         * circuit) then proves the 6th is refused. */
        .grant_qpu_submit = 1,
        .qsub_max = 5,
        .ipc_peers = {{"ghostd", CAP_READ | CAP_WRITE, CAP_READ | CAP_WRITE}},
    };

    uint32_t sid = 0;
    uint32_t swarm_pid = 0;

    if (service_register(&swarm_def, &sid) == SVC_SUCCESS &&
        service_start("swarm-svc", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            swarm_pid = info.pid;
        }
    }
    if (swarm_pid == 0) {
        boot_log("Warning: swarm-svc service failed to start");
        return;
    }

    /* IPC wiring is declarative (ADR-0023): start_slot minted the
     * swarm_svc<->ghostd pair (Pass 1, from swarm_def.ipc_peers) and the
     * one-way swarm_svc->fieldsyncd key cap (Pass 2, from FIELDSYNCD's
     * declaration) inside the same cli window as the device grants — and
     * re-mints all of it on every watchdog rebirth of either end. */

    service_monitor(sid, true);

    boot_log("ghostOS: swarm-svc (ring 3) bridging COM2, wired to ghostd");

    /* Epic #62: the interactive shell is the last citizen up, once the
     * services it can talk to already exist. */
    user_shell_init(ghostd_pid);

    /* Epic #135: the spawn-quota enforcement proof runs last of all, after
     * the shell — it briefly consumes a spawn cap the shell also holds, so
     * ordering it after keeps the boot roster stable. */
    user_quota_test_init();

    /* Epic #137: cross-ring capability delegation — a delegator hands a
     * narrowed field cap to a sub-agent. Last, so its IPC/derive activity
     * never perturbs the earlier citizens. */
    user_delegation_demo_init();
}

/* Bring up qsh — the interactive shell (epic #62 phase 1, #63) — as a
 * monitored ring-3 user-process service. Its authority is declarative and
 * minimal: the console device capability (its whole reason to exist) and a
 * quantum-pool read cap (the qrand/qseed builtins) are re-minted by the
 * service framework on every start, so a watchdog-reborn shell keeps its
 * console. Its ghostd IPC pair (the `ghost` builtin; ghostd replies to the
 * sender via SYS_SEND_TO) is DECLARED via ipc_peers (ADR-0023) and re-minted
 * on every start like the other grants — a reborn shell keeps its `ghost`
 * builtin, deterministically. qsh must hold exactly ONE outbound IPC cap
 * (untargeted send_msg routes first-match, #176), which single-sided
 * declaration preserves: qsh declares only ghostd and nobody declares qsh.
 *
 * `exit` is a feature, not a crash: the shell terminates, its heartbeat
 * goes silent, and the watchdog restarts it — the reborn banner is the
 * boot-log proof that an OS operator surface can die and come back. */
void user_shell_init(uint32_t ghostd_pid) {
    (void)ghostd_pid; /* wiring is declarative now (ipc_peers, ADR-0023) */
    service_definition_t qsh_def = {
        .name = "qsh",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_qsh_elf_start,
        .user_elf_end = _binary_qsh_elf_end,
        .dependencies = {NULL},
        .max_restarts = 3,
        .grant_quantum_pool = 1,
        .grant_console = 1,
        /* The shell is the operator surface: it alone may start programs
         * off the initrd (SYS_SPAWN) and write the filesystem (create/
         * unlink/sync — epic #71). Re-minted on every start like the
         * other declared grants. */
        .grant_spawn = 1,
        .grant_fswrite = 1,
        /* Network access: the shell alone may resolve hostnames (SYS_RESOLVE). */
        .grant_net = 1,
        /* Holographic memory (epic #95): the shell holds field region 0 —
         * `imprint`/`recall` builtins give the operator associative
         * memory at the prompt. Scrubbed + re-minted on every restart,
         * EXCEPT the first grant of a boot when the disk restored the
         * region (epic #96 field_inherit): the operator's synced
         * memories survive reboot, inherited exactly once, audibly. */
        .grant_field = 1,
        .field_region = 0,
        .field_inherit = 1,
        .ipc_peers = {{"ghostd", CAP_READ | CAP_WRITE, CAP_READ | CAP_WRITE}},
    };

    uint32_t sid = 0;
    uint32_t qsh_pid = 0;

    if (service_register(&qsh_def, &sid) == SVC_SUCCESS &&
        service_start("qsh", NULL) == SVC_SUCCESS) {
        service_info_t info;
        if (service_status(sid, &info) == SVC_SUCCESS) {
            qsh_pid = info.pid;
        }
    }
    if (qsh_pid == 0) {
        boot_log("Warning: qsh shell service failed to start");
        return;
    }

    /* qsh <-> ghostd wiring is declarative (ipc_peers above, ADR-0023):
     * start_slot minted the pair alongside the console/spawn/field grants,
     * and re-mints it on every rebirth of either end. */
    service_monitor(sid, true);

    boot_log("qsh: interactive shell online (ring 3, console capability)");
}

/* Bring up quota-test (epic #135) — the un-echoable proof that the spawn
 * QUOTA is ENFORCED, not merely declared. Registered with grant_spawn +
 * spawn_max=1, it spawns /bin/qprobe twice: the FIRST succeeds, the SECOND
 * is refused by the manifest quota (EPERM -4, recorded in the ledger as
 * AUDIT_QUOTA) — a denial the capability layer alone would never produce,
 * since quota-test holds a valid spawn cap. It prints "QUOTA ENFORCED" only
 * when the outcome is exactly {first>0, second==-4}, else "QUOTA BROKEN".
 *
 * DELIBERATELY NOT service_monitor()'d — unlike every other service here
 * (quantumd:1646, kannakad:1672, ghostd, qsh:2171). quota-test needs the
 * service framework only because grant_spawn is minted exclusively by
 * start_slot; monitoring it would let the watchdog respawn it, re-running the
 * proof and resetting the per-incarnation quota. It runs its proof once and
 * then loops idle (no heartbeat). ghost_test avoids this by being a plain
 * user_process_spawn_elf — not an option here, since we need grant_spawn. */
void user_quota_test_init(void) {
    service_definition_t quota_test_def = {
        .name = "quota-test",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_quota_test_elf_start,
        .user_elf_end = _binary_quota_test_elf_end,
        .dependencies = {NULL},
        .max_restarts = 1,
        .grant_spawn = 1,
        .spawn_max = 1, /* the first enforced quota: one successful spawn */
    };
    uint32_t sid = 0;
    if (service_register(&quota_test_def, &sid) == SVC_SUCCESS &&
        service_start("quota-test", NULL) == SVC_SUCCESS) {
        /* NO service_monitor(sid, true) — see the function comment. */
        boot_log("quota-test: spawn-quota enforcement proof (ring 3)");
    } else {
        boot_log("Warning: quota-test service failed to start");
    }
}

/* Bring up the capability-DELEGATION demo (epic #137 Phase D increment 3): a
 * delegator that holds CAP_GRANT over field region 2 and a sub-agent that holds
 * nothing. At runtime the delegator hands the sub-agent a NARROWED READ-only
 * cap over region 2 via SYS_CAP_DERIVE — "an agent hands a narrowed intent to a
 * sub-agent." Both are registered but NOT monitored (the sub-agent must not be
 * restarted, which would rebind its manifest and drop the delegated row; the
 * delegator must be able to EXIT so the reaper cascade-revokes the derived cap,
 * proving delegated provenance). They coordinate over a capability-checked IPC
 * pair minted here (the qsh<->ghostd pattern). Both spawn READY but do not run
 * until the timer starts after user_init, so the IPC caps exist before either
 * citizen sends. */
void user_delegation_demo_init(void) {
    service_definition_t subagentd_def = {
        .name = "subagentd",
        .entry = NULL, /* user-process service; NO grants — capless by design */
        .user_elf_start = _binary_subagentd_elf_start,
        .user_elf_end = _binary_subagentd_elf_end,
        .dependencies = {NULL},
        .max_restarts = 1,
    };
    service_definition_t delegation_def = {
        .name = "delegation-test",
        .entry = NULL,
        .user_elf_start = _binary_delegation_test_elf_start,
        .user_elf_end = _binary_delegation_test_elf_end,
        .dependencies = {NULL},
        .max_restarts = 1,
        /* The sole CAP_GRANT holder: READ|WRITE|GRANT over field region 2. */
        .grant_field = 1,
        .field_region = 2,
        .grant_field_delegable = 1,
    };

    uint32_t sub_sid = 0, del_sid = 0, sub_pid = 0, del_pid = 0;
    service_info_t info;
    if (service_register(&subagentd_def, &sub_sid) == SVC_SUCCESS &&
        service_start("subagentd", NULL) == SVC_SUCCESS &&
        service_status(sub_sid, &info) == SVC_SUCCESS) {
        sub_pid = info.pid;
    }
    if (service_register(&delegation_def, &del_sid) == SVC_SUCCESS &&
        service_start("delegation-test", NULL) == SVC_SUCCESS &&
        service_status(del_sid, &info) == SVC_SUCCESS) {
        del_pid = info.pid;
    }
    if (sub_pid == 0 || del_pid == 0) {
        boot_log("Warning: delegation demo failed to start");
        return;
    }

    /* Capability-checked IPC pair: the delegator can reach the sub-agent (the
     * SYS_CAP_DERIVE IPC-peer requirement) and vice versa. Both directions, so
     * the sub-agent can send its "ready"/"proven" and the delegator its
     * "go". NOT monitored — leave both to run their one-shot proof and idle. */
    uint32_t cap = CAP_ID_INVALID;
    cap_create(del_pid, CAP_RESOURCE_IPC, sub_pid, CAP_READ | CAP_WRITE, 0, &cap);
    cap_create(sub_pid, CAP_RESOURCE_IPC, del_pid, CAP_READ | CAP_WRITE, 0, &cap);

    boot_log("delegation-test: cross-ring capability delegation demo (ring 3)");
}

/* Bring up cpu-hog (epic #144) — the un-echoable proof that the manifest CPU
 * quota is ENFORCED, not merely accounted. A ring-3 service with a finite
 * cpu_limit that busy-spins forever: once its scheduled-in cpu_ticks cross the
 * limit, the kernel terminates it from the timer tick. Only a user-process
 * service can carry a cpu_limit (start_slot copies def.cpu_limit into the
 * manifest). Registered NOT monitored — the kernel also refuses to monitor a
 * cpu_limit service, since a watchdog respawn would reset the budget and
 * re-kill it forever. The kill leaves a CPUKILL authority-ledger entry for its
 * pid, and the pid vanishes from the process/manifest tables — the external,
 * un-forgeable proof (the hog is dead and cannot self-report). */
void user_cpu_hog_init(void) {
    service_definition_t cpu_hog_def = {
        .name = "cpu-hog",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_cpu_hog_elf_start,
        .user_elf_end = _binary_cpu_hog_elf_end,
        .dependencies = {NULL},
        .max_restarts = 1,
        .cpu_limit = 10, /* ~10 scheduled-in ticks then terminated */
    };
    uint32_t sid = 0;
    if (service_register(&cpu_hog_def, &sid) == SVC_SUCCESS &&
        service_start("cpu-hog", NULL) == SVC_SUCCESS) {
        /* NOT service_monitor'd — the kernel refuses it anyway (see the
         * function comment). */
        boot_log("cpu-hog: CPU-quota enforcement proof (ring 3, finite budget)");
    } else {
        boot_log("Warning: cpu-hog service failed to start");
    }
}

/* Bring up qsv — the EXACT integer quantum state-vector citizen (epic #148,
 * the native tier of the quantum stack). Pure computation + console output:
 * no grant flags at all (its authority is the null set — the proof needs no
 * capabilities, and its manifest stays empty). Runs its Bell/GHZ/Grover
 * proofs to integer equality, prints a state digest that CI cross-checks
 * against an independent host-side mirror, and exits (reaped; NOT
 * monitored — a one-shot proof, like ghost_test). */
void user_qsv_init(void) {
    service_definition_t qsv_def = {
        .name = "qsv",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_qsv_elf_start,
        .user_elf_end = _binary_qsv_elf_end,
        .dependencies = {NULL},
        .max_restarts = 1,
    };
    uint32_t sid = 0;
    if (service_register(&qsv_def, &sid) == SVC_SUCCESS &&
        service_start("qsv", NULL) == SVC_SUCCESS) {
        boot_log("qsv: exact integer quantum state-vector proof (ring 3, zero rounding)");
    } else {
        boot_log("Warning: qsv service failed to start");
    }
}

/* Bring up qpud — the QPU executor service (epic #148, quantum-stack A2). The
 * SOLE holder of the QPU EXECUTE cap (grant_qpu_execute mints READ|EXECUTE,
 * never WRITE), so it is the only process that may FETCH and COMPLETE brokered
 * jobs. Monitored (a watchdog rebirth re-mints its cap + rebinds its manifest;
 * the broker fails any job it was mid-executing closed to EXECFAIL). */
void user_qpud_init(void) {
    service_definition_t qpud_def = {
        .name = "qpud",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_qpud_elf_start,
        .user_elf_end = _binary_qpud_elf_end,
        .dependencies = {NULL},
        .max_restarts = 5,
        .grant_qpu_execute = 1,
    };
    uint32_t sid = 0;
    if (service_register(&qpud_def, &sid) == SVC_SUCCESS &&
        service_start("qpud", NULL) == SVC_SUCCESS) {
        service_monitor(sid, true);
        boot_log("qpud: QPU executor (exact integer engine, ring 3)");
    } else {
        boot_log("Warning: qpud service failed to start");
    }
}

/* Bring up qpu_test — the QPU broker proof citizen (epic #148, A2+A3). Holds
 * the SUBMIT cap with a manifest qsub quota of 2; submits opaque circuits,
 * polls the kernel for qpud's exact results, and proves the quota + cross-perm
 * partition. Depends on qpud (must be fetch-ready). NOT monitored — a one-shot
 * proof that idles forever after (a monitored respawn would double the ledger's
 * QSUBMIT count). */
void user_qpu_test_init(void) {
    service_definition_t qt_def = {
        .name = "qpu-test",
        .entry = NULL, /* user-process service */
        .user_elf_start = _binary_qpu_test_elf_start,
        .user_elf_end = _binary_qpu_test_elf_end,
        .dependencies = {"qpud", NULL},
        .max_restarts = 1,
        .grant_qpu_submit = 1,
        .qsub_max = 2,
    };
    uint32_t sid = 0;
    if (service_register(&qt_def, &sid) == SVC_SUCCESS &&
        service_start("qpu-test", NULL) == SVC_SUCCESS) {
        boot_log("qpu-test: QPU broker proof (submit/execute/poll + quota + partition)");
    } else {
        boot_log("Warning: qpu-test service failed to start");
    }
}

/* The agent-native end-to-end demo (the mission showcase). agentd is the
 * orchestrator: it holds the grants its story needs — QPU submit (qsub quota),
 * field region 3, the CAP_GRANT to DELEGATE that region, spawn, and the
 * spawn-CHANNEL opt-in (epic #175) — and ASSEMBLES ITS OWN SOCIETY: it spawns
 * AGENT_SUBS /bin/agentsub citizens itself, and each spawn mints the
 * bidirectional capability-checked IPC pair (the SYS_CAP_DERIVE peer
 * requirement + the handshake) with no kernel hand-wiring. The kernel's whole
 * job here is registering the one orchestrator. Not monitored: the demo is a
 * one-shot proof; everything it spawns exits, its channel caps UNLINK, and its
 * own caps cascade-revoke. */
void user_agent_demo_init(void) {
    /* The demo is a heavyweight SHOWCASE (an extra QPU job, spawns, a field
     * imprint/recall, and a delegation fan-out), so it is OPT-IN via the
     * `agentdemo` cmdline token — off by default so its added boot work never
     * delays the timing-sensitive proofs other CI gates poll for (e.g. the MCP
     * gate's delegation-reap) without those gates ever checking the demo. The
     * default ci-smoke boot and the GRUB "agent-native demo" entry pass the
     * token; the token is decoupled from `quiet`, so the showcase runs on a
     * CLEAN console. */
    if (!boot_run_agent_demo()) {
        return;
    }
    service_definition_t agentd_def = {
        .name = "agentd",
        .entry = NULL,
        .user_elf_start = _binary_agentd_elf_start,
        .user_elf_end = _binary_agentd_elf_end,
        .dependencies = {"qpud", NULL}, /* its QPU step needs the executor fetch-ready */
        .max_restarts = 1,
        .grant_qpu_submit = 1,
        .qsub_max = 4,
        .grant_spawn = 1,
        /* /bin/hello (step 3) + the 3-sub society (step 4) — exactly 4. */
        .spawn_max = 4,
        /* Every agentd spawn mints a parent<->child IPC pair (epic #175):
         * the roster of its society IS its spawn returns. */
        .grant_spawn_channel = 1,
        /* Sole CAP_GRANT holder for regions 3-6 (epic #177): region 3 is the
         * shared knowledge it imprints and delegates READ-narrowed; regions
         * 4-6 are the specialists' private workspaces, delegated
         * READ|WRITE-narrowed, one each. Manifest rows: spawn 1 + FIELD 4 +
         * QPU 1 = 6 of MANIFEST_MAX_ENTRIES 8. */
        .grant_field = 1,
        .field_region = 3,
        .field_region_span = 4,
        .grant_field_delegable = 1,
        /* SYS_QSEED (epic #178): the society's published aggregate is salted
         * with the boot identity (qseed) so two coupled VMs' aggregates
         * provably DIFFER and the host can verify cross-appearance. Manifest
         * budget: quantum 1 + spawn 1 + FIELD:3-6 4 + QPU 1 = 7 of 8 rows —
         * ONE row of headroom left; a 9th would hit the fail-closed guard
         * (epic #177) and be dropped loudly. Budget before adding grants. */
        .grant_quantum_pool = 1,
    };

    uint32_t ag_sid = 0, ag_pid = 0;
    service_info_t info;
    if (service_register(&agentd_def, &ag_sid) == SVC_SUCCESS &&
        service_start("agentd", NULL) == SVC_SUCCESS &&
        service_status(ag_sid, &info) == SVC_SUCCESS) {
        ag_pid = info.pid;
    }
    if (ag_pid == 0) {
        boot_log("Warning: agentd (society orchestrator) failed to start");
        return;
    }

    /* Society-of-societies wiring (epic #178): agentd hands its aggregate to
     * fieldsyncd over this pair (agentd discovers the pid via SYSINFO_PS, the
     * qtop pattern); fieldsyncd broadcasts it to configured peers as an FSYP
     * frame and prints received peer aggregates for host-side verification.
     * Minted ONLY in agentdemo boots — every other boot is unchanged.
     * Deliberately NOT converted to ipc_peers (ADR-0023), and honestly: the
     * generalized dead-target unlink makes a fieldsyncd rebirth leave agentd
     * permanently capless here (it used to sometimes re-attach by pid-reuse
     * luck) — accepted, demo-only, no CI gate depends on this pair surviving
     * a restart. */
    if (g_fieldsyncd_pid != 0) {
        uint32_t cap = CAP_ID_INVALID;
        cap_create(ag_pid, CAP_RESOURCE_IPC, g_fieldsyncd_pid, CAP_READ | CAP_WRITE, 0, &cap);
        cap_create(g_fieldsyncd_pid, CAP_RESOURCE_IPC, ag_pid, CAP_READ | CAP_WRITE, 0, &cap);
    }

    boot_log(
        "agentd: agent-native society demo (orchestrator spawns + delegates to its own society)");
}
