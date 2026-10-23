/**
 * QuantumOS System Call Implementation
 *
 * int 0x80 gate (DPL 3) dispatched through the common interrupt path;
 * the handler reads arguments from — and writes results into — the
 * saved cpu_state_t frame. Yield and exit reuse the scheduler's
 * frame-swap machinery, so returning from the syscall may resume a
 * different process.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/syscall.h>
#include <kernel/audit.h>
#include <kernel/manifest.h>
#include <kernel/qpu.h>
#include <kernel/syscall_abi.h> /* ring-crossing _k_t twins (ADR-0020) */
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/gdt.h>
#include <kernel/vmspace.h>
#include <kernel/memory.h>
#include <kernel/elf.h>
#include <kernel/capability.h>
#include <kernel/quantum.h>
#include <kernel/ipc.h>
#include <kernel/service.h>
#include <kernel/com2_uart.h>
#include <kernel/console.h>
#include <kernel/vga.h>
#include <kernel/initrd.h>
#include <kernel/ramfs.h>
#include <kernel/ata.h>
#include <kernel/net.h>
#include <kernel/rtc.h>
#include <kernel/field.h>
#include <kernel/boot.h>

/* Embedded user programs (kernel/src/user_blob.S) */
extern const uint8_t user_hello_start[], user_hello_end[];
extern const uint8_t user_canary_start[], user_canary_end[];
extern const uint8_t user_rogue_start[], user_rogue_end[];

/* Embedded ELF externs + the citizen roster (user_*_init, user_init) live in
 * citizens.c (the boot-wiring split). */

/* Argument vector ABI (epic #62). MUST stay byte-identical to user_args_t
 * in user/usys.h — there is no shared header across the ring boundary. The
 * kernel fills a copy of this and maps it read-only at USER_ARGS_VADDR into
 * the new process; get_args() on the user side reads it. */
#define KARGS_MAX 8
#define KARGS_STRBYTES 480
typedef struct {
    int argc;
    unsigned argv_off[KARGS_MAX];
    char strings[KARGS_STRBYTES];
} kuser_args_t;

static status_t finalize_user_process(address_space_t *as, const char *name, uint64_t entry,
                                      const kuser_args_t *kargs, uint32_t *pid_out);
static status_t spawn_elf_args(const char *name, const void *elf_start, const void *elf_end,
                               const kuser_args_t *kargs, uint32_t *pid_out);
/* user_*_init citizen roster: declared in kernel/syscall.h, defined in citizens.c. */

/* int 0x80 stub (kernel/src/interrupts.S) */
extern void isr128(void);

/* ============================================================================
 * Helpers
 * ============================================================================ */

/* Validate that [uptr, uptr+len) is mapped USER (and writable when write) in the
 * CALLER's address space, walking its page tables (#158). Returns 0 (caller must
 * return EFAULT) for any unmapped/read-only/out-of-range span, so a bad user
 * pointer never faults in ring 0. virtual_address_space is the raw PML4 base
 * (a uint64_t*, stored as as->pml4), NOT an address_space_t. */
static int user_ok(uint64_t uptr, uint64_t len, int write) {
    process_t *cur = process_get_current();
    if (!cur || !cur->virtual_address_space) {
        return 0; /* safe EFAULT during the create->finalize window, never a NULL walk */
    }
    return vmspace_user_ok((const uint64_t *)cur->virtual_address_space, uptr, len, write != 0);
}

/* Read a NUL-terminated user string into kbuf (page-incremental): validate the
 * page about to be touched, copy up to its end scanning for NUL, and only
 * validate/cross into the NEXT page if no NUL and budget remains. Never
 * over-reads an unmapped page (each is validated first) and never over-rejects a
 * short string near a mapped-region boundary (a max-window check would). Returns
 * the length copied (excluding NUL); over-length TRUNCATES to kmax-1 (the
 * original per-byte loops' behaviour), and only a genuine page fault returns -1.
 * kbuf is always NUL-terminated. */
static long copy_user_string(uint64_t uptr, char *kbuf, uint32_t kmax) {
    uint32_t n = 0;
    while (n < kmax - 1) {
        if (!user_ok(uptr + n, 1, 0)) {
            return -1; /* unmapped page — genuine fault */
        }
        uint64_t page_end = ((uptr + n) | 0xFFFULL); /* last byte of the current page */
        const char *src = (const char *)(uptr);
        while (n < kmax - 1 && (uptr + n) <= page_end) {
            char c = src[n];
            kbuf[n] = c;
            n++;
            if (c == '\0') {
                return (long)(n - 1);
            }
        }
    }
    kbuf[kmax - 1] = '\0';
    return (long)(kmax - 1); /* over-length: truncate (matches the old loops) */
}

/* Two-layer authority gate for a specific resource (epic #135): the
 * capability layer first (a missing cap records AUDIT_DENY), then the intent
 * manifest (a held-but-undeclared cap records AUDIT_MDENY inside
 * manifest_check). Returns 1 if the op is authorised by BOTH layers, 0
 * otherwise — the caller returns SYSCALL_EPERM on 0. On the shipped system
 * caps and manifests are minted from the same grants, so layer 2 refuses
 * nothing today; it is the outer bound cap delegation (a follow-up) will be
 * checked against. Consolidates the audit_deny hook the epic #133 sites had
 * with the audit gap ones (fs/net) so every gated site now records a DENY. */
static int authorize(uint32_t pid, uint32_t rtype, uint32_t perm, uint32_t rid) {
    if (cap_find_resource(pid, rtype, perm, rid) != CAP_SUCCESS) {
        audit_deny(pid, rtype, rid, perm);
        return 0;
    }
    return manifest_check(pid, rtype, rid, perm);
}

/* Print "[user pid=N] msg" atomically enough for the boot console.
 * Tees to the VGA screen console (no-op until enabled): on serial-less
 * hardware SYS_WRITE was invisible — the third real-laptop finding was
 * citizens running to a clean exit 0 with nobody able to see their
 * output. The pid keeps the serial log's 16-digit hex form on screen
 * too, so the two transcripts stay grep-compatible. */
static void user_console_write(uint32_t pid, const char *msg) {
    /* PER-LINE prefixing (epic #135): re-emit the prefix after every run of
     * CR/LF so no citizen byte can ever land at column 0 of the serial log.
     * Without this, one SYS_WRITE containing "\n MANIFEST:..." forges a line
     * the host's anchored ^AUDIT:/^MANIFEST:/^FIELDINFO: parsers ingest as
     * kernel ground truth — the documented #133 residual, closed here for
     * every current and future anchored marker at once. (All shipped
     * citizens emit single-line writes; their output is byte-identical.) */
    const char *p = msg;
    do {
        char seg[128]; /* sys_write's buf is 128 — a segment never exceeds it */
        size_t n = 0;
        while (*p && *p != '\r' && *p != '\n' && n < sizeof(seg) - 1) {
            seg[n++] = *p++;
        }
        seg[n] = '\0';
        while (*p == '\r' || *p == '\n') {
            p++; /* swallow the newline run; we emit our own CRLF */
        }
        early_console_write("[user pid=");
        early_console_write_hex(pid);
        early_console_write("] ");
        early_console_write(seg);
        early_console_write("\r\n");
    } while (*p);
    if (vga_console_active()) {
        uint64_t p = pid; /* widen BEFORE shifting: >>60 on a u32 is UB */
        char hex[17];
        for (int i = 0; i < 16; i++) {
            uint8_t nib = (uint8_t)((p >> (60 - 4 * i)) & 0xF);
            hex[i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
        }
        hex[16] = '\0';
        vga_console_puts("[user pid=");
        vga_console_puts(hex);
        vga_console_puts("] ");
        vga_console_puts(msg);
        vga_console_putc('\n');
        vga_console_sync();
    }
}

/* ============================================================================
 * Syscall implementations
 * ============================================================================ */

static uint64_t sys_write(uint32_t pid, uint64_t user_ptr) {
    /* Copy the NUL-terminated string out of the caller's user memory,
     * page-incrementally validating each page before touching it (#158). */
    char buf[128];
    long i = copy_user_string(user_ptr, buf, sizeof(buf));
    if (i < 0) {
        return SYSCALL_EFAULT;
    }
    user_console_write(pid, buf);
    return (uint64_t)i;
}

/* Exit ledger (epic #62 phase 3): the idle-loop reaper memsets a
 * terminated PCB long before a shell polls for it, so exit codes are
 * also recorded in this small ring. SYS_WAITPID consults the ledger
 * first (newest wins, so a recycled pid still resolves to the exit the
 * waiter is after) and the live table second. Single CPU, syscalls run
 * cli'd — no locking needed. */
#define EXIT_LEDGER_SIZE 16
static struct {
    uint32_t pid;
    int32_t code;
    bool valid;
} exit_ledger[EXIT_LEDGER_SIZE];
static uint32_t exit_ledger_next;

static void exit_ledger_record(uint32_t pid, int32_t code) {
    exit_ledger[exit_ledger_next].pid = pid;
    exit_ledger[exit_ledger_next].code = code;
    exit_ledger[exit_ledger_next].valid = true;
    exit_ledger_next = (exit_ledger_next + 1) % EXIT_LEDGER_SIZE;
}

static void sys_exit(uint32_t pid, uint64_t code, cpu_state_t *state) {
    process_t *cur = process_get_by_pid(pid);
    if (cur) {
        cur->exit_code = (int32_t)code;
        cur->has_exited = true;
    }
    exit_ledger_record(pid, (int32_t)code);
    boot_log_v("syscall: user process exited");
    process_set_state(pid, PROCESS_STATE_TERMINATED);
    scheduler_kill_current(state);
}

/* Capability-routed send: the caller may only transmit to whatever
 * destination it holds an IPC send-capability for (capability-as-
 * address). No such capability -> EPERM. */
static uint64_t sys_send(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    if (len == 0 || len > IPC_MAX_MESSAGE_SIZE) {
        return SYSCALL_EINVAL;
    }
    if (!user_ok(user_ptr, len, 0)) {
        return SYSCALL_EFAULT;
    }

    uint32_t dest = 0;
    if (cap_find(pid, CAP_RESOURCE_IPC, CAP_WRITE, &dest) != CAP_SUCCESS) {
        /* Record the ownership denial so a citizen probing IPC caps it does not
         * hold is visible in the authority ledger (ADR-0009 deny-coverage gap).
         * First-match: no specific destination, so resource_id = ANY. */
        audit_deny(pid, CAP_RESOURCE_IPC, AUDIT_RESOURCE_ANY, CAP_WRITE);
        return SYSCALL_EPERM;
    }

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    const uint8_t *src = (const uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && n < IPC_MAX_MESSAGE_SIZE) {
        msg.data[n] = src[n];
        n++;
    }
    msg.length = n;

    if (ipc_send(dest, &msg, 0) != IPC_SUCCESS) {
        return SYSCALL_EIO;
    }
    return 0;
}

/* Targeted capability-routed send: like sys_send, but the destination is
 * named explicitly (dest pid) instead of taken first-match. The caller must
 * hold an IPC send-capability whose resource_id is exactly `dest` — the same
 * capability-as-address rule, just choosing which held address to use. This
 * lets a service reply to whichever client sent it a request (the sender pid
 * recv returns) while still only being able to reach peers it was granted. */
static uint64_t sys_send_to(uint32_t pid, uint64_t dest, uint64_t user_ptr, uint64_t len) {
    if (len == 0 || len > IPC_MAX_MESSAGE_SIZE) {
        return SYSCALL_EINVAL;
    }
    if (!user_ok(user_ptr, len, 0)) {
        return SYSCALL_EFAULT;
    }

    if (cap_find_resource(pid, CAP_RESOURCE_IPC, CAP_WRITE, (uint32_t)dest) != CAP_SUCCESS) {
        /* Record the ownership denial (ADR-0009 deny-coverage gap): a targeted
         * send to an address the caller holds no cap for now appears in the
         * authority ledger, naming the exact destination it tried to reach. */
        audit_deny(pid, CAP_RESOURCE_IPC, (uint32_t)dest, CAP_WRITE);
        return SYSCALL_EPERM;
    }

    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    const uint8_t *src = (const uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && n < IPC_MAX_MESSAGE_SIZE) {
        msg.data[n] = src[n];
        n++;
    }
    msg.length = n;

    if (ipc_send((uint32_t)dest, &msg, 0) != IPC_SUCCESS) {
        return SYSCALL_EIO;
    }
    return 0;
}

/* Receive from the caller's own queue (no capability needed to drain
 * your own mailbox). Returns the sender pid, or 0 if empty. */
static uint64_t sys_recv(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    /* Validate the destination span BEFORE ipc_receive dequeues: the message is
     * CONSUMED on receive, so a range failure discovered mid-copy would
     * silently lose it (the QPU/field validate-before-mutate discipline). The
     * copy writes at most min(len, IPC_MAX_MESSAGE_SIZE) bytes plus a NUL, so
     * both ends of that span must be in the user range. */
    /* copy-OUT: validate the writable span (+1 for the trailing NUL) as MAPPED
     * and WRITABLE before ipc_receive dequeues (#158 + the consume-before-lose
     * guard). */
    uint64_t span = len < (uint64_t)IPC_MAX_MESSAGE_SIZE ? len : (uint64_t)IPC_MAX_MESSAGE_SIZE;
    if (!user_ok(user_ptr, span + 1, 1)) {
        return SYSCALL_EFAULT;
    }

    ipc_message_t msg;
    uint32_t sender = IPC_PID_ANY;
    if (ipc_receive(&sender, &msg, 0) != IPC_SUCCESS) {
        return 0; /* nothing queued */
    }

    uint8_t *dst = (uint8_t *)user_ptr;
    uint32_t n = 0;
    while (n < len && n < msg.length) {
        dst[n] = msg.data[n];
        n++;
    }
    /* NUL-terminate if room (n <= span, pre-validated in range). */
    if (n < len) {
        dst[n] = 0;
    }
    return msg.sender_id;
}

/* Capability-gated quantum randomness. Draws up to QRAND_MAX_BYTES bytes
 * from the kernel's qseed-mixed generator into the caller's buffer; the
 * caller must hold a CAP_RESOURCE_QUANTUM read capability or the draw is
 * denied (EPERM) before any byte is produced. len == 0 is a provenance
 * query returning whether a boot qseed was mixed in (still cap-gated). */
static uint64_t sys_qrand(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    uint8_t seed_present = 0;

    if (len == 0) {
        /* Provenance query: no buffer touched. Still requires the cap so a
         * capless caller cannot even learn the entropy provenance. The
         * capless denial now records an AUDIT_DENY over QRNG (epic #135) —
         * the ledger's "quantum" DENY coverage was CLAIMED by audit.h but
         * never actually hooked; ghost_test's capless draw now proves it. */
        if (quantum_user_random(pid, NULL, 0, &seed_present) != QUANTUM_SUCCESS) {
            audit_deny(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID, CAP_QUANTUM | CAP_READ);
            return SYSCALL_EPERM;
        }
        if (!manifest_check(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID,
                            CAP_QUANTUM | CAP_READ)) {
            return SYSCALL_EPERM;
        }
        return seed_present ? 1 : 0;
    }

    if (len > QRAND_MAX_BYTES) {
        len = QRAND_MAX_BYTES;
    }
    if (!user_ok(user_ptr, len, 1)) { /* copy-OUT, validated after the clamp */
        return SYSCALL_EFAULT;
    }

    /* Draw into a kernel buffer under the capability check, then copy out. */
    uint8_t tmp[QRAND_MAX_BYTES];
    if (quantum_user_random(pid, tmp, (uint32_t)len, &seed_present) != QUANTUM_SUCCESS) {
        audit_deny(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID, CAP_QUANTUM | CAP_READ);
        return SYSCALL_EPERM;
    }
    /* Held the cap — now the intent bound (epic #135). Checked before the
     * drawn bytes reach user memory. */
    if (!manifest_check(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID,
                        CAP_QUANTUM | CAP_READ)) {
        return SYSCALL_EPERM;
    }

    uint8_t *dst = (uint8_t *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        dst[n] = tmp[n];
    }
    return len;
}

/* Capability-gated COM2 serial pipe (backs SYS_COM2). The COM2 swarm-bridge
 * UART is a guarded device resource: the caller must hold a CAP_RESOURCE_DEVICE
 * capability over DEVICE_ID_COM2 carrying CAP_WRITE (to transmit) or CAP_READ
 * (to receive). Bytes are bounced through a bounded kernel buffer so the driver
 * never touches user pointers directly, and the copy is clamped to the caller's
 * mapped user half exactly like the other syscalls. Read is non-blocking: it
 * returns however many bytes the UART had waiting (0 if none). */
static uint64_t sys_com2(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    if (op != SYS_COM2_READ && op != SYS_COM2_WRITE) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (len > COM2_MAX_BYTES) {
        len = COM2_MAX_BYTES;
    }

    uint8_t tmp[COM2_MAX_BYTES];

    if (op == SYS_COM2_WRITE) {
        if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_COM2)) {
            return SYSCALL_EPERM;
        }
        if (!user_ok(user_ptr, len, 0)) { /* copy-IN */
            return SYSCALL_EFAULT;
        }
        const uint8_t *src = (const uint8_t *)user_ptr;
        for (uint32_t n = 0; n < len; n++) {
            tmp[n] = src[n];
        }
        return com2_write(tmp, (uint32_t)len);
    }

    /* SYS_COM2_READ */
    if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_COM2)) {
        return SYSCALL_EPERM;
    }
    /* copy-OUT: validate the writable span BEFORE draining the UART (a
     * post-drain fault would lose the drained bytes). */
    if (!user_ok(user_ptr, len, 1)) {
        return SYSCALL_EFAULT;
    }
    uint32_t got = com2_read(tmp, (uint32_t)len);
    uint8_t *dst = (uint8_t *)user_ptr;
    for (uint32_t n = 0; n < got; n++) {
        dst[n] = tmp[n];
    }
    return got;
}

/* Capability-gated interactive console (backs SYS_CONS, epic #62 phase 1).
 * The console is a guarded device resource exactly like the COM2 bridge: the
 * caller must hold a CAP_RESOURCE_DEVICE capability over DEVICE_ID_CONSOLE
 * carrying CAP_READ (to drain the input ring) or CAP_WRITE (to emit raw
 * bytes). Reads are non-blocking; writes are raw — no "[user pid]" prefix —
 * so the shell owns its own line discipline. Bytes bounce through a bounded
 * kernel buffer, clamped to the caller's mapped user half as usual. */
static uint64_t sys_cons(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    if (op != SYS_CONS_READ && op != SYS_CONS_WRITE) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (len > CONS_MAX_BYTES) {
        len = CONS_MAX_BYTES;
    }

    uint8_t tmp[CONS_MAX_BYTES];

    if (op == SYS_CONS_WRITE) {
        if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_CONSOLE)) {
            return SYSCALL_EPERM;
        }
        if (!user_ok(user_ptr, len, 0)) { /* copy-IN */
            return SYSCALL_EFAULT;
        }
        const uint8_t *src = (const uint8_t *)user_ptr;
        for (uint32_t n = 0; n < len; n++) {
            tmp[n] = src[n];
        }
        return console_write(tmp, (uint32_t)len);
    }

    /* SYS_CONS_READ */
    if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_CONSOLE)) {
        return SYSCALL_EPERM;
    }
    /* copy-OUT: validate before draining the input ring (post-drain fault
     * would lose the drained bytes). */
    if (!user_ok(user_ptr, len, 1)) {
        return SYSCALL_EFAULT;
    }
    const char *seed_notice = quantum_take_reseed_notice();
    if (seed_notice) {
        boot_log(seed_notice);
    }
    uint32_t got = console_read(tmp, (uint32_t)len);
    scheduler_console_reader_note(pid, process_get_generation(pid), got);
    uint8_t *dst = (uint8_t *)user_ptr;
    for (uint32_t n = 0; n < got; n++) {
        dst[n] = tmp[n];
    }
    return got;
}

/* Append a decimal u64 to buf at offset o (bounded by max). */
static size_t fmt_dec(char *buf, size_t o, size_t max, uint64_t v) {
    char t[20];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v && n < (int)sizeof(t)) {
        t[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && o + 1 < max) {
        buf[o++] = t[--n];
    }
    return o;
}

static size_t fmt_str(char *buf, size_t o, size_t max, const char *s) {
    while (*s && o + 1 < max) {
        buf[o++] = *s++;
    }
    return o;
}

/* Live introspection text (backs SYS_SYSINFO). Uncapped read-only reporting,
 * like SYS_GETPID/SYS_TICKS: it names no authority. The kernel formats the
 * text so the user side needs no struct ABI — SYSINFO_PS is one "PS: pid
 * name STATE" line per live process (formatted by process.c, which owns the
 * table), SYSINFO_MEM is a single "MEM: ..." stats line. */
static uint64_t sys_sysinfo(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    /* SYSINFO_QUIET is a bare boolean query (no buffer): returns 1 if the
     * kernel booted `quiet`, so a chatty service can silence its steady-state
     * console logging. Answered before the buffer checks. */
    if (op == SYSINFO_QUIET) {
        return (uint64_t)boot_is_quiet();
    }
    /* SYSINFO_PEER (epic #97; N-way #139): the packed `peer=` IP at index
     * `user_ptr` (0 = out of range). Bare query, no buffer — grants no
     * authority, just config a coupling service reads to learn who to talk to.
     * A legacy caller passing index 0 gets peer[0], unchanged. */
    if (op == SYSINFO_PEER) {
        return (uint64_t)net_get_peer_at((int)user_ptr);
    }
    /* SYSINFO_PEER_COUNT (epic #139): how many peers were configured. Bare
     * query — answered here BEFORE the len==0 guard below. */
    if (op == SYSINFO_PEER_COUNT) {
        return (uint64_t)(int64_t)net_get_peer_count();
    }
    if (len == 0) {
        return 0;
    }

    /* Static bounce buffer: syscalls run cli'd on one CPU, so this cannot
     * be re-entered while in use. */
    static char tmp[SYSINFO_MAX_BYTES];
    size_t produced = 0;

    if (op == SYSINFO_PS) {
        produced = process_format_ps(tmp, sizeof(tmp));
    } else if (op == SYSINFO_TIME) {
        produced = rtc_format(tmp, sizeof(tmp));
    } else if (op == SYSINFO_MEM) {
        size_t o = 0;
        o = fmt_str(tmp, o, sizeof(tmp), "MEM: heap free=");
        o = fmt_dec(tmp, o, sizeof(tmp), kheap_free_bytes());
        o = fmt_str(tmp, o, sizeof(tmp), " bytes, frames free=");
        o = fmt_dec(tmp, o, sizeof(tmp), pmm_get_free_frames());
        o = fmt_str(tmp, o, sizeof(tmp), "/");
        o = fmt_dec(tmp, o, sizeof(tmp), pmm_get_total_frames());
        o = fmt_str(tmp, o, sizeof(tmp), "\r\n");
        produced = o;
    } else if (op == SYSINFO_SCHED) {
        /* ADR-0022 prereq-2 scheduler baseline, one atomic line (this syscall runs
         * cli'd on one CPU). `now` is read ONCE so ticks= and the fairness gap base
         * are coherent. switches= is total context switches (yield-dominated),
         * preempt= is timer-quantum expiries only (the quantum-sensitive metric),
         * maxgap/spread/runnable are the fairness/tail snapshot. All read-only. */
        uint64_t now = timer_get_ticks();
        uint64_t max_gap = 0, spread = 0;
        uint32_t runnable = 0;
        scheduler_get_fairness(now, &max_gap, &spread, &runnable);
        size_t o = 0;
        o = fmt_str(tmp, o, sizeof(tmp), "SCHED: switches=");
        o = fmt_dec(tmp, o, sizeof(tmp), scheduler_get_switches());
        o = fmt_str(tmp, o, sizeof(tmp), " preempt=");
        o = fmt_dec(tmp, o, sizeof(tmp), scheduler_get_preempts());
        o = fmt_str(tmp, o, sizeof(tmp), " ticks=");
        o = fmt_dec(tmp, o, sizeof(tmp), now);
        o = fmt_str(tmp, o, sizeof(tmp), " maxgap=");
        o = fmt_dec(tmp, o, sizeof(tmp), max_gap);
        o = fmt_str(tmp, o, sizeof(tmp), " spread=");
        o = fmt_dec(tmp, o, sizeof(tmp), spread);
        o = fmt_str(tmp, o, sizeof(tmp), " runnable=");
        o = fmt_dec(tmp, o, sizeof(tmp), runnable);
        /* boost= APPENDED LAST (ADR-0022 I/O boost): honored-boost picks.
         * Trailing so every existing prefix-matched parser keeps working;
         * the latency gate asserts a positive delta across its ping batch. */
        o = fmt_str(tmp, o, sizeof(tmp), " boost=");
        o = fmt_dec(tmp, o, sizeof(tmp), scheduler_get_boosts());
        o = fmt_str(tmp, o, sizeof(tmp), "\r\n");
        produced = o;
    } else {
        return SYSCALL_EINVAL;
    }

    if (len > produced) {
        len = produced;
    }
    if (!user_ok(user_ptr, len, 1)) { /* copy-OUT, validated after the clamp */
        return SYSCALL_EFAULT;
    }
    char *dst = (char *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        dst[n] = tmp[n];
    }
    return len;
}

/* ---- Read-only VFS over the embedded initrd (epic #62 phase 2, #64) ----
 *
 * Four calls: open/read/close/readdir. The backing store is the ustar
 * archive baked into the kernel image — public, read-only data — so the
 * calls are uncapped like SYS_SYSINFO (per-file capabilities are future
 * work). File state lives in the caller's PCB fd table and dies with it. */

/* Copy a NUL-terminated path out of user memory, bounded. Returns 0 on
 * success, -1 if the pointer or termination is bad. */
static int copy_user_path(uint64_t user_ptr, char *dst, size_t max) {
    /* Page-incremental string copy (#158): -1 only on a genuine fault; an
     * over-length path truncates, exactly as the old loop did. */
    return copy_user_string(user_ptr, dst, (uint32_t)max) < 0 ? -1 : 0;
}

/* Grab a free fd slot in `cur` and fill it. Returns the fd or EIO. */
/* Lowest free fd slot in cur's table, or -1 if the table is full. Lets the
 * write path RESERVE-check the fd before mutating the overlay (see sys_open). */
static int find_free_fd(process_t *cur) {
    for (uint32_t fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (!cur->fds[fd].used) {
            return (int)fd;
        }
    }
    return -1;
}

static uint64_t install_fd(process_t *cur, const uint8_t *data, uint32_t size, bool writable,
                           int16_t ram_idx) {
    int fd = find_free_fd(cur);
    if (fd < 0) {
        return SYSCALL_EIO; /* fd table full */
    }
    cur->fds[fd].data = data;
    cur->fds[fd].size = size;
    cur->fds[fd].offset = 0;
    cur->fds[fd].used = true;
    cur->fds[fd].writable = writable;
    cur->fds[fd].ram_idx = ram_idx;
    return (uint64_t)fd;
}

/* Does the caller hold the volume-level filesystem-write authority?
 * (CAP_RESOURCE_DEVICE over DEVICE_ID_DISK — writing files is writing
 * the persistence volume; per-file capabilities remain future work.)
 * Now two-layer (epic #135): capability + intent manifest, and it records
 * an AUDIT_DENY on a capless attempt where the epic #133 sites already did
 * but the fs sites did not — closing the documented ledger gap for DISK. */
static int has_fs_write_cap(uint32_t pid) {
    return authorize(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_DISK);
}

static uint64_t sys_open(uint32_t pid, uint64_t path_ptr, uint64_t flags) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur) {
        return SYSCALL_EINVAL;
    }
    if (flags & ~(uint64_t)(O_WRONLY | O_CREAT | O_TRUNC)) {
        return SYSCALL_EINVAL;
    }

    char path[VFS_PATH_MAX];
    if (copy_user_path(path_ptr, path, sizeof(path)) != 0) {
        return SYSCALL_EFAULT;
    }

    /* ---- Write path (epic #71): create-or-append on the RAM overlay,
     * gated on the filesystem-write capability BEFORE any effect. ---- */
    if (flags & (O_WRONLY | O_CREAT | O_TRUNC)) {
        if (!(flags & O_WRONLY) || !(flags & O_CREAT)) {
            return SYSCALL_EINVAL; /* write combos require O_WRONLY|O_CREAT */
        }
        if (!has_fs_write_cap(pid)) {
            return SYSCALL_EPERM;
        }
        /* Reserve an fd BEFORE mutating the overlay: a full fd table must fail
         * up front, never AFTER ramfs_create has truncated an existing file to
         * zero (data loss) or created+marked a new slot (a phantom file that
         * persists to disk) — a half-failure the caller reads as "open failed,
         * file untouched". Single-CPU cli'd, so the slot stays free until the
         * install_fd below claims it. */
        if (find_free_fd(cur) < 0) {
            return SYSCALL_EIO;
        }

        int idx;
        const uint8_t *rdata = NULL;
        uint32_t rsize = 0;
        if (!(flags & O_TRUNC) && ramfs_lookup(path, &rdata, &rsize) >= 0) {
            /* Existing overlay file, appending. */
            idx = ramfs_lookup(path, &rdata, &rsize);
        } else {
            idx = ramfs_create(path); /* creates, or truncates existing */
            if (idx < 0) {
                return SYSCALL_EIO; /* table full / name too long / OOM */
            }
        }

        const uint8_t *d = NULL;
        uint32_t sz = 0;
        ramfs_get(idx, NULL, &d, &sz);
        return install_fd(cur, d, sz, true, (int16_t)idx);
    }

    /* ---- Read path: the overlay shadows the initrd. ---- */
    const uint8_t *data = NULL;
    uint32_t size = 0;
    int ridx = ramfs_lookup(path, &data, &size);
    if (ridx >= 0) {
        return install_fd(cur, data, size, false, (int16_t)ridx);
    }
    if (initrd_lookup(path, &data, &size) != 0) {
        return SYSCALL_ENOENT;
    }
    return install_fd(cur, data, size, false, -1);
}

/* Append through a write-opened fd (epic #71). The write authority was
 * capability-checked at open; the fd carries it. */
static uint64_t sys_fwrite(uint32_t pid, uint64_t fd, uint64_t user_ptr, uint64_t len) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur || fd >= PROCESS_MAX_FDS || !cur->fds[fd].used || !cur->fds[fd].writable ||
        cur->fds[fd].ram_idx < 0) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }

    /* Bounce through a bounded kernel buffer like every other copy-in. */
    uint8_t tmp[CONS_MAX_BYTES];
    if (len > sizeof(tmp)) {
        len = sizeof(tmp);
    }
    if (!user_ok(user_ptr, len, 0)) { /* copy-IN, validated after the clamp */
        return SYSCALL_EFAULT;
    }
    const uint8_t *src = (const uint8_t *)user_ptr;
    for (uint32_t i = 0; i < len; i++) {
        tmp[i] = src[i];
    }

    int wrote = ramfs_append(cur->fds[fd].ram_idx, tmp, (uint32_t)len);
    if (wrote < 0) {
        return SYSCALL_EINVAL;
    }
    return (uint64_t)wrote;
}

/* Remove an overlay file (epic #71). Refused while any live process
 * holds it open — the fd's data snapshot would dangle. */
static uint64_t sys_unlink(uint32_t pid, uint64_t path_ptr) {
    if (!has_fs_write_cap(pid)) {
        return SYSCALL_EPERM;
    }

    char path[VFS_PATH_MAX];
    if (copy_user_path(path_ptr, path, sizeof(path)) != 0) {
        return SYSCALL_EFAULT;
    }

    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (ramfs_lookup(path, &data, &size) < 0) {
        return SYSCALL_ENOENT;
    }
    if (process_any_fd_references(data)) {
        return SYSCALL_EIO; /* still open somewhere — refuse */
    }
    if (ramfs_unlink(path) != 0) {
        return SYSCALL_ENOENT;
    }
    return 0;
}

/* Flush the overlay to disk (epic #71 phase 3). */
static uint64_t sys_sync_fs(uint32_t pid) {
    if (!has_fs_write_cap(pid)) {
        return SYSCALL_EPERM;
    }
    int n = persist_sync();
    if (n < 0) {
        return SYSCALL_EIO;
    }
    return (uint64_t)n;
}

/* Resolve a hostname to an IPv4 address (epic #73 follow-up). Real
 * authority — using the shared NIC — so it is capability-gated on a
 * CAP_RESOURCE_DEVICE over DEVICE_ID_NET, held only by qsh. The lookup
 * itself runs in the IF=1 net thread (a cli'd syscall can't pump the RX
 * IRQ); this call posts the request on the first invocation and polls on
 * the rest, so the caller loops with SYS_YIELD. */
static uint64_t sys_resolve(uint32_t pid, uint64_t host_ptr, uint64_t out_ptr) {
    if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_NET)) {
        return SYSCALL_EPERM;
    }
    if (!user_ok(out_ptr, 4, 1)) { /* copy-OUT: the 4-byte resolved IP */
        return SYSCALL_EFAULT;
    }

    char host[64];
    if (copy_user_path(host_ptr, host, sizeof(host)) != 0) {
        return SYSCALL_EFAULT;
    }

    /* Poll first: if a result for the in-flight request is ready, deliver
     * it. A fresh (idle) state returns 0 here, so we then post below. */
    uint8_t ip[4];
    int r = net_poll_resolve(ip);
    if (r == 1) {
        uint8_t *dst = (uint8_t *)out_ptr;
        for (int i = 0; i < 4; i++) {
            dst[i] = ip[i];
        }
        return 0;
    }
    if (r < 0) {
        return SYSCALL_EIO; /* the lookup failed */
    }

    /* No NIC at all is a hard failure; a NIC that is still bringing its
     * DHCP lease up is transient — keep polling until the net thread is
     * ready and services the request. */
    if (!net_nic_present()) {
        return SYSCALL_EIO;
    }

    /* Pending or idle: (re)post the request. net_request_resolve returns
     * busy while one is already in flight or the lease isn't up yet, so
     * re-posting the same host each poll is harmless. */
    net_request_resolve(host);
    return RESOLVE_WOULDBLOCK;
}

/* SYS_UDP (epic #80): ring-3 UDP sockets, op-multiplexed over a request
 * struct (usys wrappers cap out at 3 registers — the socketcall
 * pattern). The layout must match user/usys.h's udp_req_t exactly:
 * 24 bytes, buf at offset 16, no padding either side. */
/* udp_req_k_t: defined in <kernel/syscall_abi.h>. */

/* Map a net_udp_* result onto the syscall ABI. */
static uint64_t udp_map_err(long r) {
    switch (r) {
    case NET_UDP_EINVAL:
        return SYSCALL_EINVAL;
    case NET_UDP_EPERM:
        return SYSCALL_EPERM;
    case NET_UDP_EAGAIN:
        return RESOLVE_WOULDBLOCK;
    case NET_UDP_ENONET:
        return SYSCALL_EIO;
    default:
        return (uint64_t)r;
    }
}

static uint64_t sys_udp(uint32_t pid, uint64_t op, uint64_t req_ptr) {
    /* Authority FIRST — before any argument or network check — so the
     * capless-denial gate fires even in the NIC-less default boot (the
     * sys_resolve precedent). Two-layer since epic #135. */
    if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_NET)) {
        return SYSCALL_EPERM;
    }
    /* The WHOLE struct must be mapped (copy-IN here; RECVFROM re-validates it
     * write=1 before writing the sender back). */
    if (!user_ok(req_ptr, sizeof(udp_req_k_t), 0)) {
        return SYSCALL_EFAULT;
    }
    udp_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }

    switch (op) {
    case UDP_OP_BIND:
        return udp_map_err(net_udp_bind(pid, req.port));

    case UDP_OP_SENDTO: {
        if (req.len > UDP_PAYLOAD_MAX) {
            return SYSCALL_EINVAL;
        }
        if (!net_udp_dst_ok(req.ip)) {
            return SYSCALL_EINVAL; /* fail fast — never ARP the unARPable */
        }
        /* req.buf is a second untrusted pointer (copy-IN, must be mapped). */
        if (req.len > 0 && !user_ok(req.buf, req.len, 0)) {
            return SYSCALL_EFAULT;
        }
        /* Bounce to kernel memory: syscalls run cli'd on one CPU (the
         * sys_readdir static-buffer justification), and the net thread
         * must never see a user pointer — it runs under its own CR3. */
        static uint8_t bounce[UDP_PAYLOAD_MAX];
        const uint8_t *ubuf = (const uint8_t *)req.buf;
        for (uint16_t i = 0; i < req.len; i++) {
            bounce[i] = ubuf[i];
        }
        return udp_map_err(net_udp_sendto(pid, (long)req.sock, req.ip, req.port, bounce, req.len));
    }

    case UDP_OP_RECVFROM: {
        uint16_t want = req.len > UDP_PAYLOAD_MAX ? UDP_PAYLOAD_MAX : req.len;
        /* Both destinations are copy-OUT — validate them WRITABLE up front,
         * before recvfrom, so neither the payload nor the sender writeback can
         * fault (or find a read-only page) after the receive commits. */
        if (want > 0 && !user_ok(req.buf, want, 1)) {
            return SYSCALL_EFAULT;
        }
        if (!user_ok(req_ptr, sizeof(udp_req_k_t), 1)) {
            return SYSCALL_EFAULT;
        }
        static uint8_t bounce[UDP_PAYLOAD_MAX];
        uint8_t sip[4];
        uint16_t sport = 0;
        long r = net_udp_recvfrom(pid, (long)req.sock, bounce, want, sip, &sport);
        if (r < 0) {
            return udp_map_err(r);
        }
        uint8_t *ubuf = (uint8_t *)req.buf;
        for (long i = 0; i < r; i++) {
            ubuf[i] = bounce[i];
        }
        /* Report the sender back through the (already validated) struct. */
        udp_req_k_t *ureq = (udp_req_k_t *)req_ptr;
        for (int i = 0; i < 4; i++) {
            ureq->ip[i] = sip[i];
        }
        ureq->port = sport;
        ureq->len = (uint16_t)r;
        return (uint64_t)r;
    }

    case UDP_OP_CLOSE:
        return udp_map_err(net_udp_close(pid, (long)req.sock));

    default:
        return SYSCALL_EINVAL;
    }
}

/* SYS_TCP (epic #82): ring-3 TCP client. Same request-struct ABI shape as
 * SYS_UDP (the `sock`/`ip`/`port`/`len`/`buf` layout); the single
 * connection means `sock` is unused. */
#define TCP_SEND_MAX 1460 /* == TCP_MSS */

static uint64_t tcp_map_err(long r) {
    switch (r) {
    case NET_TCP_EINVAL:
        return SYSCALL_EINVAL;
    case NET_TCP_EIO:
        return SYSCALL_EIO;
    case NET_TCP_WOULDBLOCK:
        return RESOLVE_WOULDBLOCK;
    case NET_TCP_ENONET:
        return SYSCALL_EIO;
    default:
        return (uint64_t)r;
    }
}

static uint64_t sys_tcp(uint32_t pid, uint64_t op, uint64_t req_ptr) {
    /* Authority FIRST (the sys_udp/sys_resolve precedent) so the capless
     * gate fires even in the NIC-less default boot. Two-layer since #135. */
    if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_READ, DEVICE_ID_NET)) {
        return SYSCALL_EPERM;
    }
    /* copy-IN only — TCP RECV has no struct writeback (unlike UDP RECVFROM). */
    if (!user_ok(req_ptr, sizeof(udp_req_k_t), 0)) {
        return SYSCALL_EFAULT;
    }
    udp_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }

    switch (op) {
    case TCP_OP_CONNECT:
        if (!net_udp_dst_ok(req.ip)) {
            return SYSCALL_EINVAL; /* same unARPable-destination guard as UDP */
        }
        return tcp_map_err(net_tcp_connect(pid, req.ip, req.port));

    case TCP_OP_SEND: {
        if (req.len > TCP_SEND_MAX) {
            return SYSCALL_EINVAL;
        }
        if (req.len > 0 && !user_ok(req.buf, req.len, 0)) { /* copy-IN */
            return SYSCALL_EFAULT;
        }
        static uint8_t bounce[TCP_SEND_MAX];
        const uint8_t *ubuf = (const uint8_t *)req.buf;
        for (uint16_t i = 0; i < req.len; i++) {
            bounce[i] = ubuf[i];
        }
        return tcp_map_err(net_tcp_send(pid, bounce, req.len));
    }

    case TCP_OP_RECV: {
        uint16_t want = req.len > TCP_SEND_MAX ? TCP_SEND_MAX : req.len;
        if (want > 0 && !user_ok(req.buf, want, 1)) { /* copy-OUT */
            return SYSCALL_EFAULT;
        }
        static uint8_t bounce[TCP_SEND_MAX];
        long r = net_tcp_recv(pid, bounce, want);
        if (r < 0) {
            return tcp_map_err(r);
        }
        uint8_t *ubuf = (uint8_t *)req.buf;
        for (long i = 0; i < r; i++) {
            ubuf[i] = bounce[i];
        }
        return (uint64_t)r; /* 0 = EOF */
    }

    case TCP_OP_CLOSE:
        return tcp_map_err(net_tcp_close(pid));

    case TCP_OP_STATUS:
        return tcp_map_err(net_tcp_status(pid));

    case TCP_OP_LISTEN:
        return tcp_map_err(net_tcp_listen(pid, req.port));

    case TCP_OP_ACCEPT:
        return tcp_map_err(net_tcp_accept(pid));

    default:
        return SYSCALL_EINVAL;
    }
}

static uint64_t sys_read(uint32_t pid, uint64_t fd, uint64_t user_ptr, uint64_t len) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur || fd >= PROCESS_MAX_FDS || !cur->fds[fd].used) {
        return SYSCALL_EINVAL;
    }
    if (len == 0) {
        return 0;
    }

    uint32_t remaining = cur->fds[fd].size - cur->fds[fd].offset;
    if (len > remaining) {
        len = remaining;
    }
    /* copy-OUT: validate the clamped span is mapped WRITABLE user memory. */
    if (!user_ok(user_ptr, len, 1)) {
        return SYSCALL_EFAULT;
    }

    const uint8_t *src = cur->fds[fd].data + cur->fds[fd].offset;
    uint8_t *dst = (uint8_t *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        dst[n] = src[n];
    }
    cur->fds[fd].offset += (uint32_t)len;
    return len;
}

static uint64_t sys_close(uint32_t pid, uint64_t fd) {
    process_t *cur = process_get_by_pid(pid);
    if (!cur || fd >= PROCESS_MAX_FDS || !cur->fds[fd].used) {
        return SYSCALL_EINVAL;
    }
    /* Reset EVERY field: a later open into this slot on the RO path sets
     * only data/size/offset — a lingering writable/ram_idx would make a
     * read-only reopen wrongly appear writable and target a stale file. */
    cur->fds[fd].used = false;
    cur->fds[fd].data = NULL;
    cur->fds[fd].size = 0;
    cur->fds[fd].offset = 0;
    cur->fds[fd].writable = false;
    cur->fds[fd].ram_idx = -1;
    return 0;
}

static uint64_t sys_readdir(uint32_t pid, uint64_t path_ptr, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    if (len == 0) {
        return 0;
    }

    char path[VFS_PATH_MAX];
    if (copy_user_path(path_ptr, path, sizeof(path)) != 0) {
        return SYSCALL_EFAULT;
    }

    /* Static bounce buffer: syscalls run cli'd on one CPU (same
     * justification as sys_sysinfo). Initrd rows first, then the RAM
     * overlay's (tagged [ram]) appended after them. */
    static char tmp[SYSINFO_MAX_BYTES];
    size_t produced = initrd_format_list(path, tmp, sizeof(tmp));
    produced = ramfs_format_list(path, tmp, sizeof(tmp), produced);

    if (len > produced) {
        len = produced;
    }
    /* copy-OUT: validate the clamped span is mapped WRITABLE user memory. */
    if (!user_ok(user_ptr, len, 1)) {
        return SYSCALL_EFAULT;
    }
    char *dst = (char *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        dst[n] = tmp[n];
    }
    return len;
}

/* ---- Program execution off the filesystem (epic #62 phase 3, #65) ----
 *
 * SYS_SPAWN loads a named initrd ELF into a fresh address space through
 * the same proven loader the boot-time services use. Unlike the VFS
 * reads, spawning is REAL authority — a process that can start programs
 * can multiply — so it is capability-gated: CAP_RESOURCE_PROCESS with
 * CAP_EXECUTE over SPAWN_RESOURCE_ID, declaratively granted to qsh
 * alone. The capless ghost-test proves the denial by attack every boot. */

/* Longest command line SYS_SPAWN copies from user memory (path + args). */
#define SPAWN_CMDLINE_MAX 256

/* Split `cmd` on runs of spaces/tabs into argv, packing the tokens into
 * `ka`. Returns the token count (>= 0). argv[0] is the first token — the
 * initrd path to load; it is also copied to `path0` (bounded by pmax). */
static int parse_cmdline(const char *cmd, kuser_args_t *ka, char *path0, size_t pmax) {
    /* Zero the WHOLE struct before the partial, field-by-field fill below. `ka`
     * is a reused static in sys_spawn, and finalize_user_process bulk-copies the
     * entire struct (sizeof) into the child's user-readable args page — so any
     * unwritten hole (the tail of strings[] past soff, argv_off[] past argc)
     * would otherwise copy the PRIOR spawn's argv residue into a later, unrelated
     * child, a cross-process disclosure of kernel .bss. */
    memset(ka, 0, sizeof(*ka));
    size_t soff = 0;
    const char *p = cmd;
    while (*p && ka->argc < KARGS_MAX) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        /* Record this token's start offset, copy until the next space. */
        unsigned start = (unsigned)soff;
        while (*p && *p != ' ' && *p != '\t' && soff + 1 < KARGS_STRBYTES) {
            char c = *p++;
            if (ka->argc == 0 && (size_t)(soff - start) < pmax - 1) {
                path0[soff - start] = c;
            }
            ka->strings[soff++] = c;
        }
        ka->strings[soff++] = '\0';
        if (ka->argc == 0) {
            size_t plen = (soff - 1) - start;
            path0[plen < pmax ? plen : pmax - 1] = '\0';
        }
        ka->argv_off[ka->argc] = start;
        ka->argc++;
        /* Skip any overflow of an over-long token. */
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return ka->argc;
}

/* Boot self-test for the argv-residue disclosure fix (cross-cutting bug-hunt,
 * uninit-copyout class). Parse a long command line carrying a distinctive secret
 * token, then a SHORT one into the same buffer; after the short parse NONE of the
 * secret may survive in the tail that finalize_user_process copies into the next
 * child's user-readable args page. Returns 0 on pass. Anti-vacuous: without the
 * whole-struct zero in parse_cmdline, the token persists in the reused buffer and
 * this returns negative -> the boot panics before "QuantumOS ready". */
int spawn_argv_leak_selftest(void) {
    static const char TOK[] = "ZZLEAKTOKENQ"; /* 12 bytes, unlikely to occur by chance */
    kuser_args_t ka;
    char path[VFS_PATH_MAX];

    parse_cmdline("/bin/a ZZLEAKTOKENQ", &ka, path, sizeof(path));
    parse_cmdline("/bin/b", &ka, path, sizeof(path)); /* the fix zeroes ka first */

    /* No trace of the secret may remain anywhere in the packed strings. */
    for (size_t i = 0; i + 12 <= KARGS_STRBYTES; i++) {
        int match = 1;
        for (size_t j = 0; j < 12; j++) {
            if (ka.strings[i + j] != TOK[j]) {
                match = 0;
                break;
            }
        }
        if (match) {
            return -1; /* stale argv residue would leak into the next child */
        }
    }
    /* argv_off holes past the short parse's argc must also be cleared. */
    for (int k = ka.argc; k < KARGS_MAX; k++) {
        if (ka.argv_off[k] != 0) {
            return -2;
        }
    }
    return 0;
}

static uint64_t sys_spawn(uint32_t pid, uint64_t cmd_ptr) {
    if (!authorize(pid, CAP_RESOURCE_PROCESS, CAP_EXECUTE, SPAWN_RESOURCE_ID)) {
        return SYSCALL_EPERM;
    }
    /* Spawn QUOTA precheck (epic #135) — the first ENFORCED quota. Runs
     * BEFORE any side effect (copy-in, parse, initrd lookup) so a failed
     * attempt never consumes a slot; a refusal records AUDIT_QUOTA. The
     * charge happens only after spawn_elf_args succeeds (below), and the
     * check-then-charge pair is race-free: syscalls run cli'd on one CPU. */
    if (!manifest_spawn_precheck(pid)) {
        return SYSCALL_EPERM;
    }
    /* Spawn-time parent<->child IPC channel opt-in (epic #175). Capacity is
     * verified BEFORE any side effect so the postcondition is all-or-nothing:
     * a returned pid ALWAYS carries a fully wired bidirectional channel —
     * never a half-minted one whose mute child times the demo out minutes
     * from the fault. Race-free: syscalls run cli'd on one CPU and the IF=1
     * reaper only FREES slots, so free capacity cannot shrink between this
     * check and the mints after spawn_elf_args. A refusal is audited so the
     * ledger explains the failure. */
    int spawn_channel = manifest_spawn_channel(pid);
    if (spawn_channel) {
        cap_stats_t cs;
        cap_get_stats(&cs);
        if (cs.active + 2 > MAX_CAPABILITIES) {
            audit_deny(pid, CAP_RESOURCE_IPC, AUDIT_RESOURCE_ANY, CAP_READ | CAP_WRITE);
            return SYSCALL_EIO;
        }
    }

    /* Copy the whole command line (path + args) out of user memory.
     * copy_user_string validates each page (present|user) before touching it,
     * so an in-range but unmapped cmd_ptr returns EFAULT instead of faulting
     * the kernel (issue #158). */
    char cmd[SPAWN_CMDLINE_MAX];
    if (copy_user_string(cmd_ptr, cmd, sizeof(cmd)) < 0) {
        return SYSCALL_EFAULT;
    }

    /* Parse into an argv (argv[0] = the initrd path to load). */
    static kuser_args_t ka; /* syscalls run cli'd on one CPU — not re-entered */
    char path[VFS_PATH_MAX];
    if (parse_cmdline(cmd, &ka, path, sizeof(path)) < 1) {
        return SYSCALL_EINVAL;
    }

    const uint8_t *data = NULL;
    uint32_t size = 0;
    if (initrd_lookup(path, &data, &size) != 0) {
        return SYSCALL_ENOENT;
    }

    /* Process name = the path's basename. */
    const char *name = path;
    for (const char *q = path; *q; q++) {
        if (*q == '/' && q[1]) {
            name = q + 1;
        }
    }

    uint32_t new_pid = 0;
    if (spawn_elf_args(name, data, data + size, &ka, &new_pid) != STATUS_SUCCESS) {
        return SYSCALL_EIO;
    }
    /* Mint the opted-in parent<->child channel (epic #175): a bidirectional
     * capability-checked IPC pair, tagged spawn-channel so the surviving half
     * UNLINKs when either end dies (process_destroy) — pids recycle first-fit,
     * so an untagged leftover would hand this parent a live channel into an
     * unrelated future process. Provably CAP_SUCCESS after the capacity
     * precheck above (cli'd syscall; the reaper only frees); guarded anyway —
     * an unwired child must never look wired. NO manifest_grant on either
     * side: IPC caps are pair-wise runtime wiring, deliberately OUTSIDE the
     * manifest (the epic #135 rule), and the derive peer check is cap-only. */
    if (spawn_channel) {
        uint32_t ch = CAP_ID_INVALID;
        if (cap_create(pid, CAP_RESOURCE_IPC, new_pid, CAP_READ | CAP_WRITE, 0, &ch) ==
            CAP_SUCCESS) {
            cap_mark_spawn_channel(ch);
        }
        if (cap_create(new_pid, CAP_RESOURCE_IPC, pid, CAP_READ | CAP_WRITE, 0, &ch) ==
            CAP_SUCCESS) {
            cap_mark_spawn_channel(ch);
        }
    }
    /* Charge the quota only on a real spawn (epic #135) — failed lookups /
     * loads above never reach here, so a typo'd `run /bin/nope` costs no
     * quota. */
    manifest_spawn_charge(pid);
    audit_spawn(pid, new_pid);
    return new_pid;
}

static uint64_t sys_waitpid(uint32_t pid, uint64_t target) {
    (void)pid;
    if (target >= MAX_PROCESSES) {
        return SYSCALL_EINVAL;
    }

    /* Ledger first, newest to oldest: a reaped (or recycled) pid still
     * resolves to the exit the waiter is polling for. */
    for (uint32_t i = 0; i < EXIT_LEDGER_SIZE; i++) {
        uint32_t idx = (exit_ledger_next + EXIT_LEDGER_SIZE - 1 - i) % EXIT_LEDGER_SIZE;
        if (exit_ledger[idx].valid && exit_ledger[idx].pid == (uint32_t)target) {
            return (uint64_t)(uint8_t)exit_ledger[idx].code;
        }
    }

    process_t *p = process_get_by_pid((uint32_t)target);
    if (p) {
        return WAITPID_RUNNING;
    }
    return SYSCALL_ENOENT;
}

/* Report the boot qseed (SYS_QSEED). Gated on the same quantum-pool read
 * capability as SYS_QRAND — a capless caller cannot even learn the seed. The
 * value lets a ring-3 attestation service bind its boot record to the exact
 * entropy the kernel accepted on the cmdline (0 = no qseed handoff). */
static uint64_t sys_qseed(uint32_t pid) {
    uint32_t rid = 0;
    if (cap_find(pid, CAP_RESOURCE_QUANTUM, CAP_READ, &rid) != CAP_SUCCESS ||
        rid != QUANTUM_POOL_RESOURCE_ID) {
        audit_deny(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID, CAP_READ);
        return SYSCALL_EPERM;
    }
    if (!manifest_check(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID, CAP_READ)) {
        return SYSCALL_EPERM;
    }
    return quantum_boot_seed();
}

/* Live memory-field visualization buffer. A ring-3 producer (ghostd) publishes
 * a downsampled snapshot of its field here via SYS_FIELD_SNAPSHOT; the
 * framebuffer renderer drains it in the idle loop. This is a display sink only
 * — it carries no capability and confers no authority, so it needs no cap
 * check (the worst a rogue writer can do is draw noise on a screen that only
 * exists under a GRUB/ISO boot). */
static int8_t g_field_snap[FIELD_SNAP_BYTES];
static int g_field_n = 0;
static volatile bool g_field_dirty = false;

static uint64_t sys_field_snapshot(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    if (len == 0) {
        return 0;
    }
    if (len > FIELD_SNAP_BYTES) {
        len = FIELD_SNAP_BYTES;
    }
    /* copy-IN: validate the clamped span is mapped user memory. */
    if (!user_ok(user_ptr, len, 0)) {
        return SYSCALL_EFAULT;
    }
    const int8_t *src = (const int8_t *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        g_field_snap[n] = src[n];
    }
    g_field_n = (int)len;
    g_field_dirty = true;
    return len;
}

bool field_snapshot_take(int8_t *dst, int *out_n) {
    if (!g_field_dirty) {
        return false;
    }
    int n = g_field_n;
    for (int i = 0; i < n; i++) {
        dst[i] = g_field_snap[i];
    }
    *out_n = n;
    g_field_dirty = false;
    return true;
}

/* ============================================================================
 * SYS_IMPRINT / SYS_RECALL — the holographic field as a kernel memory
 * primitive (epic #95). Region-scoped by CAP_RESOURCE_FIELD: the caller's
 * capability must name EXACTLY the region in the request. The capless
 * check runs before any user memory is read — a capless caller always
 * observes exactly EPERM (the ghost_test attack gate depends on it).
 * ============================================================================ */

static uint64_t sys_imprint(uint32_t pid, uint64_t req_ptr) {
    /* Capless callers are denied before ANY user memory is read. The
     * region id lives inside the request struct, so the specific-region
     * match runs after copy-in; this first check only asks "do you hold
     * any field-write capability at all?". */
    uint32_t any_region = 0;
    if (cap_find(pid, CAP_RESOURCE_FIELD, CAP_WRITE, &any_region) != CAP_SUCCESS) {
        audit_deny(pid, CAP_RESOURCE_FIELD, AUDIT_RESOURCE_ANY, CAP_WRITE);
        return SYSCALL_EPERM;
    }
    /* Whole-struct mapping+range validation (copy-IN, the sys_udp precedent). */
    if (!user_ok(req_ptr, sizeof(field_imprint_req_k_t), 0)) {
        return SYSCALL_EFAULT;
    }
    field_imprint_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }
    /* The cap must name EXACTLY the requested region (the sys_send_to
     * precedent): holding region 0 must never reach region 1 — this
     * comparison IS the isolation boundary. Two-layer since epic #135:
     * capability + intent manifest, so the field region is part of the
     * declared allow-set. */
    if (!authorize(pid, CAP_RESOURCE_FIELD, CAP_WRITE, req.region)) {
        return SYSCALL_EPERM;
    }
    if (req.len == 0 || req.len > FIELD_PAT_MAX) {
        return SYSCALL_EINVAL;
    }
    int64_t slot =
        field_imprint(req.region, req.pattern, req.len, req.energy_q15, timer_get_ticks());
    if (slot < 0) {
        return SYSCALL_EINVAL; /* degenerate (all-equal-bytes) pattern */
    }
    return (uint64_t)slot;
}

static uint64_t sys_recall(uint32_t pid, uint64_t req_ptr, uint64_t out_ptr) {
    uint32_t any_region = 0;
    if (cap_find(pid, CAP_RESOURCE_FIELD, CAP_READ, &any_region) != CAP_SUCCESS) {
        audit_deny(pid, CAP_RESOURCE_FIELD, AUDIT_RESOURCE_ANY, CAP_READ);
        return SYSCALL_EPERM;
    }
    /* copy-IN the request now; the output span is validated WRITABLE just
     * before the copy-out (both mappings are stable — syscalls run cli'd). */
    if (!user_ok(req_ptr, sizeof(field_recall_req_k_t), 0)) {
        return SYSCALL_EFAULT;
    }
    field_recall_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }
    if (!authorize(pid, CAP_RESOURCE_FIELD, CAP_READ, req.region)) {
        return SYSCALL_EPERM;
    }
    if (req.len == 0 || req.len > FIELD_PAT_MAX || req.k == 0 || req.k > FIELD_RANK_MAX) {
        return SYSCALL_EINVAL;
    }
    /* Retrieval reinforcement writes the energy landscape: apply it only
     * when the caller ALSO holds the write right — a read-only capability
     * must never mutate (adversarial-review commitment). */
    int reinforce =
        cap_find_resource(pid, CAP_RESOURCE_FIELD, CAP_WRITE, req.region) == CAP_SUCCESS;
    /* Compute into kernel memory, then one whole-struct copy-out of the
     * pre-validated span — the caller never sees a partially-written
     * result. Static is safe: syscalls run cli'd on one CPU (the
     * sys_readdir static-buffer justification). */
    static field_recall_out_k_t out;
    if (field_recall(req.region, req.probe, req.len, req.k, reinforce, &out, timer_get_ticks()) <
        0) {
        return SYSCALL_EINVAL;
    }
    if (!user_ok(out_ptr, sizeof(out), 1)) { /* copy-OUT: mapped WRITABLE */
        return SYSCALL_EFAULT;
    }
    uint8_t *udst = (uint8_t *)out_ptr;
    for (size_t i = 0; i < sizeof(out); i++) {
        udst[i] = ((const uint8_t *)&out)[i];
    }
    return 0;
}

/* SYS_FIELD_INFO — read-only enumeration of a field region (epic #127 B1). The
 * honest, non-mutating counterpart to SYS_RECALL: it reports live count,
 * capacity, and per-slot metadata WITHOUT reinforcing anything, so it is safe
 * under a READ-only capability (recall is the only writer). */
static uint64_t sys_field_info(uint32_t pid, uint64_t region_arg, uint64_t out_ptr) {
    /* Capless-first: a caller with no field-read capability at all observes
     * exactly EPERM before ANY user memory is touched (the ghost_test rule). */
    uint32_t any_region = 0;
    if (cap_find(pid, CAP_RESOURCE_FIELD, CAP_READ, &any_region) != CAP_SUCCESS) {
        audit_deny(pid, CAP_RESOURCE_FIELD, AUDIT_RESOURCE_ANY, CAP_READ);
        return SYSCALL_EPERM;
    }
    uint32_t region = (uint32_t)region_arg;
    /* The cap must name EXACTLY the requested region — checked BEFORE the
     * output pointer is examined, so a wrong-region caller also gets EPERM
     * without revealing anything about (or requiring) a valid buffer. */
    if (!authorize(pid, CAP_RESOURCE_FIELD, CAP_READ, region)) {
        return SYSCALL_EPERM;
    }
    /* Static is safe: syscalls run cli'd on one CPU (the sys_recall precedent).
     * field_region_info zeroes the whole struct before filling, so no residue
     * from a prior call's region survives into the copy-out. */
    static field_info_out_k_t out;
    if (field_region_info(region, &out, timer_get_ticks()) < 0) {
        return SYSCALL_EINVAL;
    }
    if (!user_ok(out_ptr, sizeof(out), 1)) { /* copy-OUT: mapped WRITABLE */
        return SYSCALL_EFAULT;
    }
    uint8_t *udst = (uint8_t *)out_ptr;
    for (size_t i = 0; i < sizeof(out); i++) {
        udst[i] = ((const uint8_t *)&out)[i];
    }
    return 0;
}

/* SYS_AUDIT — read the capability authority ledger (epic #133 Phase D). Uncapped
 * read-only introspection (the SYS_SYSINFO precedent: it names no authority, it
 * only reports). op AUDIT_OP_READ formats all live GRANT/DENY/SPAWN entries
 * oldest->newest; AUDIT_OP_STATS reports total/dropped/capacity. Reading the log
 * mints and denies nothing, so it never perturbs its own counters. */
static uint64_t sys_audit(uint32_t pid, uint64_t op, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    static char tmp[AUDIT_MAX_BYTES];
    size_t produced;
    if (op == AUDIT_OP_STATS) {
        produced = audit_format_stats(tmp, sizeof(tmp));
    } else if (op == AUDIT_OP_READ) {
        produced = audit_format(tmp, sizeof(tmp));
    } else {
        return SYSCALL_EINVAL;
    }
    if (len > produced) {
        len = produced;
    }
    /* copy-OUT: validate the clamped span is mapped WRITABLE user memory. */
    if (!user_ok(user_ptr, len, 1)) {
        return SYSCALL_EFAULT;
    }
    char *dst = (char *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        dst[n] = tmp[n];
    }
    return len;
}

/* SYS_MANIFEST — read the intent manifests (epic #135 Phase D increment 2).
 * Uncapped read-only introspection (the SYS_AUDIT/SYS_SYSINFO class): dumps
 * every BOUND per-pid manifest as text — the declared allow-set, the spawn
 * quota (used/max), and live cpu_ticks. Same disclosure stance as the audit
 * ledger for grants (already public there), with cpu_ticks the one added,
 * documented timing signal. Reading mutates nothing. Copy-out mirrors
 * sys_audit EXACTLY: format into a static kernel scratch, clamp the user
 * length to what was produced, then one user_ok(span, write) mapping check
 * before the copy — so a user-controlled length can neither wrap past the
 * user half nor fault the kernel on an unmapped page (#158). */
static uint64_t sys_manifest(uint32_t pid, uint64_t user_ptr, uint64_t len) {
    (void)pid;
    static char tmp[MANIFEST_TEXT_MAX];
    size_t produced = manifest_format(tmp, sizeof(tmp));
    if (len > produced) {
        len = produced;
    }
    /* copy-OUT: validate the clamped span is mapped WRITABLE user memory. */
    if (!user_ok(user_ptr, len, 1)) {
        return SYSCALL_EFAULT;
    }
    char *dst = (char *)user_ptr;
    for (uint32_t n = 0; n < len; n++) {
        dst[n] = tmp[n];
    }
    return len;
}

/* SYS_CAP_DERIVE — cross-ring capability delegation (epic #137 Phase D inc. 3).
 * A citizen holding CAP_GRANT hands a strictly-NARROWED slice of one of its own
 * capabilities to a sub-agent, and the delegation is bounded by AND reflected
 * in the intent manifest (#135): the delegator may only delegate a resource it
 * is itself declared to touch, and the derive EXTENDS the recipient's manifest
 * so the delegated cap is actually usable. Naming the parent by
 * (resource_type, resource_id) — never a ring-3 handle — keeps cap_ids out of
 * ring 3 and closes the forgery surface. Returns 0, or a negative errno.
 *
 * The request must match user/usys.h cap_derive_req_t byte-for-byte (24 B, no
 * shared header across the ring). */
/* cap_derive_req_k_t: defined in <kernel/syscall_abi.h>. */

static uint64_t cap_derive_errno(cap_result_t r) {
    switch (r) {
    case CAP_SUCCESS:
        return 0;
    case CAP_ERROR_NO_SPACE:
        return SYSCALL_EIO;
    default:
        return SYSCALL_EPERM; /* escalation / denied / not-owner / expired / invalid */
    }
}

static uint64_t sys_cap_derive(uint32_t pid, uint64_t req_ptr) {
    /* Whole-struct mapping+range validation (copy-IN, the sys_udp precedent). */
    if (!user_ok(req_ptr, sizeof(cap_derive_req_k_t), 0)) {
        return SYSCALL_EFAULT;
    }
    cap_derive_req_k_t req;
    const uint8_t *usrc = (const uint8_t *)req_ptr;
    for (size_t i = 0; i < sizeof(req); i++) {
        ((uint8_t *)&req)[i] = usrc[i];
    }

    /* A no-rights derive is meaningless — never touch the cap table. */
    if (req.permissions == 0) {
        return SYSCALL_EINVAL;
    }
    /* ONE-HOP: never hand over CAP_GRANT/CAP_REVOKE, so a sub-agent can never
     * itself re-delegate. Delegation is provably a single hop. */
    if (req.permissions & (CAP_GRANT | CAP_REVOKE)) {
        return SYSCALL_EPERM;
    }

    /* Target must be a live ring-3 process, not self, not the kernel/idle
     * (PROCESS_TYPE_USER excludes them). */
    if (req.target_pid == pid || req.target_pid == KERNEL_PROCESS_ID ||
        req.target_pid >= MAX_PROCESSES) {
        return SYSCALL_EPERM;
    }
    process_t *target = process_get_by_pid(req.target_pid);
    if (!target || target->type != PROCESS_TYPE_USER ||
        (target->state != PROCESS_STATE_RUNNING && target->state != PROCESS_STATE_READY)) {
        return SYSCALL_EPERM;
    }
    /* Refuse a MONITORED service target: its watchdog restart rebinds its
     * manifest and cascade-revokes the derived cap, so the delegation would
     * silently evaporate. */
    if (service_pid_is_monitored(req.target_pid)) {
        return SYSCALL_EPERM;
    }
    /* IPC-PEER requirement: the caller may only delegate to a pid it holds an
     * IPC send-cap for — so it can reach only peers it was explicitly wired to.
     * This is what stops a CAP_GRANT holder from injecting a cap into an
     * arbitrary victim (qsh/kannakad/etc, which it has no IPC cap for). */
    if (cap_find_resource(pid, CAP_RESOURCE_IPC, CAP_WRITE, req.target_pid) != CAP_SUCCESS) {
        audit_deny(pid, CAP_RESOURCE_IPC, req.target_pid, CAP_WRITE);
        return SYSCALL_EPERM;
    }
    /* Transitive INTENT bound: X may only delegate a resource X is itself
     * declared (manifest-allowed) to touch. Records MDENY on a miss. */
    if (!manifest_check(pid, req.resource_type, req.resource_id, req.permissions)) {
        return SYSCALL_EPERM;
    }
    /* Idempotent: if the target already holds a covering cap, succeed WITHOUT
     * minting another — bounds the shared cap table against a looping delegator
     * (the manifest row is already present too). */
    if (cap_find_resource(req.target_pid, req.resource_type, req.permissions, req.resource_id) ==
        CAP_SUCCESS) {
        return 0;
    }
    /* Resolve MY grantable parent by (rtype,rid) — need_perms includes CAP_GRANT
     * so a non-grant same-resource cap is never selected (which cap_derive would
     * then reject). This also proves I actually hold the resource I'm handing
     * over (not just a dangling manifest row). */
    uint32_t parent_id = CAP_ID_INVALID;
    if (cap_find_id(pid, req.resource_type, req.permissions | CAP_GRANT, req.resource_id,
                    &parent_id) != CAP_SUCCESS) {
        audit_deny(pid, req.resource_type, req.resource_id, req.permissions | CAP_GRANT);
        return SYSCALL_EPERM;
    }
    /* Target manifest must have room BEFORE we mint (>= guards the entries[]
     * bound) — so the manifest extend below cannot fail after a cap exists (no
     * undo path). Syscalls run cli'd on one CPU, so this check-then-act is
     * atomic vs the IF=1 health monitor. */
    if (!manifest_has_room(req.target_pid)) {
        return SYSCALL_EIO;
    }

    uint32_t child_id = CAP_ID_INVALID;
    cap_result_t r =
        cap_derive(parent_id, pid, req.target_pid, req.permissions, req.expiration, &child_id);
    if (r != CAP_SUCCESS) {
        return cap_derive_errno(r);
    }
    /* Extend the recipient's manifest so the delegated cap is USABLE (else
     * authorize() would MDENY it). Room was pre-checked, so this cannot fail. */
    manifest_grant(req.target_pid, req.resource_type, req.resource_id, req.permissions);
    return 0;
}

/* SYS_QPU — the QPU job broker (epic #148 A2+A3). Op-multiplexed like
 * SYS_COM2/SYS_AUDIT. The kernel is a broker, never a circuit parser: it
 * enforces the two-layer authority gate (capability + intent manifest) and the
 * qsub quota, copies OPAQUE payloads in/out of the fixed job table, and records
 * the exercised authority in the durable ledger. Request/result structs must
 * match user/usys.h byte-for-byte (twin _Static_asserts on both sides). */
/* qpu_submit_req_k_t / qpu_fetch_out_k_t / qpu_complete_req_k_t /
 * qpu_poll_out_k_t: defined in <kernel/syscall_abi.h>. */

/* Copy a whole struct in from ring 3 (mapping-validated: an in-range but
 * unmapped pointer returns 0/EFAULT rather than faulting the kernel — #158). */
static int qpu_copy_in(uint64_t uptr, void *dst, size_t n) {
    if (!user_ok(uptr, n, 0)) {
        return 0;
    }
    const uint8_t *usrc = (const uint8_t *)uptr;
    for (size_t i = 0; i < n; i++) {
        ((uint8_t *)dst)[i] = usrc[i];
    }
    return 1;
}

static int qpu_copy_out(uint64_t uptr, const void *src, size_t n) {
    if (!user_ok(uptr, n, 1)) { /* copy-OUT: mapped WRITABLE */
        return 0;
    }
    uint8_t *udst = (uint8_t *)uptr;
    for (size_t i = 0; i < n; i++) {
        udst[i] = ((const uint8_t *)src)[i];
    }
    return 1;
}

static uint64_t sys_qpu(uint32_t pid, uint64_t op, uint64_t arg, uint64_t arg2) {
    switch (op) {
    case QPU_OP_SUBMIT: {
        /* WRITE cap + intent gate first (records AUDIT_DENY/MDENY on a miss);
         * the broker then does quota + in-flight checks. */
        if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_QPU)) {
            return SYSCALL_EPERM;
        }
        qpu_submit_req_k_t req;
        if (!qpu_copy_in(arg, &req, sizeof(req))) {
            return SYSCALL_EFAULT;
        }
        return qpu_submit(pid, (const qpu_submit_req_t *)&req);
    }
    case QPU_OP_FETCH: {
        if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_EXECUTE, DEVICE_ID_QPU)) {
            return SYSCALL_EPERM;
        }
        /* Validate the output pointer BEFORE the broker commits PENDING->RUNNING
         * and stamps the executor: a copy-out failure AFTER that mutation would
         * strand the job RUNNING forever (the executor never learns its
         * job_id). Single-CPU cli'd, so a mapping valid here stays valid
         * through the copy below. Copy-OUT → validate WRITABLE. */
        if (!user_ok(arg, sizeof(qpu_fetch_out_k_t), 1)) {
            return SYSCALL_EFAULT;
        }
        qpu_fetch_out_k_t out;
        for (size_t i = 0; i < sizeof(out); i++) {
            ((uint8_t *)&out)[i] = 0;
        }
        uint64_t r = qpu_fetch(pid, (qpu_fetch_out_t *)&out);
        if (r != 0) {
            qpu_copy_out(arg, &out, sizeof(out)); /* cannot fail — pre-validated */
        }
        return r;
    }
    case QPU_OP_COMPLETE: {
        if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_EXECUTE, DEVICE_ID_QPU)) {
            return SYSCALL_EPERM;
        }
        qpu_complete_req_k_t req;
        if (!qpu_copy_in(arg, &req, sizeof(req))) {
            return SYSCALL_EFAULT;
        }
        return qpu_complete(pid, (const qpu_complete_req_t *)&req);
    }
    case QPU_OP_POLL: {
        /* POLL is ownership-gated (the broker checks owner_pid + generation)
         * under the same WRITE authority a submitter already holds. */
        if (!authorize(pid, CAP_RESOURCE_DEVICE, CAP_WRITE, DEVICE_ID_QPU)) {
            return SYSCALL_EPERM;
        }
        /* Validate the output pointer BEFORE the broker copies the DONE result
         * and FREES the slot: a copy-out failure after slot_free would destroy
         * the only copy of the result (the slot is gone). Pre-validated, so the
         * copy below cannot fail. Copy-OUT → validate WRITABLE. */
        if (!user_ok(arg2, sizeof(qpu_poll_out_k_t), 1)) {
            return SYSCALL_EFAULT;
        }
        qpu_poll_out_k_t out;
        for (size_t i = 0; i < sizeof(out); i++) {
            ((uint8_t *)&out)[i] = 0;
        }
        uint64_t r = qpu_poll(pid, (uint32_t)arg, (qpu_poll_out_t *)&out);
        /* On a successful poll (state code >= 1) copy the struct out. */
        if ((int64_t)r > 0) {
            qpu_copy_out(arg2, &out, sizeof(out)); /* cannot fail — pre-validated */
        }
        return r;
    }
    default:
        return SYSCALL_EINVAL;
    }
}

/* ============================================================================
 * Dispatch
 * ============================================================================ */

static void syscall_dispatch(cpu_state_t *state) {
    process_t *cur = process_get_current();
    uint32_t pid = cur ? cur->pid : 0;

    switch (state->rax) {
    case SYS_WRITE:
        state->rax = sys_write(pid, state->rdi);
        break;
    case SYS_GETPID:
        state->rax = pid;
        break;
    case SYS_YIELD:
        state->rax = 0;
        scheduler_reschedule(state);
        break;
    case SYS_EXIT:
        sys_exit(pid, state->rdi, state);
        break;
    case SYS_TICKS:
        state->rax = timer_get_ticks();
        break;
    case SYS_SEND:
        state->rax = sys_send(pid, state->rdi, state->rsi);
        break;
    case SYS_RECV:
        state->rax = sys_recv(pid, state->rdi, state->rsi);
        break;
    case SYS_HEARTBEAT:
        service_heartbeat(); /* uses the current process to find its slot */
        state->rax = 0;
        break;
    case SYS_SVC_RESTARTS:
        state->rax = (uint64_t)(int64_t)service_current_restart_count();
        break;
    case SYS_QRAND:
        state->rax = sys_qrand(pid, state->rdi, state->rsi);
        break;
    case SYS_SEND_TO:
        state->rax = sys_send_to(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_COM2:
        state->rax = sys_com2(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_QSEED:
        state->rax = sys_qseed(pid);
        break;
    case SYS_FIELD_SNAPSHOT:
        state->rax = sys_field_snapshot(pid, state->rdi, state->rsi);
        break;
    case SYS_CONS:
        state->rax = sys_cons(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_SYSINFO:
        state->rax = sys_sysinfo(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_OPEN:
        state->rax = sys_open(pid, state->rdi, state->rsi);
        break;
    case SYS_READ:
        state->rax = sys_read(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_CLOSE:
        state->rax = sys_close(pid, state->rdi);
        break;
    case SYS_READDIR:
        state->rax = sys_readdir(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_SPAWN:
        state->rax = sys_spawn(pid, state->rdi);
        break;
    case SYS_WAITPID:
        state->rax = sys_waitpid(pid, state->rdi);
        break;
    case SYS_FWRITE:
        state->rax = sys_fwrite(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_UNLINK:
        state->rax = sys_unlink(pid, state->rdi);
        break;
    case SYS_SYNC:
        state->rax = sys_sync_fs(pid);
        break;
    case SYS_RESOLVE:
        state->rax = sys_resolve(pid, state->rdi, state->rsi);
        break;
    case SYS_UDP:
        state->rax = sys_udp(pid, state->rdi, state->rsi);
        break;
    case SYS_TCP:
        state->rax = sys_tcp(pid, state->rdi, state->rsi);
        break;
    case SYS_IMPRINT:
        state->rax = sys_imprint(pid, state->rdi);
        break;
    case SYS_RECALL:
        state->rax = sys_recall(pid, state->rdi, state->rsi);
        break;
    case SYS_FIELD_INFO:
        state->rax = sys_field_info(pid, state->rdi, state->rsi);
        break;
    case SYS_AUDIT:
        state->rax = sys_audit(pid, state->rdi, state->rsi, state->rdx);
        break;
    case SYS_MANIFEST:
        state->rax = sys_manifest(pid, state->rdi, state->rsi);
        break;
    case SYS_CAP_DERIVE:
        state->rax = sys_cap_derive(pid, state->rdi);
        break;
    case SYS_QPU:
        state->rax = sys_qpu(pid, state->rdi, state->rsi, state->rdx);
        break;
    default:
        state->rax = SYSCALL_ENOSYS;
        break;
    }
}

void syscall_init(void) {
    /* DPL 3 gate: ring 3 may execute int 0x80; everything else in the
     * IDT stays kernel-only */
    idt_set_gate(SYSCALL_VECTOR, (uint64_t)isr128, GDT_KERNEL_CS, GATE_TYPE_INTERRUPT | DPL_USER);
    interrupt_register(SYSCALL_VECTOR, syscall_dispatch, NULL);
    boot_log("Syscall interface installed (int 0x80)");
}

/* ============================================================================
 * User process setup
 * ============================================================================ */

/* Allocate a physical frame and map it at uvaddr in `as`. Returns the
 * frame's kernel-VA (== physical, identity-mapped) for population, or
 * NULL on failure. */
static uint8_t *map_fresh_page(address_space_t *as, uint64_t uvaddr, bool writable) {
    void *frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    memset(frame, 0, PAGE_SIZE);
    if (!vmspace_map_page(as, uvaddr, (uint64_t)frame, writable)) {
        pmm_free_frame(frame); /* never mapped -> vmspace_destroy can't reclaim it (cf. elf.c) */
        return NULL;
    }
    return (uint8_t *)frame;
}

status_t user_process_spawn(const char *name, const void *blob_start, const void *blob_end,
                            uint32_t *pid_out) {
    size_t blob_size = (size_t)((const uint8_t *)blob_end - (const uint8_t *)blob_start);
    if (blob_size == 0 || blob_size > USER_CODE_PAGES * PAGE_SIZE) {
        return STATUS_INVALID_ARG;
    }

    /* Private address space for this process */
    address_space_t as = vmspace_create();
    if (!as.pml4) {
        return STATUS_NO_MEMORY;
    }

    /* Map + populate the code window (executable via absent NX; the
     * blob is copied through the frame's identity VA) */
    const uint8_t *src = (const uint8_t *)blob_start;
    for (uint32_t p = 0; p < USER_CODE_PAGES; p++) {
        uint64_t uvaddr = USER_VBASE + (uint64_t)p * PAGE_SIZE;
        uint8_t *page = map_fresh_page(&as, uvaddr, true);
        if (!page) {
            vmspace_destroy(as.pml4); /* reclaim the private half + tables, else leak-per-attempt */
            return STATUS_NO_MEMORY;
        }
        size_t off = (size_t)p * PAGE_SIZE;
        for (size_t b = 0; b < PAGE_SIZE && off + b < blob_size; b++) {
            page[b] = src[off + b];
        }
    }

    /* Zeroed writable data page (isolation canary target) */
    if (!map_fresh_page(&as, USER_DATA_VADDR, true)) {
        vmspace_destroy(as.pml4);
        return STATUS_NO_MEMORY;
    }

    /* Stack + process creation shared with the ELF path (no args) */
    return finalize_user_process(&as, name, USER_VBASE, NULL, pid_out);
}

/* Map the user stack and create the ring-3 process bound to `as`, with
 * the given entry point. Shared by the flat-blob and ELF spawn paths.
 * `kargs` (may be NULL) is the argument vector to expose read-only at
 * USER_ARGS_VADDR; NULL leaves the process with argc == 0. */
static status_t finalize_user_process(address_space_t *as, const char *name, uint64_t entry,
                                      const kuser_args_t *kargs, uint32_t *pid_out) {
    /* User stack, mapped just below USER_STACK_TOP. Every failure return below
     * MUST vmspace_destroy(as->pml4) before the address space is bound to a PCB
     * (below) — until then nothing else can reclaim it, so an abandoned `as`
     * leaks its whole private half + page tables (+ ELF segment frames on the
     * ELF path). Unbounded, this is a ring-3-reachable pmm-exhaustion DoS: fill
     * the process table and every further spawn leaks an address space. Mirrors
     * spawn_elf_args's elf_load-failure cleanup (#161). */
    for (uint32_t p = 1; p <= USER_STACK_PAGES; p++) {
        uint64_t uvaddr = USER_STACK_TOP - (uint64_t)p * PAGE_SIZE;
        if (!map_fresh_page(as, uvaddr, true)) {
            vmspace_destroy(as->pml4);
            return STATUS_NO_MEMORY;
        }
    }

    /* Argument-vector page: mapped read-only to the process (populated here
     * through the frame's supervisor identity VA). Always present so
     * get_args() reads a valid page; a spawn without args leaves argc == 0. */
    uint8_t *argpage = map_fresh_page(as, USER_ARGS_VADDR, false);
    if (!argpage) {
        vmspace_destroy(as->pml4);
        return STATUS_NO_MEMORY;
    }
    if (kargs) {
        const uint8_t *s = (const uint8_t *)kargs;
        for (size_t b = 0; b < sizeof(kuser_args_t) && b < PAGE_SIZE; b++) {
            argpage[b] = s[b];
        }
    } /* else the page is already zeroed by map_fresh_page -> argc == 0 */

    process_create_params_t params = {.name = name,
                                      .type = PROCESS_TYPE_USER,
                                      .priority = PRIORITY_NORMAL,
                                      .parent_pid = KERNEL_PROCESS_ID,
                                      .entry_point = (void *)entry,
                                      .stack_address = (void *)(USER_STACK_TOP - USER_REGION_SIZE),
                                      .stack_size =
                                          USER_REGION_SIZE, /* so stack_top == USER_STACK_TOP */
                                      .is_quantum_aware = false};

    process_t *proc = NULL;
    status_t result = process_create(&params, &proc);
    if (result != STATUS_SUCCESS) {
        /* Table full (ring-3 reachable) or any create failure: the address
         * space is not yet bound to a PCB, so reclaim it here or it leaks. */
        vmspace_destroy(as->pml4);
        return result;
    }

    /* Bind the process to its private address space. From here ownership of `as`
     * transfers to the PCB and process_destroy is responsible for reclaiming it
     * — do NOT vmspace_destroy on any later path. */
    proc->cr3 = as->cr3;
    proc->virtual_address_space = as->pml4;

    /* Bind the restrictive DEFAULT manifest to EVERY ring-3 process (epic
     * #135): a bare bound manifest with no allow rows and no spawn quota.
     * This is the single choke point all three spawn paths funnel through
     * (flat blob, ELF, SYS_SPAWN), so after it "unbound" is provably ring-0
     * only — a manifest-checked capability a ring-3 process should not hold
     * would MDENY. A SERVICE overwrites this immediately in start_slot with
     * its grant-derived manifest (manifest_bind is last-write-wins, and both
     * binds run inside start_slot's cli window). A SYS_SPAWN child or a
     * capless boot citizen keeps this default: it declares "intended to do
     * nothing privileged", which is exactly true. */
    {
        manifest_t def;
        memset(&def, 0, sizeof(def));
        def.bound = 1; /* entry_count 0, spawn_max 0 */
        manifest_bind(proc->pid, &def);
    }

    if (pid_out) {
        *pid_out = proc->pid;
    }
    return STATUS_SUCCESS;
}

/* Spawn a user process from an embedded ELF64 image: the loader maps
 * and populates the program's PT_LOAD segments into a private address
 * space, then the process starts at the ELF entry point. */
/* Full ELF spawn with an optional argument vector. `kargs` may be NULL. */
static status_t spawn_elf_args(const char *name, const void *elf_start, const void *elf_end,
                               const kuser_args_t *kargs, uint32_t *pid_out) {
    size_t elf_size = (size_t)((const uint8_t *)elf_end - (const uint8_t *)elf_start);

    address_space_t as = vmspace_create();
    if (!as.pml4) {
        return STATUS_NO_MEMORY;
    }

    uint64_t entry = 0;
    if (elf_load(&as, (const uint8_t *)elf_start, elf_size, &entry) != ELF_OK) {
        /* vmspace_create allocated a PML4+PDPT (and elf_load may have mapped
         * some segment frames before failing); reclaim them all, or a malformed
         * spawn — e.g. spawning any non-ELF initrd file from qsh — leaks ~2+
         * frames per attempt until the pmm is exhausted (a ring-3-reachable DoS).
         * vmspace_destroy frees the private user half AND the page tables. */
        vmspace_destroy(as.pml4);
        return STATUS_ERROR;
    }

    return finalize_user_process(&as, name, entry, kargs, pid_out);
}

status_t user_process_spawn_elf(const char *name, const void *elf_start, const void *elf_end,
                                uint32_t *pid_out) {
    return spawn_elf_args(name, elf_start, elf_end, NULL, pid_out);
}
