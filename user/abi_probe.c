/**
 * QuantumOS v1 ABI freeze probe - USER ring (ADR-0020).
 *
 * This translation unit is NEVER linked or run. It is compiled to an object
 * file under the REAL user build flags (USER_CFLAGS); scripts/extract-abi.py
 * reads the compiler-measured values back out of the .abi_ents section with
 * objdump -s. Emitting compiler-measured values (macro expansions, sizeof) -
 * rather than parsing the size-claim comments or the _Static_assert literals -
 * is the whole point: a developer editing a struct and its size-claim comment
 * together cannot fool the golden, because the golden is checked against what
 * the compiler actually lays out.
 *
 * Each entry is a fixed 64-byte record: a 56-char NUL-padded name plus a
 * little-endian u64 value. Names are ring-namespaced (user: here) so a symbol
 * that also exists on the kernel side never collides in the merged table.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "usys.h"

/* Teeth-check hook (inert in a normal build): the ABI gate's --selftest
 * recompiles this probe with -DSYS_QPU_OVERRIDE=<n> and asserts the gate
 * reddens, proving a CHANGED ABI value is actually caught (not just an added or
 * removed name). #undef-then-#define avoids a redefinition diagnostic. */
#ifdef SYS_QPU_OVERRIDE
#undef SYS_QPU
#define SYS_QPU SYS_QPU_OVERRIDE
#endif

/* A wrong data model (ILP32/LLP64) would silently mis-measure every size; fail
 * the compile loudly instead of emitting a plausible-but-wrong 24. */
_Static_assert(sizeof(long) == 8 && sizeof(void *) == 8, "wrong data model / ABI");

struct abi_ent {
    char name[56];
    unsigned long long value;
};

#define ABI(n, v) {n, (unsigned long long)(v)}

__attribute__((used, section(".abi_ents"))) const struct abi_ent abi_user[] = {
    /* --- syscall numbers (1..35) --- */
    ABI("user:num:SYS_WRITE", SYS_WRITE),
    ABI("user:num:SYS_GETPID", SYS_GETPID),
    ABI("user:num:SYS_YIELD", SYS_YIELD),
    ABI("user:num:SYS_EXIT", SYS_EXIT),
    ABI("user:num:SYS_TICKS", SYS_TICKS),
    ABI("user:num:SYS_SEND", SYS_SEND),
    ABI("user:num:SYS_RECV", SYS_RECV),
    ABI("user:num:SYS_HEARTBEAT", SYS_HEARTBEAT),
    ABI("user:num:SYS_SVC_RESTARTS", SYS_SVC_RESTARTS),
    ABI("user:num:SYS_QRAND", SYS_QRAND),
    ABI("user:num:SYS_SEND_TO", SYS_SEND_TO),
    ABI("user:num:SYS_COM2", SYS_COM2),
    ABI("user:num:SYS_QSEED", SYS_QSEED),
    ABI("user:num:SYS_FIELD_SNAPSHOT", SYS_FIELD_SNAPSHOT),
    ABI("user:num:SYS_CONS", SYS_CONS),
    ABI("user:num:SYS_SYSINFO", SYS_SYSINFO),
    ABI("user:num:SYS_OPEN", SYS_OPEN),
    ABI("user:num:SYS_READ", SYS_READ),
    ABI("user:num:SYS_CLOSE", SYS_CLOSE),
    ABI("user:num:SYS_READDIR", SYS_READDIR),
    ABI("user:num:SYS_SPAWN", SYS_SPAWN),
    ABI("user:num:SYS_WAITPID", SYS_WAITPID),
    ABI("user:num:SYS_FWRITE", SYS_FWRITE),
    ABI("user:num:SYS_UNLINK", SYS_UNLINK),
    ABI("user:num:SYS_SYNC", SYS_SYNC),
    ABI("user:num:SYS_RESOLVE", SYS_RESOLVE),
    ABI("user:num:SYS_UDP", SYS_UDP),
    ABI("user:num:SYS_TCP", SYS_TCP),
    ABI("user:num:SYS_IMPRINT", SYS_IMPRINT),
    ABI("user:num:SYS_RECALL", SYS_RECALL),
    ABI("user:num:SYS_FIELD_INFO", SYS_FIELD_INFO),
    ABI("user:num:SYS_AUDIT", SYS_AUDIT),
    ABI("user:num:SYS_MANIFEST", SYS_MANIFEST),
    ABI("user:num:SYS_CAP_DERIVE", SYS_CAP_DERIVE),
    ABI("user:num:SYS_QPU", SYS_QPU),

    /* --- sub-op namespaces (the second dispatch layer) --- */
    ABI("user:subop:SYS_COM2_READ", SYS_COM2_READ),
    ABI("user:subop:SYS_COM2_WRITE", SYS_COM2_WRITE),
    ABI("user:subop:SYS_CONS_READ", SYS_CONS_READ),
    ABI("user:subop:SYS_CONS_WRITE", SYS_CONS_WRITE),
    ABI("user:subop:SYSINFO_PS", SYSINFO_PS),
    ABI("user:subop:SYSINFO_MEM", SYSINFO_MEM),
    ABI("user:subop:SYSINFO_TIME", SYSINFO_TIME),
    ABI("user:subop:SYSINFO_QUIET", SYSINFO_QUIET),
    ABI("user:subop:SYSINFO_PEER", SYSINFO_PEER),
    ABI("user:subop:SYSINFO_PEER_COUNT", SYSINFO_PEER_COUNT),
    ABI("user:subop:UDP_BIND", UDP_BIND),
    ABI("user:subop:UDP_SENDTO", UDP_SENDTO),
    ABI("user:subop:UDP_RECVFROM", UDP_RECVFROM),
    ABI("user:subop:UDP_CLOSE", UDP_CLOSE),
    ABI("user:subop:TCP_CONNECT", TCP_CONNECT),
    ABI("user:subop:TCP_SEND", TCP_SEND),
    ABI("user:subop:TCP_RECV", TCP_RECV),
    ABI("user:subop:TCP_CLOSE", TCP_CLOSE),
    ABI("user:subop:TCP_STATUS", TCP_STATUS),
    ABI("user:subop:TCP_LISTEN", TCP_LISTEN),
    ABI("user:subop:TCP_ACCEPT", TCP_ACCEPT),
    ABI("user:subop:AUDIT_OP_READ", AUDIT_OP_READ),
    ABI("user:subop:AUDIT_OP_STATS", AUDIT_OP_STATS),
    ABI("user:subop:QPU_OP_SUBMIT", QPU_OP_SUBMIT),
    ABI("user:subop:QPU_OP_FETCH", QPU_OP_FETCH),
    ABI("user:subop:QPU_OP_COMPLETE", QPU_OP_COMPLETE),
    ABI("user:subop:QPU_OP_POLL", QPU_OP_POLL),

    /* --- in-band value enums + flags --- */
    ABI("user:enum:O_RDONLY", O_RDONLY),
    ABI("user:enum:O_WRONLY", O_WRONLY),
    ABI("user:enum:O_CREAT", O_CREAT),
    ABI("user:enum:O_TRUNC", O_TRUNC),

    /* --- return-channel sentinels (part of the errno contract) --- */
    ABI("user:errno:RESOLVE_WOULDBLOCK", (unsigned long long)RESOLVE_WOULDBLOCK),
    ABI("user:errno:UDP_WOULDBLOCK", (unsigned long long)UDP_WOULDBLOCK),
    ABI("user:errno:WAITPID_RUNNING", WAITPID_RUNNING),

    /* --- ring-crossing struct sizes (compiler-measured) --- */
    ABI("user:size:udp_req_t", sizeof(udp_req_t)),
    ABI("user:size:tcp_req_t", sizeof(tcp_req_t)),
    ABI("user:size:field_imprint_req_t", sizeof(field_imprint_req_t)),
    ABI("user:size:field_recall_req_t", sizeof(field_recall_req_t)),
    ABI("user:size:field_recall_out_t", sizeof(field_recall_out_t)),
    ABI("user:size:field_slot_info_t", sizeof(field_slot_info_t)),
    ABI("user:size:field_info_out_t", sizeof(field_info_out_t)),
    ABI("user:size:cap_derive_req_t", sizeof(cap_derive_req_t)),
    ABI("user:size:qpu_submit_req_t", sizeof(qpu_submit_req_t)),
    ABI("user:size:qpu_fetch_out_t", sizeof(qpu_fetch_out_t)),
    ABI("user:size:qpu_complete_req_t", sizeof(qpu_complete_req_t)),
    ABI("user:size:qpu_poll_out_t", sizeof(qpu_poll_out_t)),
    ABI("user:size:user_args_t", sizeof(user_args_t)),

    /* --- ring-crossing struct field offsets (a size-preserving field
     * reorder passes the size check but reddens here) --- */
    ABI("user:off:udp_req_t.sock", __builtin_offsetof(udp_req_t, sock)),
    ABI("user:off:udp_req_t.ip", __builtin_offsetof(udp_req_t, ip)),
    ABI("user:off:udp_req_t.port", __builtin_offsetof(udp_req_t, port)),
    ABI("user:off:udp_req_t.len", __builtin_offsetof(udp_req_t, len)),
    ABI("user:off:udp_req_t.buf", __builtin_offsetof(udp_req_t, buf)),
    ABI("user:off:cap_derive_req_t.resource_type",
        __builtin_offsetof(cap_derive_req_t, resource_type)),
    ABI("user:off:cap_derive_req_t.resource_id", __builtin_offsetof(cap_derive_req_t, resource_id)),
    ABI("user:off:cap_derive_req_t.permissions", __builtin_offsetof(cap_derive_req_t, permissions)),
    ABI("user:off:cap_derive_req_t.target_pid", __builtin_offsetof(cap_derive_req_t, target_pid)),
    ABI("user:off:cap_derive_req_t.expiration", __builtin_offsetof(cap_derive_req_t, expiration)),
    ABI("user:off:qpu_submit_req_t.circuit_len", __builtin_offsetof(qpu_submit_req_t, circuit_len)),
    ABI("user:off:qpu_submit_req_t.circuit", __builtin_offsetof(qpu_submit_req_t, circuit)),
    ABI("user:off:qpu_fetch_out_t.job_id", __builtin_offsetof(qpu_fetch_out_t, job_id)),
    ABI("user:off:qpu_fetch_out_t.owner_pid", __builtin_offsetof(qpu_fetch_out_t, owner_pid)),
    ABI("user:off:qpu_fetch_out_t.circuit_len", __builtin_offsetof(qpu_fetch_out_t, circuit_len)),
    ABI("user:off:qpu_fetch_out_t.circuit", __builtin_offsetof(qpu_fetch_out_t, circuit)),
    ABI("user:off:qpu_complete_req_t.job_id", __builtin_offsetof(qpu_complete_req_t, job_id)),
    ABI("user:off:qpu_complete_req_t.status", __builtin_offsetof(qpu_complete_req_t, status)),
    ABI("user:off:qpu_complete_req_t.result_len",
        __builtin_offsetof(qpu_complete_req_t, result_len)),
    ABI("user:off:qpu_complete_req_t.result", __builtin_offsetof(qpu_complete_req_t, result)),
    ABI("user:off:qpu_poll_out_t.state", __builtin_offsetof(qpu_poll_out_t, state)),
    ABI("user:off:qpu_poll_out_t.status", __builtin_offsetof(qpu_poll_out_t, status)),
    ABI("user:off:qpu_poll_out_t.result_len", __builtin_offsetof(qpu_poll_out_t, result_len)),
    ABI("user:off:qpu_poll_out_t.result", __builtin_offsetof(qpu_poll_out_t, result)),

    /* --- fixed-VA argument page constants --- */
    ABI("user:const:USER_ARGS_VADDR", USER_ARGS_VADDR),
    ABI("user:const:USER_ARGS_MAX", USER_ARGS_MAX),
    ABI("user:const:USER_ARGS_STRBYTES", USER_ARGS_STRBYTES),

    /* --- arg-page field offsets (the read-offset math is the contract) --- */
    ABI("user:off:user_args_t.argc", __builtin_offsetof(user_args_t, argc)),
    ABI("user:off:user_args_t.argv_off", __builtin_offsetof(user_args_t, argv_off)),
    ABI("user:off:user_args_t.strings", __builtin_offsetof(user_args_t, strings)),

    /* --- field bounds --- */
    ABI("user:const:FIELD_PAT_MAX", FIELD_PAT_MAX),
    ABI("user:const:FIELD_RANK_MAX", FIELD_RANK_MAX),
    ABI("user:const:FIELD_SLOTS", FIELD_SLOTS),
};
