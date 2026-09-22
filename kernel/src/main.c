#include <kernel/types.h>
#include <kernel/boot.h>
#include <kernel/memory.h>
#include <kernel/interrupts.h>
#include <kernel/ipc.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/capability.h>
#include <kernel/manifest.h>
#include <kernel/quantum.h>
#include <kernel/service.h>
#include <kernel/gdt.h>
#include <kernel/syscall.h>
#include <kernel/com2_uart.h>
#include <kernel/console.h>
#include <kernel/initrd.h>
#include <kernel/ata.h>
#include <kernel/ramfs.h>
#include <kernel/netdev.h>
#include <kernel/net.h>
#include <kernel/vga.h>
#include <kernel/fb.h>
#ifdef SCHED_RESONANT
#include <kernel/resonant_fixed.h>
#endif

// Single-sourced version string: the build injects -DQOS_VERSION="vX.Y.Z" from
// the repo-root VERSION file (see the Makefile). The fallback keeps a
// version-less local build honest rather than baking a stale literal.
#ifndef QOS_VERSION
#define QOS_VERSION "v0.0-dev"
#endif

// Boot splash dispatch: framebuffer when available, else VGA text
static void splash_begin(void) {
    if (fb_available())
        fb_boot_splash();
    else
        vga_boot_splash();
}
static void splash_stage(const char *label, int percent) {
    if (fb_available())
        fb_boot_stage(label, percent);
    else
        vga_boot_stage(label, percent);
}
static void splash_ready(void) {
    if (fb_available())
        fb_boot_ready();
    else
        vga_boot_ready();
}

// External symbols from linker script
extern uint8_t __bss_start;
extern uint8_t __bss_end;
extern uint8_t __end;

// ELF-loader hardening self-test (kernel/src/process_test.c): drives the
// malformed-spawn rejection paths and asserts no frame leak. Returns 0 on pass.
extern int elf_spawn_selftest(void);

// Spawn address-space-leak self-test (kernel/src/process_test.c): drives the
// process_create-failure path and asserts the built address space is reclaimed.
extern int spawn_leak_selftest(void);

// Boot state tracking
static boot_state_t current_boot_state = BOOT_STATE_FIRMWARE;
static boot_config_t boot_config;

// Forward declarations
static void kernel_init(void);
static void early_init(void);
static void hal_init(void);
static void memory_subsystem_init(void);
static void interrupts_subsystem_init(void);
static void core_services_init(void);
static void ipc_subsystem_init(void);
static void process_subsystem_init(void);
static void scheduler_subsystem_init(void);
static void parse_boot_cmdline(uint32_t info_addr);
static uint64_t multiboot_parse_memory(uint32_t info_addr);
static int boot_validate_selftest(void);

// Kernel main entry point
void kernel_main(uint32_t magic, uint32_t info_addr) {
    current_boot_state = BOOT_STATE_KERNEL_ENTRY;

    // Validate multiboot. Give a Multiboot2 handoff its own honest banner instead
    // of the generic "invalid" — the kernel is Multiboot1-only (see ADR-0021), and
    // a future UEFI/Limine bring-up should see exactly why it was refused.
    if (!boot_validate_multiboot(magic, info_addr)) {
        if (magic == MULTIBOOT2_MAGIC) {
            boot_panic("Multiboot2 handoff unsupported (kernel is Multiboot1-only)");
        }
        boot_panic("Invalid multiboot information");
        return; // Never reached
    }

    // Parse boot configuration
    boot_config.magic = magic;
    boot_config.boot_flags = *(uint32_t *)(uintptr_t)info_addr;
    // Size physical RAM from the (untrusted) multiboot memory map. Safe to read
    // the fixed MB1 offsets here: boot_validate_multiboot confirmed the MB1 magic
    // just above, and this is the ONLY consumer of the mmap — it must stay AFTER
    // the validation (running it on a v2 info block re-wild-writes like the
    // closed fb bug).
    boot_config.total_memory = multiboot_parse_memory(info_addr);

    boot_log("QuantumOS " QOS_VERSION " booting...");
    boot_log("Multiboot information validated");

    // Parse the boot command line for a quantum entropy seed
    // (qseed=<hex>) handed in by the qBraid boot harness
    parse_boot_cmdline(info_addr);

    // Use a linear framebuffer if the loader provided one (GRUB ISO /
    // real hardware); otherwise the VGA text splash. The QEMU -kernel
    // path — CI and the qBraid watch window — provides none and stays
    // in text mode.
    if (fb_init_from_multiboot(info_addr)) {
        boot_log("Linear framebuffer active — graphical boot splash");
    } else {
        boot_log("No framebuffer — VGA text boot splash");
    }

    // Early initialization
    early_init();

    // Main kernel initialization
    kernel_init();

    // Should never reach here
    boot_panic("Kernel initialization completed unexpectedly");
}

// Early initialization (before memory management)
static void early_init(void) {
    boot_log("Starting early initialization...");

    // Initialize early console
    early_console_init();

    // Basic CPU setup
    // TODO: CPU detection and setup

    boot_log("Early initialization complete");
}

// Main kernel initialization
static void kernel_init(void) {
    boot_log("Starting kernel initialization...");

    // Draw the boot splash; each subsystem advances the progress bar
    // and travels the interference wave one step further
    splash_begin();
    splash_stage("hardware abstraction layer", 5);

    // Initialize HAL
    hal_init();

    // Initialize memory management
    splash_stage("memory management", 20);
    memory_subsystem_init();

    // Initialize interrupt system
    splash_stage("interrupt controller", 35);
    interrupts_subsystem_init();

    // Install the full GDT + TSS (user-mode descriptors, ring-3 entry
    // stack) and the syscall gate
    splash_stage("descriptor tables + syscalls", 45);
    gdt_init();
    syscall_init();

    // Bring up the COM2 serial swarm bridge (0x2F8, polled, capability-guarded).
    // COM1/console is untouched; swarm_svc drives this port from ring 3.
    com2_init();

    // Console INPUT (epic #62): COM1 RX + PS/2 keyboard feed a kernel ring
    // buffer; ring 3 reads it via the capability-guarded SYS_CONS. Any byte
    // CI has already piped into the serial console is rescued here, before
    // interrupts are even on. IRQ1/IRQ4 unmask alongside the timer below.
    console_init();

    // Mount the embedded initrd (epic #62 phase 2): a read-only ustar
    // archive baked into the kernel image, served to ring 3 through the
    // SYS_OPEN/SYS_READ/SYS_CLOSE/SYS_READDIR VFS calls.
    initrd_init();

    // Probe the ATA disk (epic #71): the persistence substrate. A
    // diskless boot logs itself honestly and changes nothing else.
    // With a disk carrying a QDSK archive, the RAM filesystem overlay
    // is restored from it — files written and synced in a previous
    // boot come back.
    ata_init();
    persist_restore();

    // Probe for a NIC (epic #73). netdev_init walks the driver list and
    // binds the first device present; a NIC-less boot logs it and
    // continues unchanged, and the IRQ is unmasked and the ARP self-test
    // runs only when one came up.
    netdev_init();

    // Initialize core services
    splash_stage("capabilities + quantum resources", 60);
    core_services_init();

    // Spawn demo kernel threads and attach the scheduler
    splash_stage("scheduler + services", 75);
    scheduler_subsystem_init();

    // Map the user memory window and spawn the user-mode processes
    splash_stage("user-space processes", 90);
    user_init();

    // Start the periodic timer and enable interrupts
    splash_stage("timer + interrupts", 98);
    pit_init(TIMER_DEFAULT_HZ);
    interrupt_enable(IRQ_BASE + IRQ_TIMER);
    interrupt_enable(IRQ_BASE + IRQ_KEYBOARD);
    // Only listen to a UART that exists — on serial-less hardware the
    // line floats and its handler would just poll a phantom port.
    if (console_com1_present()) {
        interrupt_enable(IRQ_BASE + IRQ_COM1);
    }
    // Unmask the NIC's (dynamically assigned) IRQ line if a NIC came up.
    // Its handler is routed from irq_handler's default case. On a PCI
    // line >= 8 the slave PIC's cascade (IRQ2) must also be live — it is
    // unmasked by pic_init at boot. The line comes from the bound driver,
    // whichever it is.
    if (netdev_present()) {
        interrupt_enable(IRQ_BASE + netdev_irq_line());
    }
    interrupt_enable_all();
    boot_log("Timer started, interrupts enabled");

    boot_log("Kernel initialization complete");
    splash_ready();

    // Text-mode boots hand the display from the splash to the scrolling
    // VGA console: on a machine with no serial port (epic #101) this is
    // the only way to SEE the shell. Framebuffer boots keep the live
    // field view; their text stays on COM1.
    if (!fb_available()) {
        vga_console_enable();
        boot_log("CONS: screen console active (VGA text 80x25)");
    }

    boot_log("QuantumOS ready");

    // Idle loop for the kernel process. Reaps terminated processes
    // (reclaiming their address-space frames) with interrupts disabled
    // so the pmm free path can't race a preemption, then sleeps until
    // the next tick.
    //
    // Under a GRUB/ISO framebuffer boot it also drains ghostd's latest
    // published field snapshot and redraws the live memory-field view, so the
    // picture ripples with REMEMBER/RECALL activity. The snapshot is copied out
    // under the same interrupts-disabled window (a bounded 256-byte copy) so a
    // concurrent SYS_FIELD_SNAPSHOT cannot tear it; the draw runs with
    // interrupts back on. On the -kernel/VGA path fb_available() is false and
    // this is skipped entirely, so the text boot is untouched.
    static int8_t field_buf[FIELD_SNAP_BYTES];
    while (1) {
        interrupt_disable_all();
        process_reap();
        int field_n = 0;
        bool have_field = fb_available() && field_snapshot_take(field_buf, &field_n);
        interrupt_enable_all();
        if (have_field) {
            fb_render_field(field_buf, field_n);
        }
        __asm__ volatile("hlt");
    }
}

// HAL initialization
static void hal_init(void) {
    current_boot_state = BOOT_STATE_HAL_INIT;
    boot_log("Initializing HAL...");

    // TODO: Initialize hardware abstraction layer
    // - CPU detection
    // - Feature detection
    // - Basic hardware setup

    boot_log("HAL initialization complete");
}

// Memory management initialization
static void memory_subsystem_init(void) {
    current_boot_state = BOOT_STATE_MEMORY_INIT;
    boot_log("Initializing memory management...");

    // Call the real memory_init from memory.h
    mem_result_t result = memory_init(boot_config.total_memory);
    if (result != MEM_SUCCESS) {
        boot_log("Warning: Memory init returned non-success");
    }

    boot_log("Memory management initialization complete");
}

// Interrupt system initialization
static void interrupts_subsystem_init(void) {
    current_boot_state = BOOT_STATE_INTERRUPTS_INIT;
    boot_log("Initializing interrupt system...");

    // Call the real interrupts_init from interrupts.h
    irq_result_t result = interrupts_init();
    if (result != IRQ_SUCCESS) {
        boot_log("Warning: Interrupts init returned non-success");
    }

    boot_log("Interrupt system initialization complete");
}

// Core services initialization
static void core_services_init(void) {
    current_boot_state = BOOT_STATE_CORE_SERVICES;
    boot_log("Initializing core services...");

    // Initialize capability system first — process creation grants
    // each process its root capability
    if (cap_init() != CAP_SUCCESS) {
        boot_panic("Failed to initialize capability system");
    }
    if (cap_selftest() != CAP_SUCCESS) {
        boot_panic("Capability self-test failed");
    }

    // Intent-manifest self-test (epic #135): on the shipped system caps and
    // manifests are minted from the same grants, so no citizen ever trips
    // the MDENY path — this drives the real check helper to a deny and a
    // quota refusal at every boot, recording the AUDIT_MDENY/AUDIT_QUOTA
    // entries the CI gate parses. A constant-true check cannot ship green.
    if (manifest_selftest() != 0) {
        boot_panic("Manifest self-test failed");
    }

    // Initialize quantum resource manager (before processes — quantum-
    // aware processes are granted pool capabilities at creation)
    if (quantum_init() != QUANTUM_SUCCESS) {
        boot_panic("Failed to initialize quantum resource manager");
    }
    if (quantum_selftest() != QUANTUM_SUCCESS) {
        boot_panic("Quantum self-test failed");
    }

    // Initialize process management system
    process_subsystem_init();

    // Frame-allocator rover gate (hot-path optimization): the O(1)-amortized
    // search rover must still hand out only distinct, genuinely-free frames and
    // never lose one. A hint/wrap bug would corrupt memory or exhaust the pmm;
    // this self-test asserts the alloc/free invariants before the boot leans on
    // the allocator for every page table and spawn.
    if (pmm_alloc_selftest() != 0) {
        boot_panic("frame-allocator rover self-test failed");
    }
    boot_log("PMMROVER: frame allocator distinct + leak-free under the search rover");

    // Heap-reservation gate (ADR-0021): the kernel heap's backing frames must be
    // reserved in the PMM so the allocator can never hand a live-heap frame to a
    // page table or ELF segment. Reverting the reservation trips this before
    // "QuantumOS ready".
    if (pmm_heap_reservation_selftest() != 0) {
        boot_panic("PMM heap-reservation self-test failed");
    }
    boot_log("PMMHEAP: heap range reserved; allocator never hands a heap frame");

    // Physical-ceiling clamp gate (ADR-0021): every frame the PMM can hand out
    // must sit below the 1 GB identity map (a >= 1 GB frame is unmappable — a
    // #158-class escalation). This synthetic self-test feeds the clamp a > 1 GB
    // frame count and asserts it clamps to exactly PMM_MAX_FRAMES, so the clamp
    // is proven load-bearing even on the -m 128M leg; reverting it panics here.
    if (pmm_clamp_selftest() != 0) {
        boot_panic("PMM 1GB-clamp self-test failed");
    }
    boot_log("PMMCLAMP: frame count clamps to the 1 GB identity ceiling");

    // High-memory reachability gate (ADR-0021 PR-4): on RAM above 128 MB, prove a
    // top-of-pool frame is not just counted free but actually REACHABLE through
    // the boot.S 1 GB identity map (sentinel writeback), so dynamic sizing can
    // hand out high frames safely. On the -m 128M leg there is no high frame and
    // it reports the skip (the -m 256M/512M CI legs exercise the live path).
    if (pmm_highmem_selftest() != 0) {
        boot_panic("PMM high-memory reachability self-test failed");
    }

    // Residency-storm gate (ADR-0021 PR-4): extend the heap-reservation proof to a
    // bulk drain — a 1024-frame storm aimed at the heap must never surrender a
    // reserved frame. Emits PMMSTORM; reverting the heap reservation trips it.
    if (pmm_residency_storm_selftest() != 0) {
        boot_panic("PMM residency-storm self-test failed");
    }

    // Boot-validation gate (ADR-0021): the kernel is Multiboot1-only; the MB2
    // magic must be refused (a v2 info block parsed as v1 loses the cmdline and
    // wild-writes the framebuffer). Driven with a dummy info_addr since no shipped
    // loader exercises the MB2 branch.
    if (boot_validate_selftest() != 0) {
        boot_panic("boot-validation self-test failed");
    }
    boot_log("MB2REJECT: multiboot2 magic refused, multiboot1 accepted");

    // ELF-loader hardening gate (adversarial bug-hunt of the proc/mem core):
    // three malformed spawns must be REJECTED with zero frame leak. Runs before
    // the scheduler starts (all paths return before finalize_user_process). A
    // constant-true guard cannot ship green: without the loader fixes this leaks
    // frames / spawns off an out-of-bounds read and the boot never reaches
    // "QuantumOS ready".
    if (elf_spawn_selftest() != 0) {
        boot_panic("ELF-loader hardening self-test failed");
    }
    boot_log("ELFGUARD: malformed spawn rejected, no frame leak");

    // Spawn address-space-leak gate (cross-cutting bug-hunt, resource-leak
    // class): a spawn that builds a full private address space then fails at
    // process_create (ring-3 reachable by filling the process table) must
    // reclaim it. Without finalize_user_process's vmspace_destroy on that path,
    // the self-test sees a frame leak and the boot panics here.
    if (spawn_leak_selftest() != 0) {
        boot_panic("spawn address-space-leak self-test failed");
    }
    boot_log("SPAWNLEAK: failed spawn reclaimed address space (no leak)");

    // Argv-residue disclosure gate (cross-cutting bug-hunt, uninit-copyout
    // class): the reused command-line parse buffer must not carry a prior
    // spawn's argv into a later child's readable args page. Without the
    // parse_cmdline whole-struct zero the self-test finds the residue and the
    // boot panics here.
    if (spawn_argv_leak_selftest() != 0) {
        boot_panic("argv-residue disclosure self-test failed");
    }
    boot_log("ARGVLEAK: reused spawn buffer carries no stale argv residue");

    // DNS answer-binding gate (adversarial bug-hunt of the untrusted-input
    // surface): dns_parse must reject a spoofed-source or wrong-port DNS reply,
    // not just an unmatched txid — otherwise an on-link node poisons every
    // ring-3 SYS_RESOLVE. Pure in-memory parser test (no NIC), so it gates the
    // default boot. Without the binding checks the spoofed replies are accepted
    // and the self-test fails before "QuantumOS ready".
    if (net_dns_guard_selftest() != 0) {
        boot_panic("DNS answer-binding self-test failed");
    }
    boot_log("DNSGUARD: spoofed DNS answer rejected (src+port bound)");

    // Initialize IPC system
    ipc_subsystem_init();

    // Initialize the service manager and bring up system services
    // (they run once the scheduler and timer start)
    if (service_manager_init() != SVC_SUCCESS) {
        boot_panic("Failed to initialize service manager");
    }
    if (service_selftest() != SVC_SUCCESS) {
        boot_panic("Service framework self-test failed");
    }

    boot_log("Core services initialization complete");
}

// IPC subsystem initialization
static void ipc_subsystem_init(void) {
    boot_log("Initializing IPC subsystem...");

    ipc_result_t result = ipc_init();
    if (result != IPC_SUCCESS) {
        boot_panic("Failed to initialize IPC subsystem");
        return;
    }

    boot_log("IPC subsystem initialized");
}

// Process subsystem initialization
static void process_subsystem_init(void) {
    boot_log("Initializing process subsystem...");

    status_t result = process_init();
    if (result != STATUS_SUCCESS) {
        boot_panic("Failed to initialize process subsystem");
        return;
    }

    boot_log("Process subsystem initialized");
}

// ============================================================================
// Demo kernel threads — visible proof that preemptive scheduling works
// ============================================================================

static volatile uint64_t alpha_wakeups = 0;
static volatile uint64_t beta_wakeups = 0;

static void demo_thread_alpha(void) {
    boot_log("kernel-thread alpha: online");
    while (1) {
        __asm__ volatile("hlt");
        if (++alpha_wakeups % 200 == 0) {
            boot_log_v("kernel-thread alpha: alive");
        }
    }
}

static void demo_thread_beta(void) {
    boot_log("kernel-thread beta: online");
    while (1) {
        __asm__ volatile("hlt");
        if (++beta_wakeups % 200 == 0) {
            boot_log_v("kernel-thread beta: alive");
        }
    }
}

// Network self-test thread (epic #73): runs once interrupts are live so
// the NIC RX IRQ can fire, ARP-resolves the user-net gateway, then idles.
// A no-op on the NIC-less default boot.
static void net_thread(void) {
    net_init();
    net_selftest();
    // Then serve ring-3 resolve requests forever (SYS_RESOLVE). Network
    // I/O must live here, in an IF=1 thread, because a syscall runs with
    // interrupts disabled and could never pump the NIC's RX interrupt.
    net_service_loop();
}

// Scheduler + demo thread initialization
static void scheduler_subsystem_init(void) {
    boot_log("Initializing scheduler...");

    if (kernel_thread_create("alpha", demo_thread_alpha, PRIORITY_NORMAL, NULL) != STATUS_SUCCESS) {
        boot_log("Warning: failed to create kernel thread alpha");
    }
    if (kernel_thread_create("beta", demo_thread_beta, PRIORITY_NORMAL, NULL) != STATUS_SUCCESS) {
        boot_log("Warning: failed to create kernel thread beta");
    }
    if (netdev_present()) {
        if (kernel_thread_create("net", net_thread, PRIORITY_NORMAL, NULL) != STATUS_SUCCESS) {
            boot_log("Warning: failed to create kernel thread net");
        }
    }

    scheduler_init();

#ifdef SCHED_RESONANT
    /* Honest, deterministic measurement of the #21 hypothesis: run one
     * identical canned workload under round-robin and under the resonant
     * policy and print both fairness numbers + the order parameter r, before
     * the live system starts. A loss ships in the log, unedited. */
    boot_log("Resonant scheduler ACTIVE (SCHED_RESONANT) — measuring vs round-robin");
    resonant_fixed_boot_report();
#endif
}

// ============================================================================
// Boot command-line parsing — quantum entropy handoff (qseed=<hex>)
// ============================================================================

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Quiet-boot flag (cmdline token `quiet`) — see boot_is_quiet().
static int g_boot_quiet = 0;

int boot_is_quiet(void) {
    return g_boot_quiet;
}

// Agent-demo flag (cmdline token `agentdemo`): opt-in for the heavyweight
// agent-native end-to-end showcase (user/agentd.c). Off by default so it never
// perturbs the timing-sensitive CI gates or a plain boot; the default ci-smoke
// and the GRUB "agent-native demo" entry pass the token. Decoupled from `quiet`
// so the showcase can run on a CLEAN (quiet) console.
static int g_boot_agent_demo = 0;

int boot_run_agent_demo(void) {
    return g_boot_agent_demo;
}

// Is the whole-word token `word` present in `cmd` (space/tab/start bounded on
// the left, space/tab/NUL bounded on the right)? Avoids matching `quiet` inside
// a longer token like `disquiet=1`.
static int cmdline_has_word(const char *cmd, const char *word) {
    for (const char *p = cmd; *p; p++) {
        if (p != cmd && p[-1] != ' ' && p[-1] != '\t') {
            continue; // not at a token boundary
        }
        int k = 0;
        while (word[k] && p[k] == word[k]) {
            k++;
        }
        if (word[k] == '\0' && (p[k] == '\0' || p[k] == ' ' || p[k] == '\t')) {
            return 1;
        }
    }
    return 0;
}

/* Decode `<key>A.B.C.D` at a whitespace token boundary into out[4].
 * Returns 1 on a well-formed dotted quad, 0 otherwise (epic #97). */
static int cmdline_get_ipv4(const char *cmd, const char *key, uint8_t out[4]) {
    for (const char *p = cmd; *p; p++) {
        if (p != cmd && p[-1] != ' ' && p[-1] != '\t') {
            continue;
        }
        int k = 0;
        while (key[k] && p[k] == key[k]) {
            k++;
        }
        if (key[k] != '\0') {
            continue;
        }
        const char *h = p + k;
        int oi = 0, val = -1;
        for (;;) {
            char c = *h;
            if (c >= '0' && c <= '9') {
                val = (val < 0 ? 0 : val) * 10 + (c - '0');
                if (val > 255) {
                    return 0;
                }
                h++;
            } else if (c == '.' || c == '\0' || c == ' ' || c == '\t') {
                if (val < 0 || oi >= 4) {
                    return 0;
                }
                out[oi++] = (uint8_t)val;
                val = -1;
                if (c != '.') {
                    return oi == 4;
                }
                h++;
            } else {
                return 0;
            }
        }
    }
    return 0;
}

/* Parse a comma-separated peer= list (epic #139 N-way society):
 * "peer=10.0.0.2,10.0.0.3" -> net_add_peer_ip per dotted quad. cmdline_get_ipv4
 * cannot be reused (it is key-anchored and hard-rejects ','), so this walks the
 * quads itself. A single peer=10.0.0.2 yields one peer (the 2-VM path); a
 * trailing comma or a malformed quad simply ends the list. net_add_peer_ip is
 * internally bounded, so an over-long list cannot overrun. */
static void cmdline_get_peers(const char *cmd) {
    const char *key = "peer=";
    for (const char *p = cmd; *p; p++) {
        if (p != cmd && p[-1] != ' ' && p[-1] != '\t') {
            continue;
        }
        int k = 0;
        while (key[k] && p[k] == key[k]) {
            k++;
        }
        if (key[k] != '\0') {
            continue;
        }
        const char *h = p + k;
        uint8_t octets[4];
        int oi = 0, val = -1;
        for (;;) {
            char c = *h;
            if (c >= '0' && c <= '9') {
                val = (val < 0 ? 0 : val) * 10 + (c - '0');
                if (val > 255) {
                    return; /* malformed — stop the list */
                }
                h++;
            } else if (c == '.') {
                if (val < 0 || oi >= 3) {
                    return;
                }
                octets[oi++] = (uint8_t)val;
                val = -1;
                h++;
            } else if (c == ',' || c == '\0' || c == ' ' || c == '\t') {
                /* finalize one quad: the last octet comes from val */
                if (oi == 3 && val >= 0) {
                    octets[3] = (uint8_t)val;
                    net_add_peer_ip(octets);
                }
                if (c != ',') {
                    return; /* end of the peer= token */
                }
                oi = 0;
                val = -1;
                h++;
            } else {
                return; /* junk — stop */
            }
        }
    }
}

// Read a little-endian value from a possibly-misaligned untrusted address. The
// multiboot mmap entries carry 64-bit base/length at +4/+12, so byte-wise reads
// avoid an unaligned access and don't assume the mmap buffer is aligned.
static uint32_t mb_rd32(uintptr_t p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t mb_rd64(uintptr_t p) {
    return (uint64_t)mb_rd32(p) | ((uint64_t)mb_rd32(p + 4) << 32);
}

// Parse total usable physical RAM (bytes) from the Multiboot1 info block. This is
// UNTRUSTED bootloader input: every field is bounds-checked, the entry walk is
// termination- and overflow-guarded, and a garbage or absent map falls back to
// mem_upper (or panics if neither is present) rather than sizing a wild bitmap.
// Returns the highest available-region end, clamped to the 1 GB identity ceiling
// (pmm_init clamps again at frame granularity). MUST run only after
// boot_validate_multiboot has confirmed the Multiboot1 magic.
static uint64_t multiboot_parse_memory(uint32_t info_addr) {
    const uint32_t *info = (const uint32_t *)(uintptr_t)info_addr;
    uint32_t flags = info[0];
    uint64_t best_end = 0; // highest available-region end, clamped to the ceiling

    if (flags & 0x40) {                  // bit 6: full memory map present
        uint32_t mmap_length = info[11]; // bytes, at offset 44
        uint32_t mmap_addr = info[12];   // at offset 48
        uint64_t win_end = (uint64_t)mmap_addr + (uint64_t)mmap_length;
        // The whole buffer must sit inside the identity map with no overflow: this
        // is the first attacker-pointed memory we dereference.
        if (mmap_addr != 0 && mmap_length >= 24 && win_end > (uint64_t)mmap_addr &&
            win_end <= PMM_PHYS_CEIL) {
            uintptr_t cursor = mmap_addr;
            uintptr_t end = (uintptr_t)win_end;
            int iters = 0;
            while (cursor + 4 <= end && iters < 1024) {
                uint32_t size = mb_rd32(cursor); // entry size, EXCLUDES this u32
                if (size < 20) {
                    break; // too small to hold base/length/type, or no progress
                }
                uintptr_t next = cursor + (uintptr_t)size + 4;
                if (next <= cursor || next > end) {
                    break; // overflow or walk past the buffer
                }
                uint64_t base = mb_rd64(cursor + 4);
                uint64_t length = mb_rd64(cursor + 12);
                uint32_t type = mb_rd32(cursor + 20);
                if (type == 1) { // available RAM
                    uint64_t rend = base + length;
                    if (rend >= base) { // no overflow
                        if (rend > PMM_PHYS_CEIL) {
                            rend = PMM_PHYS_CEIL;
                        }
                        if (rend > best_end) {
                            best_end = rend; // ceiling = MAX end, never a sum
                        }
                    }
                }
                cursor = next;
                iters++;
            }
        }
    }

    if (best_end == 0 && (flags & 0x1)) { // fallback: mem_upper (KB above 1 MB)
        best_end = 0x100000ULL + ((uint64_t)info[2] << 10);
        if (best_end > PMM_PHYS_CEIL) {
            best_end = PMM_PHYS_CEIL;
        }
    }

    if (best_end == 0) {
        boot_panic("PMM: multiboot reported no usable memory (no mmap, no mem_upper)");
    }
    return best_end;
}

// Match "qseed=" at p; on hit, decode up to 16 hex digits into a u64. Also
// scans for the bare `quiet` token (clean interactive console).
static void parse_boot_cmdline(uint32_t info_addr) {
    uint32_t *info = (uint32_t *)(uintptr_t)info_addr;
    // Multiboot v1: flags bit 2 => cmdline valid; cmdline ptr at word 4
    if (!(info[0] & 0x4)) {
        return;
    }
    const char *cmd = (const char *)(uintptr_t)info[4];
    if (!cmd) {
        return;
    }

    if (cmdline_has_word(cmd, "quiet")) {
        g_boot_quiet = 1;
    }
    if (cmdline_has_word(cmd, "agentdemo")) {
        g_boot_agent_demo = 1;
    }

    /* Static IP + coupling peer (epic #97): `ip=A.B.C.D` configures a
     * static address and skips DHCP (the enabler for two guests on a raw
     * L2); `peer=A.B.C.D` names the field-coupling partner. Both are
     * matched only at a whitespace token boundary so they can never trip
     * on a substring, and both share the dotted-quad decoder below. */
    uint8_t octets[4];
    if (cmdline_get_ipv4(cmd, "ip=", octets)) {
        net_set_static_ip(octets);
        boot_log("NET: static ip configured from cmdline (ip=)");
    }
    /* Parse the (possibly comma-separated) peer= list directly — do NOT gate on
     * cmdline_get_ipv4, which hard-rejects the ',' in a multi-peer list. */
    cmdline_get_peers(cmd);
    if (net_get_peer_count() > 0) {
        boot_log("NET: coupling peer(s) configured from cmdline (peer=)");
    }

    static const char key[] = "qseed=";
    for (const char *p = cmd; *p; p++) {
        // match key
        int k = 0;
        while (key[k] && p[k] == key[k])
            k++;
        if (key[k] != '\0') {
            continue;
        }
        const char *h = p + k;
        uint64_t seed = 0;
        int digits = 0;
        while (digits < 16 && hex_val(*h) >= 0) {
            seed = (seed << 4) | (uint64_t)hex_val(*h);
            h++;
            digits++;
        }
        if (digits > 0) {
            quantum_set_boot_entropy(seed);
            boot_log("Boot entropy accepted from cmdline (qseed=)");
            boot_log("  qseed value: ");
            early_console_write_hex(seed);
        }
        return;
    }
}

// Boot validation
bool boot_validate_multiboot(uint32_t magic, uint32_t info_addr) {
    /* The kernel carries only a Multiboot v1 header (boot.S), so ONLY the v1
     * bootloader magic is accepted. A Multiboot2 magic (MULTIBOOT2_MAGIC) is
     * refused on purpose: every consumer of info_addr parses the v1 fixed layout
     * (cmdline word, framebuffer offset 88, ...), so treating a v2 info block as
     * v1 would silently drop the cmdline and wild-write the framebuffer. Re-accept
     * v2 only once a real MB2 tag parser exists. See ADR-0021.
     * This function is a PURE predicate — it must never dereference info_addr, so
     * boot_validate_selftest() can call it with a dummy address. */
    if (magic != MULTIBOOT1_MAGIC) {
        return false;
    }

    if (info_addr == 0) {
        return false;
    }

    // TODO: More thorough validation — keep the no-deref contract (or update the
    // dummy info_addr in boot_validate_selftest) if this ever inspects info_addr.
    return true;
}

// Boot-validation gate (ADR-0021): the Multiboot2-magic rejection lives on a
// branch no shipped loader exercises (QEMU/GRUB always hand us MB1 0x2BADB002),
// so it cannot be proven by "the machine booted". Drive the real predicate with a
// NON-ZERO dummy info_addr — boot_validate_multiboot never dereferences it — so
// the MB2 case isn't masked by the info_addr==0 short-circuit (calling it with 0
// would return false even if MB2 were still accepted: a fake-green on revert). The
// MB1-accept assertion is the vacuity self-check. Returns 0 on pass.
static int boot_validate_selftest(void) {
    if (!boot_validate_multiboot(MULTIBOOT1_MAGIC, 0x1000)) {
        return -1; /* vacuity: a real MB1 handoff must still be accepted */
    }
    if (boot_validate_multiboot(MULTIBOOT2_MAGIC, 0x1000)) {
        return -2; /* the fix: an MB2 magic must be refused */
    }
    if (boot_validate_multiboot(MULTIBOOT1_MAGIC, 0)) {
        return -3; /* a null info block is still invalid */
    }
    return 0;
}

// Boot logging
void boot_log(const char *message) {
    early_console_write("[BOOT] ");
    early_console_write(message);
    early_console_write("\r\n");
    // Tee to the screen console (no-op until enabled): on serial-less
    // real hardware this is the only place the kernel log is visible.
    if (vga_console_active()) {
        vga_console_puts("[BOOT] ");
        vga_console_puts(message);
        vga_console_putc('\n');
    }
}

// Steady-state / demo boot logging — silenced under a `quiet` boot. See boot.h.
void boot_log_v(const char *message) {
    if (!g_boot_quiet) {
        boot_log(message);
    }
}

// boot_panic is provided by boot.S (assembly implementation)

// Early console initialization
void early_console_init(void) {
    // Initialize COM1 serial port (0x3F8)
    // TODO: Proper serial port initialization
}

// Utility functions
void *memset(void *ptr, int value, size_t num) {
    uint8_t *p = (uint8_t *)ptr;
    while (num--) {
        *p++ = (uint8_t)value;
    }
    return ptr;
}

void *memcpy(void *dest, const void *src, size_t num) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (num--) {
        *d++ = *s++;
    }
    return dest;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (*str++) {
        len++;
    }
    return len;
}
