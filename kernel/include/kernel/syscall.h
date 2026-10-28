/**
 * QuantumOS System Call Interface
 *
 * Ring-3 entry into the kernel via int 0x80 (IDT gate with DPL 3,
 * dispatched through the existing interrupt path). rax selects the
 * call, rdi/rsi carry arguments, the result returns in rax.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <kernel/types.h>
#include <kernel/interrupts.h>

#define SYSCALL_VECTOR 0x80

/* Syscall numbers */
#define SYS_WRITE 1 /* rdi = NUL-terminated string in user memory */
#define SYS_GETPID 2
#define SYS_YIELD 3
#define SYS_EXIT 4 /* rdi = exit code */
#define SYS_TICKS 5
#define SYS_SEND 6         /* rdi = msg ptr, rsi = len; routed by IPC capability */
#define SYS_RECV 7         /* rdi = buf ptr, rsi = len; returns sender pid (0 = empty) */
#define SYS_HEARTBEAT 8    /* report liveness to the service watchdog */
#define SYS_SVC_RESTARTS 9 /* restart count of the caller's service slot, or -1 */
#define SYS_QRAND                                                                                  \
    10 /* rdi = buf, rsi = len; fill up to QRAND_MAX_BYTES bytes
                         * from the qseed-mixed quantum generator. Gated on a
                         * CAP_RESOURCE_QUANTUM read capability (EPERM without).
                         * len == 0 is a provenance query: returns 1 if the
                         * kernel booted with a qseed, else 0. */
#define SYS_SEND_TO                                                                                \
    11 /* rdi = dest pid, rsi = msg ptr, rdx = len; like
                         * SYS_SEND but to a chosen destination the caller holds
                         * an IPC send-capability for (EPERM if it holds none for
                         * that dest). Lets a service reply to the sender pid
                         * recv handed it rather than a first-match peer. */
#define SYS_COM2                                                                                   \
    12 /* rdi = op (SYS_COM2_READ/WRITE), rsi = buf, rdx = len;
                         * raw byte pipe on the COM2 swarm-bridge UART (0x2F8).
                         * Gated on a CAP_RESOURCE_DEVICE capability over
                         * DEVICE_ID_COM2 (CAP_WRITE to send, CAP_READ to recv);
                         * EPERM without. Returns bytes moved (read may be 0 when
                         * nothing is waiting — the pipe is non-blocking). */
#define SYS_QSEED                                                                                  \
    13 /* returns the u64 boot qseed the kernel accepted on the
                         * cmdline (0 = none). Gated on the same quantum-pool read
                         * capability as SYS_QRAND (EPERM without) — lets an
                         * attestation service name the entropy the system booted
                         * with, so a host can bind the boot to that qseed. */
#define SYS_FIELD_SNAPSHOT                                                                         \
    14 /* rdi = buf, rsi = len; publish up to
                         * FIELD_SNAP_BYTES signed bytes of a downsampled
                         * memory-field snapshot into a kernel visualization
                         * buffer. No capability: it grants no authority — it is
                         * a pure display sink the framebuffer renders under a
                         * GRUB/ISO boot. A no-op in effect under the -kernel/VGA
                         * path (nothing samples it). Returns bytes stored. */

#define SYS_CONS                                                                                   \
    15 /* rdi = op (SYS_CONS_READ/WRITE), rsi = buf, rdx = len;
                         * raw byte pipe on the interactive console (COM1 RX ring
                         * + PS/2 keyboard in, raw COM1 TX out — no "[user pid]"
                         * prefix, so a shell owns its own line discipline).
                         * Gated on a CAP_RESOURCE_DEVICE capability over
                         * DEVICE_ID_CONSOLE (CAP_READ to read, CAP_WRITE to
                         * write); EPERM without. Read is non-blocking: returns
                         * the bytes buffered so far (0 if none). */
#define SYS_SYSINFO                                                                                \
    16 /* rdi = op (SYSINFO_PS/MEM), rsi = buf, rdx = len;
                         * kernel-formatted live introspection text copied into
                         * the caller's buffer: the process table (one "PS: pid
                         * name STATE" line per live process) or memory stats
                         * ("MEM: heap free=... frames free=.../..."). Read-only
                         * and uncapped, like SYS_GETPID/SYS_TICKS: it names no
                         * authority, it only reports. Returns bytes copied. */

#define SYS_OPEN                                                                                   \
    17 /* rdi = path ptr (NUL-terminated, user memory); open a
                         * regular file on the read-only embedded initrd. Returns
                         * a small non-negative fd, ENOENT if the path names no
                         * file, EIO if the caller's fd table is full. Uncapped:
                         * the initrd is public read-only data baked into the
                         * kernel image (per-file capabilities are future work,
                         * noted in docs/INITRD_VFS.md). */
#define SYS_READ                                                                                   \
    18               /* rdi = fd, rsi = buf, rdx = len; sequential read from an
                         * open initrd file. Returns bytes copied (0 = EOF),
                         * EINVAL on a bad fd. */
#define SYS_CLOSE 19 /* rdi = fd; release the fd table slot. */
#define SYS_READDIR                                                                                \
    20 /* rdi = path ptr, rsi = buf, rdx = len; kernel-formatted
                         * listing of initrd files under path ("/" lists all):
                         * one "FS: <name> <size>" line per file. Returns bytes
                         * copied. */
#define SYS_SPAWN                                                                                  \
    21 /* rdi = path ptr (NUL-terminated, user memory); load the
                         * named initrd ELF into a fresh address space and start
                         * it as a ring-3 process. Returns the new pid. Spawning
                         * is REAL authority — gated on a CAP_RESOURCE_PROCESS
                         * capability carrying CAP_EXECUTE over SPAWN_RESOURCE_ID
                         * (EPERM without); only qsh is granted it. ENOENT if the
                         * path names no file, EIO if the load fails. */
#define SYS_WAITPID                                                                                \
    22 /* rdi = target pid; poll a spawned process's fate. Returns
                         * the exit code (0-255) once it has exited (served from
                         * a small kernel exit ledger, so a reaped PCB still
                         * answers), WAITPID_RUNNING (256) while it lives, ENOENT
                         * if the pid is unknown. Non-blocking: the caller loops
                         * with SYS_YIELD. Uncapped introspection, like
                         * SYS_SYSINFO. */

#define SYS_FWRITE                                                                                 \
    23 /* rdi = fd, rsi = buf, rdx = len; append to a file opened
                         * with write flags on the RAM overlay (epic #71). The
                         * write authority was checked at open; the fd carries
                         * it. Returns bytes appended (may be short at the file
                         * size cap), EINVAL on a non-writable or bad fd. */
#define SYS_UNLINK                                                                                 \
    24 /* rdi = path ptr; remove a RAM-overlay file and free its
                         * storage. Gated on the filesystem-write capability
                         * (EPERM without). Refused with EIO while any live
                         * process holds the file open; ENOENT if absent (the
                         * initrd is immutable — not unlinkable). */
#define SYS_SYNC                                                                                   \
    25 /* no args; serialize the RAM overlay to the ATA disk as a
                         * ustar archive behind the QDSK superblock (restored at
                         * next boot). Gated on the filesystem-write capability.
                         * Returns files flushed, EIO with no disk or on write
                         * failure. */
#define SYS_RESOLVE                                                                                \
    26 /* rdi = hostname ptr, rsi = 4-byte out buffer; resolve a
                         * hostname to an IPv4 A record via the kernel's DNS
                         * client. Non-blocking, request/poll: the first call
                         * posts the request (returns WOULD_BLOCK); subsequent
                         * calls poll — WOULD_BLOCK while pending, 0 with the
                         * address written on success, EIO on failure (no
                         * network or lookup failed). The network I/O runs in
                         * the IF=1 net thread, not this cli'd syscall. Gated on
                         * a CAP_RESOURCE_DEVICE capability over DEVICE_ID_NET
                         * (EPERM without) — held only by qsh. */

#define SYS_UDP                                                                                    \
    27 /* rdi = op, rsi = udp_req_t ptr; ring-3 UDP sockets (epic
                         * #80). Ops: UDP_OP_BIND (port 0 = ephemeral, returns
                         * sock id), UDP_OP_SENDTO (<= 1472 bytes queued for the
                         * net thread, WOULD_BLOCK when the TX queue is full),
                         * UDP_OP_RECVFROM (pops the socket's RX ring, returns
                         * byte count or WOULD_BLOCK when empty), UDP_OP_CLOSE.
                         * Non-blocking throughout — all NIC I/O happens in the
                         * IF=1 net thread, never in this cli'd syscall. Gated
                         * on CAP_RESOURCE_DEVICE over DEVICE_ID_NET (EPERM
                         * without) — held only by qsh. */

/* SYS_UDP operations (rdi). */
#define UDP_OP_BIND 0
#define UDP_OP_SENDTO 1
#define UDP_OP_RECVFROM 2
#define UDP_OP_CLOSE 3

#define SYS_TCP                                                                                    \
    28 /* rdi = op, rsi = tcp_req_t ptr; ring-3 TCP (epics #82
                         * client, #98 server). Ops: TCP_OP_CONNECT (active
                         * open to ip:port, WOULD_BLOCK while the handshake
                         * runs, 0 once ESTABLISHED, EIO on refuse/timeout),
                         * TCP_OP_SEND (<= MSS bytes, WOULD_BLOCK while a
                         * segment is outstanding), TCP_OP_RECV (bytes from
                         * the receive ring, 0 = peer closed / EOF,
                         * WOULD_BLOCK when empty), TCP_OP_CLOSE (graceful
                         * FIN, poll to CLOSED), TCP_OP_STATUS, TCP_OP_LISTEN
                         * (passive open on req.port — poll to 0 = armed),
                         * TCP_OP_ACCEPT (poll: 0 = peer connected, EIO =
                         * recover with CLOSE + re-LISTEN). One client and
                         * one server connection at a time, one connection
                         * per pid; all TCP I/O runs in the IF=1 net thread.
                         * Gated on CAP_RESOURCE_DEVICE over DEVICE_ID_NET
                         * (EPERM without) — held only by grant_net services
                         * (qsh, fieldsyncd, httpd). */

/* SYS_TCP operations (rdi). */
#define TCP_OP_CONNECT 0
#define TCP_OP_SEND 1
#define TCP_OP_RECV 2
#define TCP_OP_CLOSE 3
#define TCP_OP_STATUS 4
#define TCP_OP_LISTEN 5
#define TCP_OP_ACCEPT 6

#define SYS_IMPRINT                                                                                \
    29 /* rdi = field_imprint_req ptr; store a pattern into a
                         * holographic field region (epic #95). Gated on a
                         * CAP_RESOURCE_FIELD WRITE cap naming EXACTLY
                         * req.region (cap_find_resource; EPERM without).
                         * Returns the slot index. Capless callers are denied
                         * BEFORE any user memory is read, so they always get
                         * exactly EPERM. */
#define SYS_RECALL                                                                                 \
    30 /* rdi = field_recall_req ptr, rsi = field_recall_out ptr;
                         * ranked associative recall against a region (epic
                         * #95). Gated on a CAP_RESOURCE_FIELD READ cap naming
                         * req.region. Retrieval reinforcement (a write to the
                         * energy landscape) is applied ONLY when the caller
                         * also holds CAP_WRITE — a read-only capability never
                         * mutates. n=0 (nothing resonates, incl. a degenerate
                         * probe) is success, not an error. */
#define SYS_FIELD_INFO                                                                             \
    31 /* rdi = region, rsi = field_info_out ptr; read-only
                         * enumeration of a field region's live slots (epic #127
                         * B1). Gated on a CAP_RESOURCE_FIELD READ cap naming
                         * EXACTLY the region — writes NOTHING, the honest
                         * non-mutating counterpart to SYS_RECALL. Returns 0. */

#define SYS_AUDIT                                                                                  \
    32 /* rdi = op (AUDIT_OP_READ/STATS), rsi = buf, rdx = len; read
                         * the capability authority ledger (epic #133 Phase D).
                         * Uncapped read-only introspection (the SYS_SYSINFO
                         * class): formats kernel-recorded GRANT/DENY/SPAWN text.
                         * Names no authority — it only reports. Returns bytes
                         * copied. */

#define SYS_MANIFEST                                                                               \
    33 /* rdi = buf, rsi = len; read the per-pid intent manifests
                         * (epic #135 Phase D increment 2). Uncapped read-only
                         * introspection (the SYS_AUDIT class): dumps every bound
                         * manifest's declared allow-set, spawn quota (used/max),
                         * and cpu_ticks as text. Reading mutates nothing.
                         * Returns bytes copied. */

#define SYS_CAP_DERIVE                                                                             \
    34 /* rdi = cap_derive_req ptr; delegate a NARROWED slice of one
                         * of the caller's own capabilities to a sub-agent (epic
                         * #137 Phase D increment 3). Bounded by AND reflected in
                         * the intent manifest: the caller may only delegate a
                         * resource it is declared to touch, and the derive
                         * extends the recipient's manifest. One-hop only
                         * (CAP_GRANT/REVOKE are refused in the handed perms).
                         * Names the parent by (resource_type,resource_id), never
                         * a handle. Returns 0 or a negative errno. */

#define SYS_QPU                                                                                    \
    35 /* rdi = QPU_OP_*; the QPU job BROKER (epic #148). SUBMIT
                         * (rsi=req) queues an OPAQUE circuit for holders of the
                         * QPU WRITE cap, quota-gated (manifest qsub) and
                         * in-flight-capped, returning a job id; FETCH/COMPLETE
                         * (rsi=out/req) are the EXECUTOR's ops (QPU EXECUTE cap,
                         * qpud only — the completer must be the fetcher, same
                         * incarnation); POLL (rsi=job id, rdx=out) returns the
                         * owner's job state and, when DONE, the bounded result
                         * (slot freed). The kernel never parses a circuit. */

/* SYS_RESOLVE / SYS_UDP: still pending (poll again) / queue-ring full. */
#define RESOLVE_WOULDBLOCK ((uint64_t) - 11)

/* SYS_WAITPID: the target is still running. */
#define WAITPID_RUNNING 256

/* Resource id of the spawn right under CAP_RESOURCE_PROCESS. */
#define SPAWN_RESOURCE_ID 0

/* SYS_OPEN flags (rsi). Write flags require the filesystem-write
 * capability (CAP_RESOURCE_DEVICE over DEVICE_ID_DISK) — volume-level
 * write authority granted declaratively to qsh alone. O_WRONLY requires
 * O_CREAT (overlay files are created-or-appended; the initrd is
 * immutable). Writes always append; O_TRUNC clears the file first. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 2
#define O_TRUNC 4

/* Bytes SYS_FIELD_SNAPSHOT stores at most (bounds the kernel viz buffer). */
#define FIELD_SNAP_BYTES 256

/* Longest path SYS_OPEN/SYS_READDIR will copy from user memory. */
#define VFS_PATH_MAX 128

/* SYS_COM2 operations (rdi) */
#define SYS_COM2_READ 0
#define SYS_COM2_WRITE 1

/* SYS_CONS operations (rdi) */
#define SYS_CONS_READ 0
#define SYS_CONS_WRITE 1

/* SYS_SYSINFO operations (rdi) */
#define SYSINFO_PS 0
#define SYSINFO_MEM 1
#define SYSINFO_TIME 2       /* "TIME: YYYY-MM-DD HH:MM:SS" from the CMOS RTC */
#define SYSINFO_QUIET 3      /* no buffer; returns 1 if the kernel booted `quiet` */
#define SYSINFO_PEER 4       /* no buffer; arg2 = index; returns packed peer= IP (0=oob) */
#define SYSINFO_PEER_COUNT 5 /* no buffer; returns the configured peer count (epic #139) */
#define SYSINFO_SCHED                                                                              \
    6 /* scheduler baseline line: "SCHED: switches=.. preempt=.. ticks=.. maxgap=.. spread=.. runnable=.." (ADR-0022) */

/* Bytes SYS_CONS will move per call at most (bounds the kernel bounce buffer). */
#define CONS_MAX_BYTES 256

/* Bytes SYS_SYSINFO will produce at most (bounds the kernel format buffer). */
#define SYSINFO_MAX_BYTES 1024

/* Bytes SYS_QRAND will draw per call at most (a capless caller is denied
 * before any draw). */
#define QRAND_MAX_BYTES 64

/* Bytes SYS_COM2 will move per call at most (bounds the kernel bounce buffer). */
#define COM2_MAX_BYTES 256

/* Error returns (in rax) */
#define SYSCALL_EINVAL ((uint64_t) - 1)
#define SYSCALL_EFAULT ((uint64_t) - 2)
#define SYSCALL_ENOSYS ((uint64_t) - 3)
#define SYSCALL_EPERM ((uint64_t) - 4)   /* no capability authorising the op */
#define SYSCALL_EIO ((uint64_t) - 5)     /* delivery failed (e.g. queue full) */
#define SYSCALL_ENOENT ((uint64_t) - 6)  /* no such file on the initrd */
#define SYSCALL_EEGRESS ((uint64_t) - 7) /* refused by the egress grant (ADR-0030) */

/* User memory window: identity-mapped, user-bit pages above the
 * kernel heap. Each user process gets one 2 MB region (code at the
 * base, stack at the top). */
#define USER_REGION_BASE 0x06000000UL /* 96 MB */
#define USER_REGION_SIZE 0x200000UL   /* one 2 MB page */
#define USER_MAX_PROCESSES 4

/* Install the int 0x80 gate and dispatch handler */
void syscall_init(void);

/* Take the latest published field snapshot for rendering. Copies up to
 * FIELD_SNAP_BYTES into `dst`, writes the count to `*out_n`, and returns true
 * iff a new snapshot has arrived since the last take (clears the dirty flag).
 * Consumed by the framebuffer live-field renderer in the idle loop. */
bool field_snapshot_take(int8_t *dst, int *out_n);

/* Map the user regions (sets the user bit on the covering page-table
 * entries), copy the embedded user programs, and spawn them */
void user_init(void);

/* Spawn a user process running a copy of the blob [start, end) in the
 * next free user region. Returns STATUS_* codes. */
status_t user_process_spawn(const char *name, const void *blob_start, const void *blob_end,
                            uint32_t *pid_out);

/* Spawn a user process from an embedded ELF64 image [start, end). */
status_t user_process_spawn_elf(const char *name, const void *elf_start, const void *elf_end,
                                uint32_t *pid_out);

/* Boot self-test: proves the reused command-line parse buffer carries no stale
 * argv residue into a later child's args page (cross-process disclosure).
 * Returns 0 on pass, negative on the first failure. */
int spawn_argv_leak_selftest(void);

#endif /* SYSCALL_H */
