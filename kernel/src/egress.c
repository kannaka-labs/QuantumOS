/**
 * The egress grant (ADR-0030, #252): networking is a grant, not a side effect of a NIC.
 *
 * One table, one check. The grant is parsed from the boot command line, or later from the
 * console escape a host sends with the key it booted the kernel with; the allowlist is
 * walked on every ring-3 send, every verdict goes to the audit ledger, and the byte
 * counters are advanced by netdev for every frame in either direction. When the ttl or
 * the byte budget is spent the grant is marked over, and netdev stops carrying frames
 * (netdev.c reads egress_active()).
 *
 * Single-core rules (ADR-0005): the live grant is read from the net thread and from
 * syscalls (IF=0) and replaced only by the net thread, with interrupts off for the copy,
 * so a syscall never sees a half-written table. The console escape arrives in interrupt
 * context and is only RECORDED there (egress_offer); the net thread parses, applies, logs
 * and receipts it (egress_apply_pending). Receipts go through audit_record, which handles
 * its own interrupt state.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/egress.h>
#include <kernel/audit.h>
#include <kernel/boot.h>
#include <kernel/capability.h>
#include <kernel/interrupts.h>
#include <kernel/net.h>

#define MAX_ALLOW 8
#define MAX_LISTEN 8
#define HOST_MAX 63
#define TICKS_PER_S 100u /* the PIT runs at 100 Hz (interrupts.c) */

/* Receipt rows (AUDIT_EGRESS) for the grant itself: pid 0, resource DEVICE_ID_NET,
 * permissions = one of these. */
#define GRANT_GIVEN 1
#define GRANT_EXPIRED 2
#define GRANT_BUDGET 3
#define GRANT_REFUSED 4 /* a console offer refused: no key, wrong key, or a bad spec */

typedef struct {
    uint8_t any;    /* `*`: every destination */
    uint8_t has_ip; /* ip[] is set (a literal, or a resolved host) */
    uint8_t ip[4];
    uint16_t port;
    uint8_t proto;           /* EGRESS_PROTO_UDP / EGRESS_PROTO_TCP */
    char host[HOST_MAX + 1]; /* "" for a literal entry */
} allow_t;

/* A parsed grant. Parsed into a local, then copied over `live` in one go. */
typedef struct {
    uint64_t ttl_ticks;
    uint64_t byte_budget;
    allow_t allow[MAX_ALLOW];
    int n_allow;
    uint16_t listen[MAX_LISTEN];
    int n_listen;
} grant_t;

static int configured;
static int over; /* expired or budget spent; sticky until the next grant */
static uint64_t start_tick;
static uint64_t bytes_used;
static grant_t live;

/* The host's key (egress_key=) and the offer mailbox the console escape writes. One slot:
 * a second offer before the first is applied replaces it, as a second grant would. */
static uint8_t key_set;
static uint8_t key[EGRESS_KEY_HEX];
static volatile int offer_pending;
static int offer_refused; /* OFFER_* reason, or 0 for a spec to parse */
static char offer_spec[EGRESS_SPEC_MAX + 1];
static uint32_t offer_len;

#define OFFER_NOKEY 1  /* no egress_key= on the command line: the escape is dead */
#define OFFER_BADKEY 2 /* the key did not match */
#define OFFER_SHAPE 3  /* payload is not <16 hex>;<spec> */

/* Interrupt guard for the two commits below (the audit.c idiom). */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static void zero_bytes(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int ip4_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static uint32_t ip_u32(const uint8_t ip[4]) {
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
}

static void receipt_grant(uint32_t what, uint32_t verdict) {
    audit_record(AUDIT_EGRESS, 0, verdict, CAP_RESOURCE_DEVICE, DEVICE_ID_NET, what);
}

/* Parse an unsigned decimal; returns the char after it, or 0 when none was read. */
static const char *parse_u64(const char *p, uint64_t *out) {
    uint64_t v = 0;
    const char *s = p;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
    }
    if (p == s) {
        return 0;
    }
    *out = v;
    return p;
}

/* A dotted quad; returns the char after it, or 0 when `s` is not one. */
static const char *parse_ip(const char *s, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) {
        uint64_t v;
        const char *n = parse_u64(s, &v);
        if (!n || v > 255 || (i < 3 && *n != '.')) {
            return 0;
        }
        out[i] = (uint8_t)v;
        s = (i < 3) ? n + 1 : n;
    }
    return s;
}

/* One allow entry: `*` | ip|host `:` port `/` udp|tcp, ending at ',' ';' or NUL. */
static const char *parse_allow(const char *s, allow_t *a) {
    zero_bytes(a, sizeof(*a));
    if (*s == '*' && (s[1] == ',' || s[1] == ';' || s[1] == '\0')) {
        a->any = 1;
        return s + 1;
    }
    const char *n = parse_ip(s, a->ip);
    if (n && *n == ':') {
        a->has_ip = 1;
    } else {
        int k = 0;
        n = s;
        while (*n && *n != ':' && *n != ',' && *n != ';') {
            if (k >= HOST_MAX) {
                return 0;
            }
            a->host[k++] = *n++;
        }
        a->host[k] = '\0';
        if (k == 0 || *n != ':') {
            return 0;
        }
    }
    uint64_t port;
    n = parse_u64(n + 1, &port);
    if (!n || port == 0 || port > 65535 || *n != '/') {
        return 0;
    }
    a->port = (uint16_t)port;
    n++;
    if (n[0] == 'u' && n[1] == 'd' && n[2] == 'p') {
        a->proto = EGRESS_PROTO_UDP;
        n += 3;
    } else if (n[0] == 't' && n[1] == 'c' && n[2] == 'p') {
        a->proto = EGRESS_PROTO_TCP;
        n += 3;
    } else {
        return 0;
    }
    return (*n == ',' || *n == ';' || *n == '\0') ? n : 0;
}

/* The whole spec into `g`; 0 on success, -1 (logged) on a field the kernel refuses. Touches
 * nothing but `g`, so a refused spec leaves the live grant exactly as it was. */
static int parse_grant(const char *spec, grant_t *g) {
    int have_ttl = 0, have_bytes = 0;
    zero_bytes(g, sizeof(*g));
    const char *p = spec;
    while (*p) {
        if (p[0] == 't' && p[1] == 't' && p[2] == 'l' && p[3] == '=') {
            uint64_t v;
            if (!(p = parse_u64(p + 4, &v)) || v == 0) {
                boot_log("NET: egress refused: bad ttl");
                return -1;
            }
            g->ttl_ticks = v * TICKS_PER_S;
            have_ttl = 1;
        } else if (p[0] == 'b' && p[1] == 'y' && p[2] == 't' && p[3] == 'e' && p[4] == 's' &&
                   p[5] == '=') {
            uint64_t v;
            if (!(p = parse_u64(p + 6, &v)) || v == 0) {
                boot_log("NET: egress refused: bad bytes");
                return -1;
            }
            g->byte_budget = v;
            have_bytes = 1;
        } else if (p[0] == 'a' && p[1] == 'l' && p[2] == 'l' && p[3] == 'o' && p[4] == 'w' &&
                   p[5] == '=') {
            p += 6;
            while (*p && *p != ';') {
                if (g->n_allow >= MAX_ALLOW || !(p = parse_allow(p, &g->allow[g->n_allow]))) {
                    boot_log("NET: egress refused: bad allow entry (ip|host:port/udp|tcp, or *)");
                    return -1;
                }
                g->n_allow++;
                if (*p == ',') {
                    p++;
                }
            }
        } else if (p[0] == 'l' && p[1] == 'i' && p[2] == 's' && p[3] == 't' && p[4] == 'e' &&
                   p[5] == 'n' && p[6] == '=') {
            p += 7;
            while (*p && *p != ';') {
                uint64_t v;
                if (g->n_listen >= MAX_LISTEN || !(p = parse_u64(p, &v)) || v == 0 || v > 65535) {
                    boot_log("NET: egress refused: bad listen port");
                    return -1;
                }
                g->listen[g->n_listen++] = (uint16_t)v;
                if (*p == ',') {
                    p++;
                }
            }
        } else {
            boot_log("NET: egress refused: unknown field (ttl, bytes, allow, listen)");
            return -1;
        }
        if (*p == ';') {
            p++;
        } else if (*p) {
            boot_log("NET: egress refused: fields are separated by ';'");
            return -1;
        }
    }
    if (!have_ttl || !have_bytes) {
        boot_log("NET: egress refused: ttl and bytes are both required");
        return -1;
    }
    return 0;
}

/* Parse, then commit in one go with interrupts off: a syscall's egress_check runs with
 * IF=0 and must never see half of an old table and half of a new one. */
static int configure_from(const char *spec, int from_console) {
    grant_t g;
    if (parse_grant(spec, &g) != 0) {
        return -1;
    }
    uint64_t flags = irq_save();
    live = g;
    start_tick = timer_get_ticks();
    bytes_used = 0;
    over = 0;
    configured = 1;
    irq_restore(flags);
    receipt_grant(GRANT_GIVEN, AUDIT_V_OK);
    boot_log(from_console
                 ? "NET: egress granted over the console escape (host key verified)"
                 : "NET: egress granted (ttl, byte budget, allowlist, listen ports from cmdline)");
    return 0;
}

int egress_configure(const char *spec) {
    return configure_from(spec, 0);
}

int egress_configured(void) {
    return configured;
}

static void quiesce(uint32_t why, const char *line) {
    if (!over) {
        over = 1;
        receipt_grant(why, AUDIT_V_OK);
        boot_log(line);
    }
}

int egress_active(void) {
    if (!configured || over) {
        return 0;
    }
    if (timer_get_ticks() - start_tick >= live.ttl_ticks) {
        quiesce(GRANT_EXPIRED, "NET: egress grant expired (ttl): networking off");
        return 0;
    }
    return 1;
}

void egress_account(uint32_t len) {
    if (!configured || over) {
        return;
    }
    bytes_used += len;
    if (bytes_used >= live.byte_budget) {
        quiesce(GRANT_BUDGET, "NET: egress grant exhausted (bytes): networking off");
    }
}

static int state_verdict(void) {
    if (!configured) {
        return EGRESS_E_NOGRANT;
    }
    if (!egress_active()) {
        return bytes_used >= live.byte_budget ? EGRESS_E_BUDGET : EGRESS_E_EXPIRED;
    }
    return EGRESS_OK;
}

static void receipt(uint32_t pid, int verdict, uint32_t dst, uint16_t port, uint8_t proto) {
    audit_record(AUDIT_EGRESS, pid, verdict == EGRESS_OK ? AUDIT_V_OK : AUDIT_V_EPERM,
                 CAP_RESOURCE_DEVICE, dst, ((uint32_t)proto << 16) | port);
}

int egress_check(uint32_t pid, const uint8_t dip[4], uint16_t dport, uint8_t proto) {
    int v = state_verdict();
    if (v == EGRESS_OK) {
        v = EGRESS_E_DEST;
        for (int i = 0; i < live.n_allow; i++) {
            const allow_t *a = &live.allow[i];
            if (a->any) {
                v = EGRESS_OK;
                break;
            }
            if (!a->has_ip || !ip4_eq(a->ip, dip)) {
                continue;
            }
            if (a->port == dport && a->proto == proto) {
                v = EGRESS_OK;
                break;
            }
            v = EGRESS_E_PORT; /* right host, wrong port: keep looking, remember why */
        }
    }
    receipt(pid, v, ip_u32(dip), dport, proto);
    return v;
}

int egress_listen_ok(uint32_t pid, uint16_t lport) {
    int v = state_verdict();
    if (v == EGRESS_OK) {
        v = EGRESS_E_LISTEN;
        for (int i = 0; i < live.n_listen; i++) {
            if (live.listen[i] == lport) {
                v = EGRESS_OK;
                break;
            }
        }
    }
    receipt(pid, v, 0, lport, 0);
    return v;
}

void egress_note_resolution(const char *host, const uint8_t ip[4]) {
    for (int i = 0; i < live.n_allow; i++) {
        if (live.allow[i].host[0] && str_eq(live.allow[i].host, host)) {
            for (int b = 0; b < 4; b++) {
                live.allow[i].ip[b] = ip[b];
            }
            live.allow[i].has_ip = 1;
        }
    }
}

const char *egress_reason(int verdict) {
    switch (verdict) {
    case EGRESS_OK:
        return "allowed";
    case EGRESS_E_NOGRANT:
        return "no egress grant";
    case EGRESS_E_EXPIRED:
        return "egress grant expired";
    case EGRESS_E_BUDGET:
        return "egress byte budget spent";
    case EGRESS_E_DEST:
        return "destination not in the egress allowlist";
    case EGRESS_E_PORT:
        return "port/proto not in the egress allowlist";
    case EGRESS_E_LISTEN:
        return "local port not in the egress listen list";
    default:
        return "egress refused";
    }
}

/* ---- the console escape (PR A2) ---------------------------------------------------- */

static int hex_ok(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static char hex_lower(char c) {
    return (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
}

void egress_set_key(const char *hex) {
    int n = 0;
    while (n <= EGRESS_KEY_HEX && hex[n]) {
        n++;
    }
    if (n != EGRESS_KEY_HEX) {
        boot_log("NET: egress_key refused: 16 hex digits expected (the escape stays dead)");
        return;
    }
    for (int i = 0; i < EGRESS_KEY_HEX; i++) {
        if (!hex_ok(hex[i])) {
            boot_log("NET: egress_key refused: 16 hex digits expected (the escape stays dead)");
            return;
        }
        key[i] = (uint8_t)hex_lower(hex[i]);
    }
    key_set = 1;
    boot_log("NET: egress_key set: a host may grant over the console escape");
}

void egress_offer(const char *payload, uint32_t len) {
    int refused = 0;
    if (!key_set) {
        refused = OFFER_NOKEY;
    } else if (len < EGRESS_KEY_HEX + 1 || payload[EGRESS_KEY_HEX] != ';') {
        refused = OFFER_SHAPE;
    } else {
        /* Every byte compared, whatever matched: the compare takes the same time for a
         * near miss as for a miss. */
        uint8_t diff = 0;
        for (int i = 0; i < EGRESS_KEY_HEX; i++) {
            diff |= (uint8_t)(key[i] ^ (uint8_t)hex_lower(payload[i]));
        }
        if (diff) {
            refused = OFFER_BADKEY;
        }
    }
    if (refused) {
        offer_refused = refused;
        offer_len = 0;
    } else {
        uint32_t n = len - (EGRESS_KEY_HEX + 1);
        if (n > EGRESS_SPEC_MAX) {
            n = EGRESS_SPEC_MAX; /* console.c holds at most this much; belt and braces */
        }
        for (uint32_t i = 0; i < n; i++) {
            offer_spec[i] = payload[EGRESS_KEY_HEX + 1 + i];
        }
        offer_spec[n] = '\0';
        offer_len = n;
        offer_refused = 0;
    }
    offer_pending = 1;
}

int egress_apply_pending(void) {
    if (!offer_pending) {
        return 0;
    }
    /* Take the mailbox with interrupts off: a second escape mid-copy must not interleave. */
    char spec[EGRESS_SPEC_MAX + 1];
    uint64_t flags = irq_save();
    int refused = offer_refused;
    for (uint32_t i = 0; i <= offer_len; i++) {
        spec[i] = offer_spec[i];
    }
    offer_pending = 0;
    irq_restore(flags);
    if (refused) {
        receipt_grant(GRANT_REFUSED, AUDIT_V_EPERM);
        boot_log(refused == OFFER_NOKEY ? "NET: egress escape refused: no egress_key on the cmdline"
                 : refused == OFFER_BADKEY ? "NET: egress escape refused: key mismatch"
                                           : "NET: egress escape refused: payload is <key>;<spec>");
        return 0;
    }
    if (configure_from(spec, 1) != 0) {
        receipt_grant(GRANT_REFUSED, AUDIT_V_EPERM); /* parse_grant logged which field */
        return 0;
    }
    return 1;
}
