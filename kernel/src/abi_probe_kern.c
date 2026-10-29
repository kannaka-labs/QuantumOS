/**
 * QuantumOS v1 ABI freeze probe - KERNEL ring (ADR-0020).
 *
 * Kernel-side twin of user/abi_probe.c. NEVER linked into the kernel or run: it
 * is compiled to an object file under the REAL kernel build flags (CFLAGS), and
 * scripts/extract-abi.py reads the compiler-measured values back out of the
 * .abi_ents section with objdump -s. Entries are ring-namespaced kern: so they
 * never collide with the user: table; the extractor cross-checks that each
 * shared logical name has the SAME value on both rings (the twin ABI), diffs the
 * merged table against the committed golden, and fails on any add/remove.
 *
 * Kernel struct sizes are emitted under the user-facing LOGICAL name (e.g.
 * kern:size:udp_req_t = sizeof(udp_req_k_t)) so the twin check lines up directly
 * with user:size:udp_req_t.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <kernel/syscall.h>
#include <kernel/syscall_abi.h>
#include <kernel/audit.h>
#include <kernel/qpu.h>
#include <kernel/capability.h>
#include <kernel/net.h>
#include <kernel/console.h>
#include <kernel/ata.h>
#include <kernel/com2_uart.h>

_Static_assert(sizeof(long) == 8 && sizeof(void *) == 8, "wrong data model / ABI");

struct abi_ent {
    char name[56];
    unsigned long long value;
};

#define ABI(n, v) {n, (unsigned long long)(v)}

__attribute__((used, section(".abi_ents"))) const struct abi_ent abi_kern[] = {
    /* --- syscall numbers (1..35) --- */
    ABI("kern:num:SYS_WRITE", SYS_WRITE),
    ABI("kern:num:SYS_GETPID", SYS_GETPID),
    ABI("kern:num:SYS_YIELD", SYS_YIELD),
    ABI("kern:num:SYS_EXIT", SYS_EXIT),
    ABI("kern:num:SYS_TICKS", SYS_TICKS),
    ABI("kern:num:SYS_SEND", SYS_SEND),
    ABI("kern:num:SYS_RECV", SYS_RECV),
    ABI("kern:num:SYS_HEARTBEAT", SYS_HEARTBEAT),
    ABI("kern:num:SYS_SVC_RESTARTS", SYS_SVC_RESTARTS),
    ABI("kern:num:SYS_QRAND", SYS_QRAND),
    ABI("kern:num:SYS_SEND_TO", SYS_SEND_TO),
    ABI("kern:num:SYS_COM2", SYS_COM2),
    ABI("kern:num:SYS_QSEED", SYS_QSEED),
    ABI("kern:num:SYS_FIELD_SNAPSHOT", SYS_FIELD_SNAPSHOT),
    ABI("kern:num:SYS_CONS", SYS_CONS),
    ABI("kern:num:SYS_SYSINFO", SYS_SYSINFO),
    ABI("kern:num:SYS_OPEN", SYS_OPEN),
    ABI("kern:num:SYS_READ", SYS_READ),
    ABI("kern:num:SYS_CLOSE", SYS_CLOSE),
    ABI("kern:num:SYS_READDIR", SYS_READDIR),
    ABI("kern:num:SYS_SPAWN", SYS_SPAWN),
    ABI("kern:num:SYS_WAITPID", SYS_WAITPID),
    ABI("kern:num:SYS_FWRITE", SYS_FWRITE),
    ABI("kern:num:SYS_UNLINK", SYS_UNLINK),
    ABI("kern:num:SYS_SYNC", SYS_SYNC),
    ABI("kern:num:SYS_RESOLVE", SYS_RESOLVE),
    ABI("kern:num:SYS_UDP", SYS_UDP),
    ABI("kern:num:SYS_TCP", SYS_TCP),
    ABI("kern:num:SYS_IMPRINT", SYS_IMPRINT),
    ABI("kern:num:SYS_RECALL", SYS_RECALL),
    ABI("kern:num:SYS_FIELD_INFO", SYS_FIELD_INFO),
    ABI("kern:num:SYS_AUDIT", SYS_AUDIT),
    ABI("kern:num:SYS_MANIFEST", SYS_MANIFEST),
    ABI("kern:num:SYS_CAP_DERIVE", SYS_CAP_DERIVE),
    ABI("kern:num:SYS_QPU", SYS_QPU),

    /* --- sub-op namespaces (values twin the user side; kernel uses *_OP_ names
     * for UDP/TCP but the numeric contract is identical) --- */
    ABI("kern:subop:SYS_COM2_READ", SYS_COM2_READ),
    ABI("kern:subop:SYS_COM2_WRITE", SYS_COM2_WRITE),
    ABI("kern:subop:SYS_CONS_READ", SYS_CONS_READ),
    ABI("kern:subop:SYS_CONS_WRITE", SYS_CONS_WRITE),
    ABI("kern:subop:SYSINFO_PS", SYSINFO_PS),
    ABI("kern:subop:SYSINFO_MEM", SYSINFO_MEM),
    ABI("kern:subop:SYSINFO_TIME", SYSINFO_TIME),
    ABI("kern:subop:SYSINFO_QUIET", SYSINFO_QUIET),
    ABI("kern:subop:SYSINFO_PEER", SYSINFO_PEER),
    ABI("kern:subop:SYSINFO_PEER_COUNT", SYSINFO_PEER_COUNT),
    ABI("kern:subop:UDP_BIND", UDP_OP_BIND),
    ABI("kern:subop:UDP_SENDTO", UDP_OP_SENDTO),
    ABI("kern:subop:UDP_RECVFROM", UDP_OP_RECVFROM),
    ABI("kern:subop:UDP_CLOSE", UDP_OP_CLOSE),
    ABI("kern:subop:TCP_CONNECT", TCP_OP_CONNECT),
    ABI("kern:subop:TCP_SEND", TCP_OP_SEND),
    ABI("kern:subop:TCP_RECV", TCP_OP_RECV),
    ABI("kern:subop:TCP_CLOSE", TCP_OP_CLOSE),
    ABI("kern:subop:TCP_STATUS", TCP_OP_STATUS),
    ABI("kern:subop:TCP_LISTEN", TCP_OP_LISTEN),
    ABI("kern:subop:TCP_ACCEPT", TCP_OP_ACCEPT),
    ABI("kern:subop:AUDIT_OP_READ", AUDIT_OP_READ),
    ABI("kern:subop:AUDIT_OP_STATS", AUDIT_OP_STATS),
    ABI("kern:subop:QPU_OP_SUBMIT", QPU_OP_SUBMIT),
    ABI("kern:subop:QPU_OP_FETCH", QPU_OP_FETCH),
    ABI("kern:subop:QPU_OP_COMPLETE", QPU_OP_COMPLETE),
    ABI("kern:subop:QPU_OP_POLL", QPU_OP_POLL),

    /* --- error-code table (the kernel is the canonical source) --- */
    ABI("kern:errno:SYSCALL_EINVAL", (unsigned long long)SYSCALL_EINVAL),
    ABI("kern:errno:SYSCALL_EFAULT", (unsigned long long)SYSCALL_EFAULT),
    ABI("kern:errno:SYSCALL_ENOSYS", (unsigned long long)SYSCALL_ENOSYS),
    ABI("kern:errno:SYSCALL_EPERM", (unsigned long long)SYSCALL_EPERM),
    ABI("kern:errno:SYSCALL_EIO", (unsigned long long)SYSCALL_EIO),
    ABI("kern:errno:SYSCALL_ENOENT", (unsigned long long)SYSCALL_ENOENT),
    ABI("kern:errno:RESOLVE_WOULDBLOCK", (unsigned long long)RESOLVE_WOULDBLOCK),
    ABI("kern:errno:WAITPID_RUNNING", WAITPID_RUNNING),

    /* --- capability permission bits (security-observable authority namespace) --- */
    ABI("kern:enum:CAP_READ", CAP_READ),
    ABI("kern:enum:CAP_WRITE", CAP_WRITE),
    ABI("kern:enum:CAP_EXECUTE", CAP_EXECUTE),
    ABI("kern:enum:CAP_GRANT", CAP_GRANT),
    ABI("kern:enum:CAP_REVOKE", CAP_REVOKE),
    ABI("kern:enum:CAP_QUANTUM", CAP_QUANTUM),
    ABI("kern:enum:CAP_DEVICE", CAP_DEVICE),
    ABI("kern:enum:CAP_PROCESS", CAP_PROCESS),
    ABI("kern:enum:CAP_PERM_ALL", CAP_PERM_ALL),

    /* --- capability resource-type namespace (which class a cap guards) --- */
    ABI("kern:enum:CAP_RESOURCE_MEMORY", CAP_RESOURCE_MEMORY),
    ABI("kern:enum:CAP_RESOURCE_IPC", CAP_RESOURCE_IPC),
    ABI("kern:enum:CAP_RESOURCE_DEVICE", CAP_RESOURCE_DEVICE),
    ABI("kern:enum:CAP_RESOURCE_QUANTUM", CAP_RESOURCE_QUANTUM),
    ABI("kern:enum:CAP_RESOURCE_PROCESS", CAP_RESOURCE_PROCESS),
    ABI("kern:enum:CAP_RESOURCE_SERVICE", CAP_RESOURCE_SERVICE),
    ABI("kern:enum:CAP_RESOURCE_FIELD", CAP_RESOURCE_FIELD),
    ABI("kern:enum:CAP_RESOURCE_TYPE_COUNT", CAP_RESOURCE_TYPE_COUNT),

    /* --- device-id namespace (the resource_id a CAP_RESOURCE_DEVICE cap names) --- */
    ABI("kern:devid:DEVICE_ID_NET", DEVICE_ID_NET),
    ABI("kern:devid:DEVICE_ID_QPU", DEVICE_ID_QPU),
    ABI("kern:devid:DEVICE_ID_COM2", DEVICE_ID_COM2),
    ABI("kern:devid:DEVICE_ID_CONSOLE", DEVICE_ID_CONSOLE),
    ABI("kern:devid:DEVICE_ID_DISK", DEVICE_ID_DISK),

    /* --- ring-crossing struct sizes, emitted under the user-facing logical name
     * (value is the kernel _k_t size) so the twin check lines up 1:1 --- */
    ABI("kern:size:udp_req_t", sizeof(udp_req_k_t)),
    ABI("kern:size:cap_derive_req_t", sizeof(cap_derive_req_k_t)),
    ABI("kern:size:qpu_submit_req_t", sizeof(qpu_submit_req_k_t)),
    ABI("kern:size:qpu_fetch_out_t", sizeof(qpu_fetch_out_k_t)),
    ABI("kern:size:qpu_complete_req_t", sizeof(qpu_complete_req_k_t)),
    ABI("kern:size:qpu_poll_out_t", sizeof(qpu_poll_out_k_t)),

    /* --- ring-crossing struct field offsets (a size-preserving field
     * reorder passes the size check but reddens here) --- */
    ABI("kern:off:udp_req_t.sock", __builtin_offsetof(udp_req_k_t, sock)),
    ABI("kern:off:udp_req_t.ip", __builtin_offsetof(udp_req_k_t, ip)),
    ABI("kern:off:udp_req_t.port", __builtin_offsetof(udp_req_k_t, port)),
    ABI("kern:off:udp_req_t.len", __builtin_offsetof(udp_req_k_t, len)),
    ABI("kern:off:udp_req_t.buf", __builtin_offsetof(udp_req_k_t, buf)),
    ABI("kern:off:cap_derive_req_t.resource_type",
        __builtin_offsetof(cap_derive_req_k_t, resource_type)),
    ABI("kern:off:cap_derive_req_t.resource_id",
        __builtin_offsetof(cap_derive_req_k_t, resource_id)),
    ABI("kern:off:cap_derive_req_t.permissions",
        __builtin_offsetof(cap_derive_req_k_t, permissions)),
    ABI("kern:off:cap_derive_req_t.target_pid", __builtin_offsetof(cap_derive_req_k_t, target_pid)),
    ABI("kern:off:cap_derive_req_t.expiration", __builtin_offsetof(cap_derive_req_k_t, expiration)),
    ABI("kern:off:qpu_submit_req_t.circuit_len",
        __builtin_offsetof(qpu_submit_req_k_t, circuit_len)),
    ABI("kern:off:qpu_submit_req_t.circuit", __builtin_offsetof(qpu_submit_req_k_t, circuit)),
    ABI("kern:off:qpu_fetch_out_t.job_id", __builtin_offsetof(qpu_fetch_out_k_t, job_id)),
    ABI("kern:off:qpu_fetch_out_t.owner_pid", __builtin_offsetof(qpu_fetch_out_k_t, owner_pid)),
    ABI("kern:off:qpu_fetch_out_t.circuit_len", __builtin_offsetof(qpu_fetch_out_k_t, circuit_len)),
    ABI("kern:off:qpu_fetch_out_t.circuit", __builtin_offsetof(qpu_fetch_out_k_t, circuit)),
    ABI("kern:off:qpu_complete_req_t.job_id", __builtin_offsetof(qpu_complete_req_k_t, job_id)),
    ABI("kern:off:qpu_complete_req_t.status", __builtin_offsetof(qpu_complete_req_k_t, status)),
    ABI("kern:off:qpu_complete_req_t.result_len",
        __builtin_offsetof(qpu_complete_req_k_t, result_len)),
    ABI("kern:off:qpu_complete_req_t.result", __builtin_offsetof(qpu_complete_req_k_t, result)),
    ABI("kern:off:qpu_poll_out_t.state", __builtin_offsetof(qpu_poll_out_k_t, state)),
    ABI("kern:off:qpu_poll_out_t.status", __builtin_offsetof(qpu_poll_out_k_t, status)),
    ABI("kern:off:qpu_poll_out_t.result_len", __builtin_offsetof(qpu_poll_out_k_t, result_len)),
    ABI("kern:off:qpu_poll_out_t.result", __builtin_offsetof(qpu_poll_out_k_t, result)),
};
