/**
 * QuantumOS Capability Authority Ledger (epic #133 — Phase D)
 *
 * A kernel-authoritative, append-only record of capability GRANTs, DENIALs, and
 * SPAWNs. The KERNEL writes it — a ring-3 citizen cannot forge or suppress its
 * own entry — so an external verifier can see what authority each agent holds
 * and every attempt to exceed it. This is the "conscience before wallet"
 * substrate: reliability = faithfulness to granted intent, made observable.
 *
 * HONEST SCOPE: this is an AUTHORITY LEDGER, NOT a full syscall/action trace.
 * It records GRANT (a capability minted for a pid), DENY (a capability-gated
 * syscall refused because the caller lacked the right), SPAWN (a process
 * created), REVOKE (a capability explicitly withdrawn via cap_revoke, incl.
 * its cascade), REAP (slots freed because their owner died — the reaper's
 * cap_revoke_all_for_process + its cascade), and UNLINK (a spawn-channel cap
 * freed under its SURVIVING owner because the channel's TARGET died — epic
 * #175; never conflated with REAP). It does NOT record successful
 * exercise of already-held authority (two exceptions, both quota-bounded and
 * coalescible: SPAWN, and QSUBMIT — a quantum job queued, the QPU WRITE right
 * exercised, parallel to SPAWN). EXPIRE is still not recorded: expired
 * caps are never freed — they are refused at check/find/derive time and the
 * slot is reclaimed only by owner death (recorded then as REAP), so the
 * absence of an EXPIRE entry does not mean the cap was live until revoked.
 * There is no TRANSFER kind: capability ownership is never moved in place (the
 * unwired cap_transfer helper was deleted — a re-introduction MUST emit
 * REVOKE(from) + GRANT(to) so the move is visible). GRANT/
 * REVOKE/REAP record the pid whose HOLDINGS changed, NOT the requester — a
 * forced ancestor revoke is indistinguishable from a self-revoke; before
 * SYS_CAP_REVOKE is ever exposed to ring 3, the denied path MUST audit_deny
 * and the forced path needs an actor-bearing record. DENY coverage now spans
 * every gated authority site: the authorize() helper records a DENY for all
 * device/disk/net/field caps (a capless SYS_UDP/TCP/RESOLVE is a DEV:NET DENY),
 * and IPC send/send_to — which use raw cap_find, not authorize() — were the last
 * gap, now closed (a capless send records an IPC DENY). Remaining non-coverage
 * is by design, not omission: successful use is unrecorded (except the
 * quota-bounded SPAWN/QSUBMIT), and there is no EXPIRE/TRANSFER kind.
 *
 * CONCURRENCY: audit_record is called from BOTH syscall context AND the IF=1
 * service health monitor (a watchdog-reborn citizen re-mints caps from that
 * thread — service.c). So unlike field.c it does NOT assume a cli'd caller: the
 * ring append + monotonic seq bump are self-contained under a local
 * irqsave/irqrestore. Single CPU, so that suffices.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNEL_AUDIT_H
#define KERNEL_AUDIT_H

#include <kernel/types.h>

/* Ring capacity (evictable, oldest-first). REVOKE/REAP symmetry roughly
 * doubles the steady-state entry rate vs GRANT-only (a qsh watchdog rebirth is
 * ~20 entries: ~9 REAPs + ~9 re-GRANTs + SPAWN), so the boot sequence alone now
 * exceeds 128 (~143 measured) — 256 keeps the boot-time MDENY/QUOTA/selftest
 * evidence inside the window the CI gates read. Geometry is in the durable
 * blob header, so audit_load honestly cold-starts a 128-era disk. */
#define AUDIT_LOG_ENTRIES 256
#define AUDIT_LINE_MAX 96 /* max formatted bytes per entry line */
#define AUDIT_MAX_BYTES (AUDIT_LOG_ENTRIES * AUDIT_LINE_MAX) /* one-shot text buffer */

/* Record kinds — 1-based so a zeroed .bss slot (kind 0) is never a real entry. */
#define AUDIT_KIND_EMPTY 0
#define AUDIT_GRANT 1   /* a capability was minted for pid (cap_create/cap_derive) */
#define AUDIT_DENY 2    /* a cap-gated syscall was refused (caller lacked the right) */
#define AUDIT_SPAWN 3   /* a process was created (EXECUTE authority exercised) */
#define AUDIT_MDENY 4   /* a HELD capability exceeded declared intent (manifest deny) */
#define AUDIT_QUOTA 5   /* a manifest quota refused the operation (e.g. spawn_max) */
#define AUDIT_CPUKILL 6 /* a process was TERMINATED for exceeding its cpu_limit (epic #144) */
#define AUDIT_REVOKE 7  /* a capability was EXPLICITLY withdrawn (cap_revoke + its cascade) */
#define AUDIT_REAP 8    /* slots freed because their OWNER DIED (reaper cleanup + cascade) */
#define AUDIT_QSUBMIT 9 /* a quantum job was queued (QPU WRITE authority exercised, #148) */
#define AUDIT_UNLINK                                                                               \
    10 /* a spawn-channel cap freed because its TARGET pid died (epic #175).
                         * Recorded under the SURVIVING owner — never REAP, which must keep
                         * meaning "owner died"; resource_id (== the dead pid) carries the
                         * death linkage for verifiers. */
#define AUDIT_EGRESS                                                                               \
    11 /* an egress verdict or grant event (ADR-0030): pid 0 + DEVICE_ID_NET +
                         * permissions 1|2|3 = granted|expired|budget spent; otherwise the
                         * sender's pid, resource_id = destination IPv4 as a u32 (0 for a
                         * listen check), permissions = (proto << 16) | port, verdict OK or
                         * EPERM. The receipt: attempted and allowed are both on the record. */

/* Verdicts — 1-based likewise. */
#define AUDIT_V_OK 1
#define AUDIT_V_EPERM 2

/* A cap_find (any-match) denial names no specific resource. */
#define AUDIT_RESOURCE_ANY 0xFFFFFFFFu

/* SYS_AUDIT operations (rdi). */
#define AUDIT_OP_READ 0  /* format all live entries (oldest->newest) into the buf */
#define AUDIT_OP_STATS 1 /* one line: AUDIT: total=<seq> dropped=<n> capacity=<cap> */

typedef struct {
    uint64_t seq;           /* monotonic; strictly increases across the log's life */
    uint64_t tick;          /* boot-relative timer tick of the record */
    uint32_t pid;           /* the acting/owning process */
    uint16_t kind;          /* AUDIT_GRANT/DENY/SPAWN */
    uint16_t verdict;       /* AUDIT_V_OK / AUDIT_V_EPERM */
    uint32_t resource_type; /* cap_resource_type_t; SPAWN uses PROCESS */
    uint32_t resource_id;   /* OPAQUE handle — NEVER an address; ANY = 0xFFFFFFFF */
    uint32_t permissions;   /* CAP_READ/WRITE/EXECUTE/... bits attempted/granted */
    uint32_t count;         /* coalesced repeats: N identical consecutive records */
} audit_entry_t;            /* 40 bytes */

/* Record one authority event. Self-contained (irqsave); safe from any context.
 * Coalesces a record identical to the most-recent one into its `count` so a
 * denial flood cannot evict older GRANT/DENY evidence in O(flood). */
void audit_record(uint16_t kind, uint32_t pid, uint16_t verdict, uint32_t resource_type,
                  uint32_t resource_id, uint32_t permissions);

/* Convenience wrappers used at the hook sites. Like GRANT, revoke/reap record
 * the pid whose holdings changed (the cap's OWNER), not the requester. */
void audit_grant(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);
void audit_deny(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);
void audit_spawn(uint32_t parent_pid, uint32_t child_pid);
void audit_revoke(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);
void audit_reap(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);
void audit_unlink(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions);

/* Format all live entries oldest->newest as "AUDIT: ..." text lines into buf
 * (bounded, NUL-terminated). Returns bytes written (excluding the NUL). */
size_t audit_format(char *buf, size_t max);

/* One "AUDIT: total=<seq> dropped=<n> capacity=<cap>" line. dropped is guarded
 * (0 while total <= capacity; total-capacity once the ring has wrapped). */
size_t audit_format_stats(char *buf, size_t max);

/* ---- durable persistence (across reboot; reuses the epic-#71 disk layer) ----
 *
 * The ledger is serialized to a FIXED disk home as a small LE header + the
 * raw whole-ring image, so a restore reconstructs the exact modular ring state
 * and `seq` (total) CONTINUES across reboots — a genuine append-history, not a
 * per-boot scratchpad. `tick` is boot-relative and is zeroed on restore; `seq`
 * is the sole durable ordering key. */
#define AUDIT_BLOB_MAGIC 0x31445541u /* 'AUD1' little-endian */
#define AUDIT_BLOB_VERSION 1u
#define AUDIT_BLOB_HDR 32u /* header bytes preceding the ring image */
#define AUDIT_BLOB_BYTES (AUDIT_BLOB_HDR + AUDIT_LOG_ENTRIES * sizeof(audit_entry_t)) /* 10272 */
#define AUDIT_BLOB_SECTORS ((AUDIT_BLOB_BYTES + 511u) / 512u)                         /* 21 */
#define AUDIT_BLOB_BUFSZ (AUDIT_BLOB_SECTORS * 512u)                                  /* 10752 */

/* Serialize the whole ledger (header + ring) into buf. buf must be at least
 * AUDIT_BLOB_BUFSZ bytes; the WHOLE sector-rounded buffer is zeroed first so no
 * kernel tail-padding leaks to disk. Returns AUDIT_BLOB_BYTES, or 0 if too small.
 * Snapshots the ring under irqsave (audit_record can fire from the timer IRQ). */
uint32_t audit_serialize(uint8_t *buf, uint32_t buf_len);

/* Load a serialized ledger: validate magic/version/geometry (mismatch -> -1,
 * caller cold-starts), then restore the ring + total and seal the coalesce
 * boundary so the first post-restore append cannot fold into a prior-boot
 * entry. tick is zeroed (boot-relative). Returns 0 on success, -1 on mismatch. */
int audit_load(const uint8_t *buf, uint32_t len);

/* True if any live ring slot carries `kind`. Used at restore time to prove a
 * specific prior-boot event survived (the restore runs before this boot emits
 * any audit entry, so a hit can only have come from disk). */
bool audit_ring_has_kind(uint16_t kind);

#endif /* KERNEL_AUDIT_H */
