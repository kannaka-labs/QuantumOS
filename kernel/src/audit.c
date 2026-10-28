/**
 * QuantumOS Capability Authority Ledger implementation (epic #133 — Phase D).
 * See kernel/include/kernel/audit.h for the model, scope, and concurrency rule.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/audit.h>
#include <kernel/capability.h>
#include <kernel/interrupts.h> /* timer_get_ticks */

_Static_assert(sizeof(audit_entry_t) == 40, "audit entry ABI drift");
_Static_assert(AUDIT_LOG_ENTRIES *AUDIT_LINE_MAX == AUDIT_MAX_BYTES, "audit buffer sizing");

/* The ring lives in .bss (zeroed → every slot is kind AUDIT_KIND_EMPTY until
 * written). `total` is the monotonic count of records ever ACCEPTED as new
 * (coalesced repeats do NOT advance it — they bump the last entry's count), so
 * seq == the entry's total-at-write and strictly increases down the log. */
static audit_entry_t ring[AUDIT_LOG_ENTRIES];
static uint64_t total; /* new records ever accepted; also the next seq */

/* Coalesce floor: the value of `total` at the last restore (0 on a cold boot).
 * audit_record never coalesces into a slot with seq < coalesce_floor, so the
 * first append after a restore cannot fold a boot-2 event into a durable
 * boot-1 tail entry (which would drop a real seq and mutate a persisted
 * record). Coalescing resumes normally among THIS boot's own entries. */
static uint64_t coalesce_floor;

/* Self-contained IRQ save/restore: audit_record is called from syscall context
 * AND the IF=1 service health monitor (service.c), so it must not assume a cli'd
 * caller. Single CPU, so disabling interrupts around the ~6-field append is a
 * sufficient mutual-exclusion (mirrors service.c's svc_irq_save). */
static inline uint64_t audit_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void audit_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

void audit_record(uint16_t kind, uint32_t pid, uint16_t verdict, uint32_t resource_type,
                  uint32_t resource_id, uint32_t permissions) {
    uint64_t flags = audit_irq_save();

    /* Coalesce: an identical record repeated back-to-back (a denial flood) bumps
     * the most-recent entry's count instead of appending, so it cannot evict
     * older GRANT/DENY evidence. `total`/seq do not advance on a coalesce.
     * The `> coalesce_floor` guard (not `> 0`) seals the restore boundary: a
     * freshly-restored ledger has total == coalesce_floor, so the first
     * post-restore append force-appends a new seq rather than mutating the
     * durable prior-boot tail entry. */
    if (total > coalesce_floor) {
        audit_entry_t *last = &ring[(uint32_t)((total - 1) % AUDIT_LOG_ENTRIES)];
        if (last->kind == kind && last->pid == pid && last->verdict == verdict &&
            last->resource_type == resource_type && last->resource_id == resource_id &&
            last->permissions == permissions) {
            last->count++;
            last->tick = timer_get_ticks(); /* refresh to the latest occurrence */
            audit_irq_restore(flags);
            return;
        }
    }

    audit_entry_t *e = &ring[(uint32_t)(total % AUDIT_LOG_ENTRIES)];
    e->seq = total;
    e->tick = timer_get_ticks();
    e->pid = pid;
    e->kind = kind;
    e->verdict = verdict;
    e->resource_type = resource_type;
    e->resource_id = resource_id;
    e->permissions = permissions;
    e->count = 1;
    total++;

    audit_irq_restore(flags);
}

void audit_grant(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions) {
    audit_record(AUDIT_GRANT, pid, AUDIT_V_OK, resource_type, resource_id, permissions);
}

void audit_deny(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions) {
    audit_record(AUDIT_DENY, pid, AUDIT_V_EPERM, resource_type, resource_id, permissions);
}

void audit_spawn(uint32_t parent_pid, uint32_t child_pid) {
    /* resource_id carries the child pid; PROCESS/EXECUTE mirrors the spawn cap. */
    audit_record(AUDIT_SPAWN, parent_pid, AUDIT_V_OK, CAP_RESOURCE_PROCESS, child_pid, CAP_EXECUTE);
}

void audit_revoke(uint32_t pid, uint32_t resource_type, uint32_t resource_id,
                  uint32_t permissions) {
    audit_record(AUDIT_REVOKE, pid, AUDIT_V_OK, resource_type, resource_id, permissions);
}

void audit_reap(uint32_t pid, uint32_t resource_type, uint32_t resource_id, uint32_t permissions) {
    audit_record(AUDIT_REAP, pid, AUDIT_V_OK, resource_type, resource_id, permissions);
}

/* UNLINK records the SURVIVING owner (the peer left holding a channel half
 * whose target died) — never the dead pid, which is already carried in
 * resource_id. See audit.h: REAP must keep meaning "owner died". */
void audit_unlink(uint32_t pid, uint32_t resource_type, uint32_t resource_id,
                  uint32_t permissions) {
    audit_record(AUDIT_UNLINK, pid, AUDIT_V_OK, resource_type, resource_id, permissions);
}

/* ---- freestanding formatting (no libc) ---- */
static size_t put_str(char *b, size_t o, size_t max, const char *s) {
    while (*s && o < max - 1) {
        b[o++] = *s++;
    }
    return o;
}

static size_t put_u64(char *b, size_t o, size_t max, uint64_t v) {
    char t[20];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v > 0 && n < 20) {
        t[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && o < max - 1) {
        b[o++] = t[--n];
    }
    return o;
}

static size_t put_hex8(char *b, size_t o, size_t max, uint32_t v) {
    if (o < max - 1) {
        b[o++] = '0';
    }
    if (o < max - 1) {
        b[o++] = 'x';
    }
    char t[8];
    int n = 0;
    if (v == 0) {
        t[n++] = '0';
    }
    while (v > 0 && n < 8) {
        uint32_t d = v & 0xF;
        t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    while (n > 0 && o < max - 1) {
        b[o++] = t[--n];
    }
    return o;
}

static const char *kind_name(uint16_t k) {
    switch (k) {
    case AUDIT_GRANT:
        return "GRANT";
    case AUDIT_DENY:
        return "DENY";
    case AUDIT_SPAWN:
        return "SPAWN";
    case AUDIT_MDENY:
        return "MDENY";
    case AUDIT_QUOTA:
        return "QUOTA";
    case AUDIT_CPUKILL:
        return "CPUKILL";
    case AUDIT_REVOKE:
        return "REVOKE";
    case AUDIT_REAP:
        return "REAP";
    case AUDIT_QSUBMIT:
        return "QSUBMIT";
    case AUDIT_UNLINK:
        return "UNLINK";
    case AUDIT_EGRESS:
        return "EGRESS";
    case AUDIT_KIND_EMPTY:
        return "EMPTY";
    default:
        return "?"; /* an unknown kind must be VISIBLY unknown, not EMPTY */
    }
}

static const char *type_name(uint32_t t) {
    /* Mirrors cap_resource_type_t (capability.h). */
    static const char *const names[] = {"MEM", "IPC", "DEV", "QRNG", "PROC", "SVC", "FIELD"};
    return t < (sizeof(names) / sizeof(names[0])) ? names[t] : "?";
}

static size_t format_entry(char *b, size_t o, size_t max, const audit_entry_t *e) {
    o = put_str(b, o, max, "AUDIT: seq=");
    o = put_u64(b, o, max, e->seq);
    o = put_str(b, o, max, " tick=");
    o = put_u64(b, o, max, e->tick);
    o = put_str(b, o, max, " pid=");
    o = put_u64(b, o, max, e->pid);
    o = put_str(b, o, max, " kind=");
    o = put_str(b, o, max, kind_name(e->kind));
    o = put_str(b, o, max, " res=");
    o = put_str(b, o, max, type_name(e->resource_type));
    o = put_str(b, o, max, ":");
    if (e->resource_id == AUDIT_RESOURCE_ANY) {
        o = put_str(b, o, max, "*");
    } else {
        o = put_u64(b, o, max, e->resource_id);
    }
    o = put_str(b, o, max, " perms=");
    o = put_hex8(b, o, max, e->permissions);
    o = put_str(b, o, max, " verdict=");
    o = put_str(b, o, max, e->verdict == AUDIT_V_EPERM ? "EPERM" : "OK");
    o = put_str(b, o, max, " count=");
    o = put_u64(b, o, max, e->count);
    o = put_str(b, o, max, "\r\n");
    return o;
}

/* Commit a whole line or nothing (line-atomicity, the manifest_format idiom): a
 * clipped "perms=0x1f" -> "perms=0x1" would parse host-side as a WRONG value, so
 * a straddling entry is dropped entirely and a "truncated=1" tail says so. */
static size_t audit_put_line(char *buf, size_t o, size_t budget, const char *line, size_t linelen) {
    if (o + linelen >= budget) {
        return (size_t)-1;
    }
    for (size_t i = 0; i < linelen; i++) {
        buf[o + i] = line[i];
    }
    return o + linelen;
}

size_t audit_format(char *buf, size_t max) {
    static const char tail[] = "AUDIT: truncated=1\r\n";
    const size_t tail_len = sizeof(tail) - 1;
    if (!buf || max < tail_len + 1) {
        return 0;
    }
    /* Reserve the tail up front so "truncated=1" itself always fits. */
    size_t budget = max - tail_len - 1;
    uint64_t flags = audit_irq_save();
    uint64_t live = total < AUDIT_LOG_ENTRIES ? total : AUDIT_LOG_ENTRIES;
    /* Oldest first: when wrapped, the oldest lives at total % CAP. Never touch
     * an unpopulated slot (live bounds the walk). */
    uint64_t start = total <= AUDIT_LOG_ENTRIES ? 0 : (total % AUDIT_LOG_ENTRIES);
    size_t o = 0;
    int truncated = 0;
    for (uint64_t i = 0; i < live; i++) {
        const audit_entry_t *e = &ring[(uint32_t)((start + i) % AUDIT_LOG_ENTRIES)];
        /* Format into a scratch line (generously sized so a large seq/tick can
         * never clip mid-field here), then commit whole-or-nothing. */
        char line[200];
        size_t l = format_entry(line, 0, sizeof(line), e);
        size_t r = audit_put_line(buf, o, budget, line, l);
        if (r == (size_t)-1) {
            truncated = 1;
            break;
        }
        o = r;
    }
    audit_irq_restore(flags);
    if (truncated) {
        o = put_str(buf, o, max, tail);
    }
    buf[o < max ? o : max - 1] = '\0';
    return o;
}

size_t audit_format_stats(char *buf, size_t max) {
    if (!buf || max == 0) {
        return 0;
    }
    uint64_t flags = audit_irq_save();
    uint64_t t = total;
    audit_irq_restore(flags);
    uint64_t dropped = t > AUDIT_LOG_ENTRIES ? t - AUDIT_LOG_ENTRIES : 0;
    size_t o = 0;
    o = put_str(buf, o, max, "AUDIT: total=");
    o = put_u64(buf, o, max, t);
    o = put_str(buf, o, max, " dropped=");
    o = put_u64(buf, o, max, dropped);
    o = put_str(buf, o, max, " capacity=");
    o = put_u64(buf, o, max, AUDIT_LOG_ENTRIES);
    o = put_str(buf, o, max, "\r\n");
    buf[o < max ? o : max - 1] = '\0';
    return o;
}

/* ---- durable persistence (across reboot) ---- */

/* Little-endian binary header helpers (distinct from the decimal put_u64 above,
 * which formats human-readable text). */
static void blob_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t blob_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t audit_serialize(uint8_t *buf, uint32_t buf_len) {
    if (!buf || buf_len < AUDIT_BLOB_BUFSZ) {
        return 0;
    }
    /* Zero the WHOLE sector-rounded buffer first: the tail padding lands on
     * disk and must never carry stale kernel memory (mirrors field_blob_write). */
    for (uint32_t i = 0; i < AUDIT_BLOB_BUFSZ; i++) {
        buf[i] = 0;
    }
    uint64_t flags = audit_irq_save();
    blob_put_u32(buf + 0, AUDIT_BLOB_MAGIC);
    blob_put_u32(buf + 4, AUDIT_BLOB_VERSION);
    blob_put_u32(buf + 8, AUDIT_LOG_ENTRIES);
    blob_put_u32(buf + 12, (uint32_t)sizeof(audit_entry_t));
    blob_put_u32(buf + 16, (uint32_t)(total & 0xFFFFFFFFu));
    blob_put_u32(buf + 20, (uint32_t)(total >> 32));
    /* buf+24, buf+28 reserved, already zero. */
    const uint8_t *src = (const uint8_t *)ring;
    for (uint32_t i = 0; i < AUDIT_LOG_ENTRIES * sizeof(audit_entry_t); i++) {
        buf[AUDIT_BLOB_HDR + i] = src[i];
    }
    audit_irq_restore(flags);
    return AUDIT_BLOB_BYTES;
}

int audit_load(const uint8_t *buf, uint32_t len) {
    if (!buf || len < AUDIT_BLOB_BYTES) {
        return -1;
    }
    /* Geometry guard: the header must describe EXACTLY this build's ledger
     * (mirrors field_blob_load) — an older layout or scribbled sector means
     * nothing here is trustworthy, and nothing has been touched yet. */
    if (blob_get_u32(buf + 0) != AUDIT_BLOB_MAGIC || blob_get_u32(buf + 4) != AUDIT_BLOB_VERSION ||
        blob_get_u32(buf + 8) != AUDIT_LOG_ENTRIES ||
        blob_get_u32(buf + 12) != (uint32_t)sizeof(audit_entry_t)) {
        return -1;
    }
    uint64_t restored_total =
        (uint64_t)blob_get_u32(buf + 16) | ((uint64_t)blob_get_u32(buf + 20) << 32);
    /* total is a monotonic event count; an implausibly large value (a scribbled
     * blob that survived the checksum by collision, or a future bug) would wrap
     * to 0 on the next append and corrupt the seq/dropped math. 2^48 is
     * astronomically above any real boot — reject and cold-start above it. */
    if (restored_total > ((uint64_t)1 << 48)) {
        return -1;
    }

    uint64_t flags = audit_irq_save();
    uint8_t *dst = (uint8_t *)ring;
    for (uint32_t i = 0; i < AUDIT_LOG_ENTRIES * sizeof(audit_entry_t); i++) {
        dst[i] = buf[AUDIT_BLOB_HDR + i];
    }
    total = restored_total;
    /* Seal the restore boundary: the first post-restore append must not
     * coalesce into (and mutate) the durable prior-boot tail entry. */
    coalesce_floor = total;
    /* tick is boot-relative (timer_get_ticks resets to 0 each boot); zero it on
     * restore so the cross-boot log is not temporally misleading. seq is the
     * sole durable ordering key (mirrors field.c imprint_tick reset). */
    for (uint32_t i = 0; i < AUDIT_LOG_ENTRIES; i++) {
        ring[i].tick = 0;
    }
    audit_irq_restore(flags);
    return 0;
}

bool audit_ring_has_kind(uint16_t kind) {
    uint64_t flags = audit_irq_save();
    bool found = false;
    for (uint32_t i = 0; i < AUDIT_LOG_ENTRIES; i++) {
        if (ring[i].kind == kind) {
            found = true;
            break;
        }
    }
    audit_irq_restore(flags);
    return found;
}
