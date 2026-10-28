/**
 * QuantumOS qsh — the interactive shell (epic #62 phase 1, #63).
 *
 * A ring-3 user-process service, watchdog-monitored like every other
 * citizen. Its entire authority is what it was granted: the console
 * device capability (keystrokes in, raw bytes out), a quantum-pool read
 * capability (the qrand/qseed builtins), and one IPC send-capability to
 * ghostd (the ghost builtin). It can inspect the live system through the
 * uncapped read-only SYS_SYSINFO, and nothing it does can reach past
 * those grants.
 *
 * Line discipline lives HERE, not in the kernel: the kernel hands over
 * raw bytes; qsh echoes, handles backspace, and assembles lines. `exit`
 * is a supervised death — the watchdog notices the silent heartbeat and
 * restarts the shell, which then introduces itself as reborn. That
 * banner is the merge-gate proof that the operator surface survives its
 * own crash.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h"       /* usys.h + wire structs + freestanding string builders */
#include "qpu_circuit.h" /* the circuit wire format the host broker takes (#262) */

#define LINE_MAX 120

/* ------------------------------------------------------------------ *
 * Output helpers — every logical line is written with ONE SYS_CONS
 * call (< 256 bytes), so a timer-tick boot_log can never split it.
 * ------------------------------------------------------------------ */

static void out_bytes(const char *b, long n) {
    long off = 0;
    while (off < n) {
        long w = cons_write(b + off, n - off);
        if (w <= 0) {
            break;
        }
        off += w;
    }
}

static void out(const char *s) {
    out_bytes(s, str_len(s));
}

/* ANSI colour. The screen console (kernel/src/vga.c) interprets these SGR
 * codes into VGA attributes and a real terminal renders them natively, so the
 * shell looks the same in the browser demo and on hardware. */
#define A0 "\x1b[0m"         /* reset */
#define A_TITLE "\x1b[1;36m" /* bold cyan */
#define A_CAT "\x1b[1;35m"   /* bold magenta — category labels */
#define A_CMD "\x1b[1;32m"   /* bold green — command names */
#define A_KEY "\x1b[1;33m"   /* bold yellow — highlights */
#define A_DIM "\x1b[90m"     /* grey — descriptions */

static void prompt(void) {
    out(A_TITLE "qsh" A_DIM "> " A0);
}

static int put_hex64(char *b, int o, unsigned long long v) {
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        b[o++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return o;
}

/* ------------------------------------------------------------------ *
 * Command matching (no libc)
 * ------------------------------------------------------------------ */

static int is_cmd(const char *line, const char *cmd) {
    int i = 0;
    while (cmd[i] && line[i] == cmd[i]) {
        i++;
    }
    return cmd[i] == '\0' && line[i] == '\0';
}

/* Returns the argument after "<cmd> ", or 0 if line doesn't start so. */
static const char *arg_of(const char *line, const char *cmd) {
    int i = 0;
    while (cmd[i] && line[i] == cmd[i]) {
        i++;
    }
    if (cmd[i] == '\0' && line[i] == ' ') {
        while (line[i] == ' ') {
            i++;
        }
        return line + i;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Builtins
 * ------------------------------------------------------------------ */

static void cmd_help(void) {
    /* "qsh commands:" is the CI grep anchor — keep it intact (colour codes
     * sit outside the phrase). Grouped by what each command is FOR, with a
     * one-line description, so a newcomer knows where to start. */
    out(A_TITLE "qsh commands:" A0 "\r\n");
    out(A_CAT "  memory " A0 A_CMD "imprint" A0 " <text>   " A_DIM "store a phrase (opt. " A_KEY
              "--energy 0-100" A_DIM ")" A0 "\r\n");
    out("         " A_CMD "recall" A0 " <cue>     " A_DIM "bring it back from a corrupted cue" A0
        "\r\n");
    out("         " A_CMD "field" A0 "          " A_DIM
        "inspect the field: live slots + energy (read-only)" A0 "\r\n");
    out("         " A_CMD "ghost" A0 "          " A_DIM "field status: order R and live patterns" A0
        "\r\n");
    out("         " A_CMD "fieldtest" A0 "      " A_DIM "run the field self-test" A0 "\r\n");
    out(A_CAT "  quantum" A0 " " A_CMD "qrand" A0 "          " A_DIM
              "64 bits from the seeded generator" A0 "\r\n");
    out("         " A_CMD "qseed" A0 "          " A_DIM "the seed this system started with" A0
        "\r\n");
    out("         " A_CMD "ghost" A0 "          " A_DIM
        "dice + resonant recall (seed: see qseed)" A0 "\r\n");
    out("         " A_CMD "qpu" A0 "            " A_DIM
        "send a circuit to the host's qBraid broker, if one is attached" A0 "\r\n");
    out(A_CAT "  run    " A0 A_CMD "run" A0 " <path>      " A_DIM "start a citizen  " A_KEY
              "try: run /bin/qtop" A0 "\r\n");
    out(A_CAT "  files  " A0 A_CMD "ls" A0 " " A_CMD "cat" A0 " " A_CMD "write" A0 " " A_CMD "rm" A0
              " " A_CMD "sync" A0 "  " A_DIM "browse and edit the initrd" A0 "\r\n");
    out(A_CAT "  net    " A0 A_CMD "http" A0 " <host>   " A_CMD "nslookup" A0 "   " A_CMD
              "udping" A0 "   " A_CMD "egress" A0 "   " A_DIM
              "reach the network, within the grant" A0 "\r\n");
    out(A_CAT "  system " A0 A_CMD "ps" A0 " " A_CMD "free" A0 " " A_CMD "uptime" A0 " " A_CMD
              "date" A0 " " A_CMD "pid" A0 " " A_CMD "echo" A0 "  " A_DIM "inspect the machine" A0
              "\r\n");
    out("         " A_CMD "audit" A0 "          " A_DIM
        "capability ledger: who was granted/denied what (kernel-recorded)" A0 "\r\n");
    out(A_CAT "  shell  " A0 A_CMD "clear" A0 " " A_CMD "help" A0 " " A_CMD "exit" A0 "\r\n");
}

/* write <path> <text>: create/truncate an overlay file with the text
 * (the shell's filesystem-write capability at work). */
static void cmd_write(const char *args) {
    /* Split "<path> <text...>" */
    char path[64];
    int p = 0;
    while (args[p] && args[p] != ' ' && p < (int)sizeof(path) - 1) {
        path[p] = args[p];
        p++;
    }
    path[p] = '\0';
    const char *text = args + p;
    while (*text == ' ') {
        text++;
    }
    if (!path[0] || !*text) {
        out("qsh: write: usage: write <path> <text>\r\n");
        return;
    }

    long fd = openf_(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: write: cannot create (err ");
        o = ghost_put_u(b, o, (unsigned)(-fd));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }

    long total = 0;
    long len = str_len(text);
    while (total < len) {
        long w = fwrite_(fd, text + total, len - total);
        if (w <= 0) {
            break;
        }
        total += w;
    }
    fwrite_(fd, "\n", 1);
    close_(fd);

    char b[96];
    int o = ghost_put(b, 0, "qsh: wrote ");
    o = ghost_put_u(b, o, (unsigned)total);
    o = ghost_put(b, o, " bytes to ");
    for (int i = 0; path[i] && o < (int)sizeof(b) - 4; i++) {
        b[o++] = path[i];
    }
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_rm(const char *path) {
    long r = unlink_(path);
    if (r == 0) {
        out("qsh: removed\r\n");
    } else {
        char b[96];
        int o = ghost_put(b, 0, "qsh: rm: failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    }
}

/* ------------------------------------------------------------------ *
 * Kernel holographic field (epic #95): the shell holds the
 * CAP_RESOURCE_FIELD capability over region 0, so the operator gets
 * associative memory at the prompt — imprint text, recall it later
 * from a corrupted probe. The "FIELD:" output prefixes are merge
 * gates; their line forms cannot be produced by echoed input.
 * ------------------------------------------------------------------ */
#define QSH_FIELD_REGION 0u

static void cmd_imprint(const char *args) {
    /* Optional leading "--energy <int>" sets the slot's importance (percent
     * 0..100 -> Q15). It is consumed ONLY when the token is all-digits, so
     * `imprint --energy hello` stays literal text and a doubled --energy from a
     * caller leaves the second occurrence as content. */
    int energy_q15 = 0; /* 0 => kernel default (0.5) when the flag is absent */
    const char flag[] = "--energy ";
    int match = 1;
    for (int i = 0; flag[i]; i++) {
        if (args[i] != flag[i]) {
            match = 0;
            break;
        }
    }
    if (match) {
        const char *p = args + (int)(sizeof(flag) - 1);
        long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (v < 1000) {
                v = v * 10 + (*p - '0'); /* saturate: never overflow */
            }
            digits++;
            p++;
        }
        if (digits > 0 && (*p == ' ' || *p == '\0')) {
            while (*p == ' ') {
                p++;
            }
            if (v > 100) {
                v = 100;
            }
            /* percent -> Q15. N=0 maps to the FLOOR, not 0: energy_q15==0 is the
             * kernel's "use the 0.5 default" sentinel, the opposite of minimum. */
            int e = (int)(((long)v * FIELD_ENERGY_MAX) / 100);
            if (e < FIELD_ENERGY_MIN) {
                e = FIELD_ENERGY_MIN;
            }
            if (e > FIELD_ENERGY_MAX) {
                e = FIELD_ENERGY_MAX;
            }
            energy_q15 = e;
            args = p; /* the text follows the consumed flag */
        }
    }

    long n = str_len(args);
    if (n <= 0) {
        out("qsh: imprint: usage: imprint [--energy <0-100>] <text>\r\n");
        return;
    }
    if (n > FIELD_PAT_MAX) {
        n = FIELD_PAT_MAX;
    }
    field_imprint_req_t req;
    req.region = QSH_FIELD_REGION;
    req.len = (unsigned)n;
    req.energy_q15 = energy_q15;
    for (long i = 0; i < FIELD_PAT_MAX; i++) {
        req.pattern[i] = i < n ? (unsigned char)args[i] : 0;
    }
    long r = imprint_(&req);
    if (r < 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: imprint failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }
    char b[96];
    int o = ghost_put(b, 0, "FIELD: imprinted slot ");
    o = ghost_put_u(b, o, (unsigned)r);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_recall(const char *args) {
    long n = str_len(args);
    if (n <= 0) {
        out("qsh: recall: usage: recall <probe text>\r\n");
        return;
    }
    if (n > FIELD_PAT_MAX) {
        n = FIELD_PAT_MAX;
    }
    field_recall_req_t req;
    req.region = QSH_FIELD_REGION;
    req.len = (unsigned)n;
    req.k = 3;
    for (long i = 0; i < FIELD_PAT_MAX; i++) {
        req.probe[i] = i < n ? (unsigned char)args[i] : 0;
    }
    field_recall_out_t res;
    long r = recall_(&req, &res);
    if (r < 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: recall failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }
    if (res.n == 0) {
        out("FIELD: recall empty (n=0)\r\n");
        return;
    }
    char b[192];
    int o = ghost_put(b, 0, "FIELD: winner=\"");
    for (unsigned i = 0; i < res.winner_len && o < (int)sizeof(b) - 40; i++) {
        b[o++] = (char)res.winner[i];
    }
    o = ghost_put(b, o, "\" slot=");
    o = ghost_put_u(b, o, res.rank[0].slot);
    o = ghost_put(b, o, " score=");
    int sc = res.rank[0].score_q15;
    if (sc < 0) {
        b[o++] = '-';
        sc = -sc;
    }
    o = ghost_put_u(b, o, (unsigned)sc);
    o = ghost_put(b, o, " n=");
    o = ghost_put_u(b, o, res.n);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

/* fieldtest: assert the field's two negative contracts from the one
 * process that HOLDS a field cap (ghost_test proves the capless side):
 * a cross-region request must be exactly EPERM even for a cap holder,
 * and a degenerate probe must be a clean n=0, never a kernel fault. */
/* Read-only field introspection (epic #127 B1): SYS_FIELD_INFO reports live
 * count, capacity, and per-slot metadata WITHOUT reinforcing anything — the
 * honest counterpart to recall. The FIELDINFO:/FIELDSLOT: prefixes are merge
 * gates; preview bytes are sanitised to printable-non-quote so stored content
 * can never forge or break a FIELDSLOT line. */
static void cmd_field(void) {
    field_info_out_t info;
    long r = field_info_(QSH_FIELD_REGION, &info);
    if (r < 0) {
        char b[64];
        int o = ghost_put(b, 0, "qsh: field denied (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }
    char b[160];
    int o = ghost_put(b, 0, "FIELDINFO: region=");
    o = ghost_put_u(b, o, info.region);
    o = ghost_put(b, o, " live=");
    o = ghost_put_u(b, o, info.live);
    o = ghost_put(b, o, " cap=");
    o = ghost_put_u(b, o, info.capacity);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
    for (unsigned i = 0; i < info.live && i < FIELD_SLOTS; i++) {
        const field_slot_info_t *s = &info.slots[i];
        o = ghost_put(b, 0, "FIELDSLOT: slot=");
        o = ghost_put_u(b, o, s->slot);
        o = ghost_put(b, o, " len=");
        o = ghost_put_u(b, o, s->len);
        o = ghost_put(b, o, " energy=");
        o = ghost_put_u(b, o, (unsigned)(((long)s->energy_q15 * 100) / FIELD_ENERGY_MAX));
        o = ghost_put(b, o, " eff=");
        o = ghost_put_u(b, o, (unsigned)(((long)s->eff_energy_q15 * 100) / FIELD_ENERGY_MAX));
        o = ghost_put(b, o, " retr=");
        o = ghost_put_u(b, o, s->retrievals);
        o = ghost_put(b, o, " age=");
        o = ghost_put_u(b, o, s->age_ticks);
        o = ghost_put(b, o, " preview=\"");
        for (unsigned k = 0; k < s->len && k < FIELD_PREVIEW; k++) {
            char c = (char)s->preview[k];
            b[o++] = (c >= 32 && c < 127 && c != '"') ? c : '.';
        }
        o = ghost_put(b, o, "\"\r\n");
        out_bytes(b, o);
    }
}

/* Capability authority ledger (epic #133 Phase D): the KERNEL records every
 * capability GRANT, DENY, and SPAWN — a citizen cannot forge or suppress its own
 * entry. `audit` reads it read-only (SYS_AUDIT), first the stats line then every
 * live entry. The AUDIT: prefix is a merge gate. (Read over COM1: a cryptographic
 * export over the attested COM2 channel is a filed follow-up.) */
/* The egress receipt (ADR-0030): the ledger's EGRESS rows only. res=DEV:<destination IPv4
 * as a decimal u32, 0 for a listen check>, perms=(proto<<16)|port, verdict OK or EPERM,
 * count = how many times. pid 0 rows are the grant itself (perms 1 granted, 2 expired,
 * 3 budget spent). The kernel wrote every row; this only filters. */
static void cmd_egress(void) {
    static char buf[AUDIT_TEXT_MAX];
    long n = audit_(AUDIT_OP_READ, buf, sizeof(buf));
    int shown = 0;
    long start = 0;
    for (long i = 0; i <= n; i++) {
        if (i == n || buf[i] == '\n') {
            long len = i - start;
            int hit = 0;
            for (long k = start; k + 11 <= i; k++) {
                if (buf[k] == 'k' && buf[k + 1] == 'i' && buf[k + 2] == 'n' && buf[k + 3] == 'd' &&
                    buf[k + 4] == '=' && buf[k + 5] == 'E' && buf[k + 6] == 'G' &&
                    buf[k + 7] == 'R' && buf[k + 8] == 'E' && buf[k + 9] == 'S' &&
                    buf[k + 10] == 'S') {
                    hit = 1;
                    break;
                }
            }
            if (hit && len > 0) {
                out_bytes(buf + start, (int)len);
                out("\r\n");
                shown++;
            }
            start = i + 1;
        }
    }
    if (!shown) {
        out("qsh: egress: no egress events in the ledger (no grant was given, or nothing "
            "asked)\r\n");
    }
}

static void cmd_audit(void) {
    static char buf[AUDIT_TEXT_MAX];
    long n = audit_(AUDIT_OP_STATS, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
    n = audit_(AUDIT_OP_READ, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

/* Intent manifests (epic #135 Phase D inc. 2): the KERNEL dumps every bound
 * citizen's DECLARED intent — its allow-set, spawn quota (used/max), and CPU
 * ticks — a policy layer above raw capabilities that a citizen cannot forge.
 * `manifest` reads it read-only (SYS_MANIFEST). The MANIFEST: prefix is a
 * merge gate; the static buffer is mandatory (the 16 KB user stack cannot
 * hold a 16 KB dump — a stack buffer page-faults and kills the shell). */
static void cmd_manifest(void) {
    static char buf[MANIFEST_TEXT_MAX];
    long n = manifest_(buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

static void cmd_fieldtest(void) {
    field_imprint_req_t req;
    req.region = QSH_FIELD_REGION + 1; /* a region the shell holds NO cap for */
    req.len = 5;
    req.energy_q15 = 0;
    for (int i = 0; i < FIELD_PAT_MAX; i++) {
        req.pattern[i] = 0;
    }
    req.pattern[0] = 'c';
    req.pattern[1] = 'r';
    req.pattern[2] = 'o';
    req.pattern[3] = 's';
    req.pattern[4] = 's';
    long r = imprint_(&req);
    if (r == -4) {
        out("FIELD: cross-region denied (EPERM)\r\n");
    } else {
        char b[96];
        int o = ghost_put(b, 0, "FIELD: cross-region NOT denied (ret ");
        o = ghost_put_u(b, o, (unsigned)(r < 0 ? -r : r));
        o = ghost_put(b, o, ") — isolation broken\r\n");
        out_bytes(b, o);
    }

    field_recall_req_t rr;
    rr.region = QSH_FIELD_REGION;
    rr.len = 4;
    rr.k = 1;
    for (int i = 0; i < FIELD_PAT_MAX; i++) {
        rr.probe[i] = 0;
    }
    rr.probe[0] = ' ';
    rr.probe[1] = ' ';
    rr.probe[2] = ' ';
    rr.probe[3] = ' '; /* all-equal bytes: degenerate on purpose */
    field_recall_out_t res;
    r = recall_(&rr, &res);
    if (r == 0 && res.n == 0) {
        out("FIELD: empty-probe ok (n=0)\r\n");
    } else {
        out("FIELD: empty-probe MISBEHAVED\r\n");
    }
}

/* net2 <peer-ip>: prove guest-to-guest UDP (epic #97 wire). Bind port
 * 9999, send a probe to peer:9999, and poll our own :9999 for the peer's
 * probe. Symmetric — both guests run it, each sends and receives, so
 * there is no listener-ordering race. A received datagram from the
 * peer's IP (which differs from ours) can only mean the ARP responder
 * answered and the on-link route worked: it cannot be loopback. */
#define NET2_PORT 9999
static void cmd_net2(const char *peer) {
    unsigned char pip[4];
    /* Parse a dotted-quad (no DNS on a raw peer L2). */
    int oi = 0, val = -1;
    for (const char *h = peer;; h++) {
        char c = *h;
        if (c >= '0' && c <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (c - '0');
        } else if (c == '.' || c == '\0' || c == ' ') {
            if (val < 0 || val > 255 || oi >= 4) {
                oi = -1;
                break;
            }
            pip[oi++] = (unsigned char)val;
            val = -1;
            if (c != '.') {
                break;
            }
        } else {
            oi = -1;
            break;
        }
    }
    if (oi != 4) {
        out("qsh: net2: usage: net2 <peer-ip>\r\n");
        return;
    }

    udp_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    req.port = NET2_PORT;
    long sock = udp_(UDP_BIND, &req);
    if (sock < 0) {
        out(sock == -4            ? "qsh: net2: denied (no network capability)\r\n"
            : sock == SYS_EEGRESS ? "qsh: net2: refused by the egress grant\r\n"
                                  : "qsh: net2: bind failed (no network?)\r\n");
        return;
    }

    static unsigned char probe[8] = {'Q', 'O', 'S', 'P', 'R', 'O', 'B', 'E'};
    long got = -1;
    /* Send-then-poll, retransmitting each round: the peer may not be up
     * yet, and UDP is lossy — keep offering until we hear back or time
     * out (~25 s at 100 Hz). The window must span the whole two-guest run
     * so it covers boot skew: whichever guest boots first keeps listening
     * until the slower one starts sending. */
    for (long t0 = ticks(); ticks() - t0 < 2500 && got < 0;) {
        udp_req_t s;
        for (unsigned i = 0; i < sizeof(s); i++) {
            ((unsigned char *)&s)[i] = 0;
        }
        s.sock = sock;
        for (int i = 0; i < 4; i++) {
            s.ip[i] = pip[i];
        }
        s.port = NET2_PORT;
        s.buf = probe;
        s.len = sizeof(probe);
        udp_(UDP_SENDTO, &s); /* WOULDBLOCK ok — we retry */

        for (int spin = 0; spin < 40 && got < 0; spin++) {
            heartbeat();
            udp_req_t r;
            for (unsigned i = 0; i < sizeof(r); i++) {
                ((unsigned char *)&r)[i] = 0;
            }
            static unsigned char rbuf[64];
            r.sock = sock;
            r.buf = rbuf;
            r.len = sizeof(rbuf);
            long n = udp_(UDP_RECVFROM, &r);
            if (n > 0) {
                got = n;
                char b[80];
                int o = ghost_put(b, 0, "NET2: probe from ");
                for (int i = 0; i < 4; i++) {
                    o = ghost_put_u(b, o, r.ip[i]);
                    if (i < 3) {
                        b[o++] = '.';
                    }
                }
                o = ghost_put(b, o, "\r\n");
                out_bytes(b, o);
            } else {
                yield();
            }
        }
    }
    req.sock = sock;
    udp_(UDP_CLOSE, &req);
    if (got < 0) {
        out("NET2: no probe received (peer down?)\r\n");
    }
}

/* nslookup <host>: resolve a hostname to an IPv4 address via SYS_RESOLVE
 * (the shell's network capability at work). The kernel does the lookup
 * in its net thread; we poll, yielding+heartbeating so a slow lookup
 * doesn't get the shell watchdog-killed. */
static void cmd_nslookup(const char *host) {
    unsigned char ip[4];
    long r = RESOLVE_WOULDBLOCK;
    for (int tries = 0; tries < 2000 && r == RESOLVE_WOULDBLOCK; tries++) {
        heartbeat();
        r = resolve_(host, ip);
        if (r == RESOLVE_WOULDBLOCK) {
            yield();
        }
    }
    if (r == 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: ");
        for (int i = 0; host[i] && o < (int)sizeof(b) - 32; i++) {
            b[o++] = host[i];
        }
        o = ghost_put(b, o, " -> ");
        for (int i = 0; i < 4; i++) {
            o = ghost_put_u(b, o, ip[i]);
            if (i < 3) {
                b[o++] = '.';
            }
        }
        o = ghost_put(b, o, "\r\n");
        out_bytes(b, o);
    } else if (r == -4) {
        out("qsh: nslookup: denied (no network capability)\r\n");
    } else if (r == SYS_EEGRESS) {
        out("qsh: nslookup: refused by the egress grant\r\n");
    } else if (r == RESOLVE_WOULDBLOCK) {
        out("qsh: nslookup: timed out\r\n");
    } else {
        out("qsh: nslookup: no network / lookup failed\r\n");
    }
}

/* ------------------------------------------------------------------ *
 * udping <host>: a USERSPACE DNS client over the SYS_UDP socket API
 * (epic #80). Where nslookup asks the kernel to resolve, udping builds
 * the DNS A-query itself in ring 3, sends it as a raw datagram to
 * SLIRP's proxy (10.0.2.3:53) with UDP_SENDTO, receives the raw reply
 * with UDP_RECVFROM, and parses the answer itself — proving user
 * programs can speak arbitrary UDP through the capability-gated
 * socket API.
 * ------------------------------------------------------------------ */

/* Build a DNS A query for `name`. Returns the query length, 0 if the
 * name doesn't fit. */
static int dns_query_build(unsigned char *q, int max, const char *name, unsigned txid) {
    if (max < 18) {
        return 0;
    }
    q[0] = (unsigned char)(txid >> 8);
    q[1] = (unsigned char)txid;
    q[2] = 0x01; /* recursion desired */
    q[3] = 0x00;
    q[4] = 0x00;
    q[5] = 0x01; /* qdcount = 1 */
    for (int i = 6; i < 12; i++) {
        q[i] = 0;
    }
    int p = 12;
    int label = p++;
    int llen = 0;
    for (int i = 0; name[i]; i++) {
        if (p >= max - 5) {
            return 0;
        }
        if (name[i] == '.') {
            q[label] = (unsigned char)llen;
            label = p++;
            llen = 0;
        } else {
            if (llen >= 63) {
                return 0; /* RFC 1035: labels cap at 63 bytes — 64+ would
                           * collide with the compression-pointer tag */
            }
            q[p++] = (unsigned char)name[i];
            llen++;
        }
    }
    q[label] = (unsigned char)llen;
    q[p++] = 0x00; /* root label */
    q[p++] = 0x00; /* QTYPE A = 1 */
    q[p++] = 0x01;
    q[p++] = 0x00; /* QCLASS IN = 1 */
    q[p++] = 0x01;
    return p;
}

/* Advance past a (possibly compressed) DNS name. */
static int dns_skip_name(const unsigned char *r, int len, int off) {
    while (off < len) {
        unsigned char c = r[off];
        if (c == 0) {
            return off + 1;
        }
        if ((c & 0xC0) == 0xC0) {
            return off + 2; /* a compression pointer ends the name */
        }
        off += c + 1;
    }
    return len;
}

/* Find the first A record in a DNS response. 1 + ip filled, or 0. */
static int dns_reply_parse(const unsigned char *r, int len, unsigned char *ip) {
    if (len < 12) {
        return 0;
    }
    int qd = (r[4] << 8) | r[5];
    int an = (r[6] << 8) | r[7];
    int off = 12;
    for (int q = 0; q < qd; q++) {
        off = dns_skip_name(r, len, off) + 4; /* + QTYPE/QCLASS */
    }
    for (int a = 0; a < an; a++) {
        off = dns_skip_name(r, len, off);
        if (off + 10 > len) {
            return 0;
        }
        int type = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            for (int i = 0; i < 4; i++) {
                ip[i] = r[off + i];
            }
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

/* Send the query to 10.0.2.3:53, retrying WOULDBLOCK (the net thread
 * may be busy with a kernel resolve). Bounded, heartbeating. */
static long udping_send(long sock, unsigned char *query, int qlen) {
    udp_req_t req;
    req.sock = sock;
    req.ip[0] = 10;
    req.ip[1] = 0;
    req.ip[2] = 2;
    req.ip[3] = 3;
    req.port = 53;
    req.len = (unsigned short)qlen;
    req.buf = query;
    long r = UDP_WOULDBLOCK;
    for (int tries = 0; tries < 2000 && r == UDP_WOULDBLOCK; tries++) {
        heartbeat();
        r = udp_(UDP_SENDTO, &req);
        if (r == UDP_WOULDBLOCK) {
            yield();
        }
    }
    return r;
}

static void cmd_udping(const char *host) {
    udp_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    long sock = udp_(UDP_BIND, &req); /* port 0 = ephemeral */
    if (sock == -4) {
        out("qsh: udping: denied (no network capability)\r\n");
        return;
    }
    if (sock < 0) {
        out("qsh: udping: no socket (no network?)\r\n");
        return;
    }

    unsigned char query[288];
    unsigned txid = (unsigned)ticks() & 0xFFFFu;
    int qlen = dns_query_build(query, (int)sizeof(query), host, txid);
    if (qlen == 0) {
        req.sock = sock;
        udp_(UDP_CLOSE, &req);
        out("qsh: udping: hostname too long\r\n");
        return;
    }

    long sr = udping_send(sock, query, qlen);
    if (sr < 0) {
        req.sock = sock;
        udp_(UDP_CLOSE, &req);
        out(sr == UDP_WOULDBLOCK ? "qsh: udping: send queue busy\r\n"
            : sr == SYS_EEGRESS  ? "qsh: udping: refused by the egress grant\r\n"
                                 : "qsh: udping: send failed\r\n");
        return;
    }

    /* Poll for the reply, TIME-bounded (~8s at 100 Hz: resolver latency
     * is wall-clock, not an iteration count). Validate the datagram is
     * really our answer — right sender, right txid; strays are skipped.
     * One retransmit halfway in case the first query was lost. */
    unsigned char resp[512];
    long t0 = ticks();
    long got = -1;
    int resent = 0;
    while (ticks() - t0 < 800) {
        heartbeat();
        req.sock = sock;
        req.len = (unsigned short)sizeof(resp);
        req.buf = resp;
        long rr = udp_(UDP_RECVFROM, &req);
        if (rr == UDP_WOULDBLOCK) {
            if (!resent && ticks() - t0 > 400) {
                resent = 1;
                udping_send(sock, query, qlen);
            }
            yield();
            continue;
        }
        if (rr < 0) {
            break;
        }
        if (req.ip[0] == 10 && req.ip[1] == 0 && req.ip[2] == 2 && req.ip[3] == 3 &&
            req.port == 53 && rr >= 12 && resp[0] == (unsigned char)(txid >> 8) &&
            resp[1] == (unsigned char)txid) {
            got = rr;
            break;
        }
    }

    /* Single close point BEFORE reporting: qsh never exits, so a slot
     * leaked on any path here would be leaked for good (4 exist). */
    req.sock = sock;
    udp_(UDP_CLOSE, &req);

    if (got < 0) {
        out("qsh: udping: timed out\r\n");
        return;
    }

    char b[96];
    int o = ghost_put(b, 0, "qsh: udp ");
    o = ghost_put_u(b, o, (unsigned)got);
    o = ghost_put(b, o, " bytes from 10.0.2.3:53\r\n");
    out_bytes(b, o);

    unsigned char ip[4];
    if (dns_reply_parse(resp, (int)got, ip)) {
        o = ghost_put(b, 0, "qsh: udpdns ");
        for (int i = 0; host[i] && o < (int)sizeof(b) - 40; i++) {
            b[o++] = host[i];
        }
        o = ghost_put(b, o, " -> ");
        for (int i = 0; i < 4; i++) {
            o = ghost_put_u(b, o, ip[i]);
            if (i < 3) {
                b[o++] = '.';
            }
        }
        o = ghost_put(b, o, "\r\n");
        out_bytes(b, o);
    } else {
        o = ghost_put(b, 0, "qsh: udpdns ");
        for (int i = 0; host[i] && o < (int)sizeof(b) - 24; i++) {
            b[o++] = host[i];
        }
        o = ghost_put(b, o, ": no A record\r\n");
        out_bytes(b, o);
    }
}

/* ------------------------------------------------------------------ *
 * http <host> [port]: fetch a web page over the ring-3 TCP client
 * (epic #82). Resolves the name (SYS_RESOLVE — two syscalls compose),
 * opens a connection, sends one HTTP/1.0 GET, reads the response to EOF,
 * and prints the status line + byte count. All polling is time-bounded
 * (heartbeat+yield so a slow fetch can't get the shell watchdog-killed),
 * and the socket is closed on every exit path (qsh never exits — a
 * leaked connection would wedge the single TCB).
 * ------------------------------------------------------------------ */

/* Parse "a.b.c.d" into ip[4]; returns 1 on success. Stops at NUL/space. */
static int parse_ipv4(const char *s, unsigned char *ip) {
    int vals[4];
    int vi = 0, cur = 0, digits = 0;
    for (int i = 0;; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            digits++;
            if (cur > 255) {
                return 0;
            }
        } else if (c == '.' || c == '\0' || c == ' ') {
            if (digits == 0 || vi >= 4) {
                return 0;
            }
            vals[vi++] = cur;
            cur = 0;
            digits = 0;
            if (c == '\0' || c == ' ') {
                break;
            }
        } else {
            return 0;
        }
    }
    if (vi != 4) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        ip[i] = (unsigned char)vals[i];
    }
    return 1;
}

/* Poll-close the connection (bounded); ignores the outcome. */
static void http_close(tcp_req_t *req) {
    long t0 = ticks();
    for (int guard = 0; guard < 20000 && ticks() - t0 < 300; guard++) {
        heartbeat();
        long r = tcp_(TCP_CLOSE, req);
        if (r != UDP_WOULDBLOCK) {
            break;
        }
        yield();
    }
}

static void cmd_http(const char *args) {
    char host[64];
    int hlen = 0;
    while (args[hlen] && args[hlen] != ' ' && hlen < (int)sizeof(host) - 1) {
        host[hlen] = args[hlen];
        hlen++;
    }
    host[hlen] = '\0';

    const char *pp = args + hlen;
    while (*pp == ' ') {
        pp++;
    }
    unsigned port = 0;
    while (*pp >= '0' && *pp <= '9') {
        port = port * 10 + (unsigned)(*pp - '0');
        pp++;
    }
    if (port == 0) {
        port = 80;
    }

    /* Resolve the host (literal dotted-quad, or DNS via SYS_RESOLVE). */
    unsigned char ip[4];
    if (!parse_ipv4(host, ip)) {
        long r = RESOLVE_WOULDBLOCK;
        for (int tries = 0; tries < 2000 && r == RESOLVE_WOULDBLOCK; tries++) {
            heartbeat();
            r = resolve_(host, ip);
            if (r == RESOLVE_WOULDBLOCK) {
                yield();
            }
        }
        if (r == -4) {
            out("qsh: http: denied (no network capability)\r\n");
            return;
        }
        if (r != 0) {
            out(r == SYS_EEGRESS ? "qsh: http: refused by the egress grant\r\n"
                                 : "qsh: http: could not resolve host\r\n");
            return;
        }
    }

    tcp_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }

    /* Connect (time-bounded poll). */
    long cr = UDP_WOULDBLOCK;
    long t0 = ticks();
    for (int guard = 0; guard < 200000 && ticks() - t0 < 600; guard++) {
        heartbeat();
        req.sock = 0;
        for (int i = 0; i < 4; i++) {
            req.ip[i] = ip[i];
        }
        req.port = (unsigned short)port;
        cr = tcp_(TCP_CONNECT, &req);
        if (cr == 0) {
            break;
        }
        if (cr != UDP_WOULDBLOCK) {
            break;
        }
        yield();
    }
    if (cr != 0) {
        http_close(&req);
        if (cr == -4) {
            out("qsh: http: denied (no network capability)\r\n");
        } else if (cr == SYS_EEGRESS) {
            out("qsh: http: refused by the egress grant\r\n");
        } else {
            out("qsh: http: connect failed\r\n");
        }
        return;
    }

    /* One HTTP/1.0 GET (<= MSS, sends in a single segment). */
    char get[256];
    int g = ghost_put(get, 0, "GET / HTTP/1.0\r\nHost: ");
    for (int i = 0; host[i] && g < (int)sizeof(get) - 32; i++) {
        get[g++] = host[i];
    }
    g = ghost_put(get, g, "\r\nConnection: close\r\n\r\n");

    long sres = UDP_WOULDBLOCK;
    t0 = ticks();
    for (int guard = 0; guard < 200000 && ticks() - t0 < 500; guard++) {
        heartbeat();
        req.buf = get;
        req.len = (unsigned short)g;
        sres = tcp_(TCP_SEND, &req);
        if (sres == g) {
            break;
        }
        if (sres != UDP_WOULDBLOCK) {
            break;
        }
        yield();
    }
    if (sres != g) {
        http_close(&req);
        out("qsh: http: send failed\r\n");
        return;
    }

    /* Read the response to EOF, capturing the first line and counting
     * bytes. The idle timer resets on progress so a large transfer isn't
     * cut short; only a genuine stall trips the bound. */
    char chunk[2048]; /* 2 KiB per RECV (#329): 8x fewer syscalls per window than 256 */
    char status[80];
    int slen = 0, got_status = 0;
    long total = 0;
    int reset = 0;
    long tr = ticks(); /* t0 (set before the request went out) is the transfer clock, #329 */
    for (int guard = 0; guard < 2000000; guard++) {
        heartbeat();
        if (ticks() - tr > 800) {
            break;
        }
        req.buf = chunk;
        req.len = (unsigned short)sizeof(chunk);
        long rr = tcp_(TCP_RECV, &req);
        if (rr == 0) {
            break; /* clean EOF (peer FIN, ring drained) */
        }
        if (rr == UDP_WOULDBLOCK) {
            yield();
            continue;
        }
        if (rr < 0) {
            reset = 1; /* reset/error mid-stream — the fetch is truncated */
            break;
        }
        if (!got_status) {
            for (long i = 0; i < rr; i++) {
                char c = chunk[i];
                if (c == '\r' || c == '\n') {
                    got_status = 1;
                    break;
                }
                if (slen < (int)sizeof(status) - 1) {
                    status[slen++] = c;
                }
            }
        }
        total += rr;
        tr = ticks();
    }
    status[slen] = '\0';

    http_close(&req);

    if (!got_status) {
        out("qsh: http: no response (timed out)\r\n");
        return;
    }

    /* Status line — host label WITHOUT the port. */
    char b[128];
    int o = ghost_put(b, 0, "qsh: http ");
    for (int i = 0; host[i] && o < (int)sizeof(b) - 48; i++) {
        b[o++] = host[i];
    }
    o = ghost_put(b, o, " -> ");
    for (int i = 0; i < slen && o < (int)sizeof(b) - 4; i++) {
        b[o++] = status[i];
    }
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);

    o = ghost_put(b, 0, "qsh: http ");
    for (int i = 0; host[i] && o < (int)sizeof(b) - 40; i++) {
        b[o++] = host[i];
    }
    o = ghost_put(b, o, ": ");
    o = ghost_put_u(b, o, (unsigned)total);
    /* Be honest about a truncated transfer: a mid-stream reset drains what
     * was buffered and then returns an error, which must NOT read as a
     * clean fetch. */
    o = ghost_put(b, o, reset ? " bytes then connection reset" : " bytes received");
    /* Transfer time in ms (ticks at 100 Hz), so a gate can hold the stack to
     * a throughput floor that is paced by the guest's own clock (#329). */
    o = ghost_put(b, o, " in ");
    o = ghost_put_u(b, o, (unsigned)((ticks() - t0) * 10));
    o = ghost_put(b, o, " ms\r\n");
    out_bytes(b, o);
}

static void cmd_sync(void) {
    long r = sync_();
    if (r >= 0) {
        char b[64];
        int o = ghost_put(b, 0, "qsh: sync ok (");
        o = ghost_put_u(b, o, (unsigned)r);
        o = ghost_put(b, o, " files flushed)\r\n");
        out_bytes(b, o);
    } else if (r == -5) {
        out("qsh: sync failed (no disk)\r\n");
    } else {
        char b[64];
        int o = ghost_put(b, 0, "qsh: sync failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    }
}

/* Print an initrd file raw (used for the motd greeting and `cat`).
 * Returns 0 on success, the negative errno if the open failed. */
static long print_file(const char *path) {
    long fd = open_(path);
    if (fd < 0) {
        return fd;
    }
    char buf[192];
    long n;
    char last = '\n';
    while ((n = read_(fd, buf, sizeof(buf))) > 0) {
        heartbeat();
        out_bytes(buf, n);
        last = buf[n - 1];
    }
    close_(fd);
    if (last != '\n') {
        out("\r\n");
    }
    return 0;
}

static void cmd_ls(const char *path) {
    static char buf[900];
    long n = readdir_(path, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    } else {
        out("qsh: ls: nothing found\r\n");
    }
}

static void cmd_cat(const char *path) {
    long err = print_file(path);
    if (err != 0) {
        char b[LINE_MAX + 48];
        int o = ghost_put(b, 0, "qsh: cat: cannot open '");
        for (int i = 0; path[i] && o < (int)sizeof(b) - 24; i++) {
            b[o++] = path[i];
        }
        o = ghost_put(b, o, "' (err ");
        o = ghost_put_u(b, o, (unsigned)(-err));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    }
}

static void cmd_ps(void) {
    static char buf[900];
    long n = sysinfo(SYSINFO_PS, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

/* Scheduler baseline (ADR-0022 prereq-2): print one SCHED: line — total switches,
 * timer-quantum preemptions, current tick, and the fairness/tail snapshot. Argument-
 * free (no agent-controlled bytes reach the SCHED: line) so it is not a marker-spoof
 * surface for the latency/baseline harness. */
static void cmd_sched(void) {
    static char buf[256];
    long n = sysinfo(SYSINFO_SCHED, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

static void cmd_free(void) {
    char buf[128];
    long n = sysinfo(SYSINFO_MEM, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

static void cmd_date(void) {
    char buf[64];
    long n = sysinfo(SYSINFO_TIME, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    } else {
        out("qsh: date: unavailable\r\n");
    }
}

static void cmd_uptime(void) {
    char b[64];
    unsigned long t = (unsigned long)ticks();
    int o = ghost_put(b, 0, "qsh: uptime ");
    o = ghost_put_u(b, o, (unsigned)t);
    o = ghost_put(b, o, " ticks (");
    o = ghost_put_u(b, o, (unsigned)(t / 100));
    o = ghost_put(b, o, " s)\r\n");
    out_bytes(b, o);
}

static void cmd_pid(void) {
    char b[32];
    int o = ghost_put(b, 0, "qsh: pid ");
    o = ghost_put_u(b, o, (unsigned)getpid());
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_qrand(void) {
    unsigned char rb[8];
    long r = qrand_fill(rb, sizeof(rb));
    if (r < 0) {
        out("qsh: qrand denied (EPERM)\r\n");
        return;
    }
    char b[48];
    int o = ghost_put(b, 0, "qsh: qrand ");
    for (int i = 0; i < (int)r; i++) {
        int hi = rb[i] >> 4, lo = rb[i] & 0xF;
        b[o++] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        b[o++] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
    }
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_qseed(void) {
    /* Probe the capability OUT-OF-BAND first: qrand_seed_present() returns only
     * {0,1} when the quantum-pool cap is held, and a negative errno (-4 EPERM)
     * when it is not — so it distinguishes denial unambiguously. SYS_QSEED
     * itself returns an UNCONSTRAINED 64-bit seed, so testing that value against
     * -4 (as this did) misreads the one valid seed equal to (uint64_t)-4, e.g.
     * from qseed=FFFFFFFFFFFFFFFC, as a capability denial. */
    if (qrand_seed_present() < 0) {
        out("qsh: qseed denied (EPERM)\r\n");
        return;
    }
    char b[40];
    int o = ghost_put(b, 0, "qsh: qseed ");
    o = put_hex64(b, o, (unsigned long long)qseed_value());
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

/* Query ghostd's field over capability-checked IPC: send a STATUS
 * request, poll the mailbox (yielding) for the reply, report R and the
 * live pattern count. ghostd replies to the sender via SYS_SEND_TO. */
static void cmd_ghost(void) {
    ghost_req_t req;
    req.op = GHOST_STATUS;
    req.slot = 0;
    req.pad[0] = req.pad[1] = 0;
    for (int w = 0; w < GHOST_PW; w++) {
        req.bits[w] = 0;
    }

    long r = send_msg((const char *)&req, sizeof(req));
    if (r < 0) {
        out("qsh: ghost send denied (EPERM)\r\n");
        return;
    }

    /* Poll for the reply, heartbeating: ghostd may be compute-bound in a
     * field relaxation for a while, and a silent wait would otherwise get
     * this shell watchdog-killed mid-command (found live: the phase-2
     * smoke run landed this query in ghostd's imprint window). */
    ghost_rep_t rep;
    long sender = 0;
    for (int tries = 0; tries < 5000 && sender == 0; tries++) {
        heartbeat();
        sender = recv_msg((char *)&rep, sizeof(rep));
        if (sender == 0) {
            yield();
        }
    }
    if (sender == 0) {
        out("qsh: ghost timeout (no reply)\r\n");
        return;
    }

    char b[64];
    int o = ghost_put(b, 0, "qsh: ghost R=");
    o = ghost_put_r(b, o, rep.r_q16);
    o = ghost_put(b, o, " live=");
    o = ghost_put_u(b, o, rep.live);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

/* `ghost <sub>` — today only `ghost exit` (ADR-0023): ask ghostd to
 * terminate so the watchdog rebirths it. Restarting the TARGET under this
 * LIVING shell is the one rebirth direction a qsh `exit` never exercises:
 * our stale ghostd cap must be unlinked at its death (Part 1) and the fresh
 * pair re-minted at its restart (Part 2) for the NEXT `ghost` to answer —
 * the ci-smoke Part-1 integration leg drives exactly that sequence. The op
 * is fire-and-forget (ghostd sends no reply; it is dying). */
static void cmd_ghost_arg(const char *sub) {
    if (!is_cmd(sub, "exit")) {
        out("qsh: ghost: usage: ghost [exit]\r\n");
        return;
    }
    ghost_req_t req;
    req.op = GHOST_EXIT;
    req.slot = 0;
    req.pad[0] = req.pad[1] = 0;
    for (int w = 0; w < GHOST_PW; w++) {
        req.bits[w] = 0;
    }
    if (send_msg((const char *)&req, sizeof(req)) < 0) {
        out("qsh: ghost exit send denied (EPERM)\r\n");
        return;
    }
    out("qsh: ghost exit sent — the watchdog will rebirth ghostd\r\n");
}

/* Start an initrd program (SYS_SPAWN — the shell's spawn capability at
 * work) and poll its fate with SYS_WAITPID, heartbeating so the watchdog
 * never mistakes the wait for a hang. */
static void cmd_run(const char *path) {
    long pid = spawn_(path);
    if (pid < 0) {
        char b[LINE_MAX + 48];
        int o = ghost_put(b, 0, "qsh: run: cannot start '");
        for (int i = 0; path[i] && o < (int)sizeof(b) - 24; i++) {
            b[o++] = path[i];
        }
        o = ghost_put(b, o, "' (err ");
        o = ghost_put_u(b, o, (unsigned)(-pid));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }

    char b[80];
    int o = ghost_put(b, 0, "qsh: spawned pid ");
    o = ghost_put_u(b, o, (unsigned)pid);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);

    long r = WAITPID_RUNNING;
    for (int tries = 0; tries < 5000 && r == WAITPID_RUNNING; tries++) {
        heartbeat();
        r = waitpid_(pid);
        if (r == WAITPID_RUNNING) {
            yield();
        }
    }

    o = ghost_put(b, 0, "qsh: pid ");
    o = ghost_put_u(b, o, (unsigned)pid);
    if (r >= 0 && r < 256) {
        o = ghost_put(b, o, " exited (code ");
        o = ghost_put_u(b, o, (unsigned)r);
        o = ghost_put(b, o, ")\r\n");
    } else if (r == WAITPID_RUNNING) {
        o = ghost_put(b, o, " still running (wait timed out)\r\n");
    } else {
        o = ghost_put(b, o, " vanished (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
    }
    out_bytes(b, o);
}

static void cmd_unknown(const char *line) {
    char b[LINE_MAX + 48];
    int o = ghost_put(b, 0, "qsh: unknown command '");
    for (int i = 0; line[i] && o < (int)sizeof(b) - 24; i++) {
        b[o++] = line[i];
    }
    o = ghost_put(b, o, "' — try 'help'\r\n");
    out_bytes(b, o);
}

/* ------------------------------------------------------------------ *
 * qpu: hand a small circuit to the HOST's quantum broker (#262).
 *
 * The guest holds no credentials and has no network. It writes one
 * request on COM1 as an OSC escape, ESC ] qpu ; <op> ; <tag> ; ... BEL,
 * and waits for the host's answer, ESC ] qres ; <tag> ; <state> ;
 * <job> ; <text> BEL. On the public console the launcher strips the
 * request out of the visitor's stream, adds the session identity the
 * guest cannot forge, and forwards it to the broker, which validates
 * the circuit, applies the spending policy and talks to qBraid. With no
 * host broker (QEMU, a laptop) nothing answers and the command says so.
 *
 * Every line printed for a result comes from the host and names the
 * device that ran it ("qBraid simulator ..." or "QPU ...").
 *   ops: I = info, S = submit (mode s = simulator, q = real QPU),
 *        J = job status/result.
 *   states: Q queued, R running, D done, E error/refused, I info.
 * ------------------------------------------------------------------ */

#define QPU_REPLY_MAX 600
#define QPU_WAIT_TICKS 1500 /* 15 s for one answer from the host */
#define QPU_SIM_TICKS 9000  /* 90 s for a simulator job to finish */

static unsigned qpu_tag_seq;

static int qpu_op(unsigned char *c, int o, int op, int a, int b) {
    c[o] = (unsigned char)op;
    c[o + 1] = (unsigned char)a;
    c[o + 2] = (unsigned char)b;
    return o + 3;
}

/* The wire bytes of a canonical circuit (user/qpu_circuit.h); returns the
 * length, or 0 for an unknown name. */
static int qpu_circuit_of(const char *name, unsigned char *c) {
    for (int i = 0; i < QC_HDR; i++) {
        c[i] = 0;
    }
    c[0] = QC_VERSION;
    int o = QC_HDR;
    if (is_cmd(name, "bell")) {
        c[1] = 2;
        c[2] = 2;
        o = qpu_op(c, o, QC_OP_H, 0, 0);
        o = qpu_op(c, o, QC_OP_CNOT, 0, 1);
    } else if (is_cmd(name, "ghz3")) {
        c[1] = 3;
        c[2] = 3;
        o = qpu_op(c, o, QC_OP_H, 0, 0);
        o = qpu_op(c, o, QC_OP_CNOT, 0, 1);
        o = qpu_op(c, o, QC_OP_CNOT, 1, 2);
    } else if (is_cmd(name, "grover3")) {
        /* H on 3 qubits, then 2 x (oracle |101> + diffusion): p(5) = 121/128 */
        c[1] = 3;
        c[2] = 7;
        c[3] = 5;
        for (int q = 0; q < 3; q++) {
            o = qpu_op(c, o, QC_OP_H, q, 0);
        }
        for (int k = 0; k < 2; k++) {
            o = qpu_op(c, o, QC_OP_ORACLE, 5, 0);
            o = qpu_op(c, o, QC_OP_DIFFUSION, 0, 0);
        }
    } else {
        return 0;
    }
    return o;
}

static int qpu_put_hex(char *b, int o, const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) {
        int hi = p[i] >> 4, lo = p[i] & 0xF;
        b[o++] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        b[o++] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
    }
    return o;
}

/* Wait for ESC ] qres ; <tag> ; ... BEL. Fills reply with the part after
 * "<tag>;" and returns its length; -1 on timeout, -2 if the visitor
 * pressed Ctrl-C. Other input that arrives while waiting is dropped. */
static int qpu_await(const char *tag, char *reply, long max_ticks) {
    static const char pre[] = "\x1b]qres;";
    int pi = 0; /* bytes of the prefix matched */
    int in = 0; /* inside a reply body */
    int rl = 0;
    long t0 = ticks();
    while (ticks() - t0 < max_ticks) {
        heartbeat();
        unsigned char chunk[64];
        long n = cons_read(chunk, sizeof(chunk));
        if (n <= 0) {
            yield();
            continue;
        }
        for (long i = 0; i < n; i++) {
            char ch = (char)chunk[i];
            if (!in) {
                if (ch == 3) {
                    return -2;
                }
                if (ch == pre[pi]) {
                    pi++;
                    if (pre[pi] == '\0') {
                        in = 1;
                        rl = 0;
                        pi = 0;
                    }
                } else {
                    pi = (ch == pre[0]) ? 1 : 0;
                }
                continue;
            }
            if (ch == 7) {
                reply[rl] = '\0';
                int tl = str_len(tag);
                int ok = rl > tl && reply[tl] == ';';
                for (int k = 0; ok && k < tl; k++) {
                    ok = reply[k] == tag[k];
                }
                in = 0;
                if (ok) {
                    int m = 0;
                    for (int k = tl + 1; k <= rl; k++) {
                        reply[m++] = reply[k];
                    }
                    return m - 1;
                }
                continue; /* a stale answer to an earlier request */
            }
            if (rl < QPU_REPLY_MAX - 1) {
                reply[rl++] = ch;
            } else {
                in = 0; /* oversized: drop it */
            }
        }
    }
    return -1;
}

/* Send one request (body after the tag) and wait for its answer. */
static int qpu_ask(const char op, const char *body, char *reply, long max_ticks) {
    char tag[12];
    int t = ghost_put_u(tag, 0, ++qpu_tag_seq);
    tag[t] = '\0';
    char req[320];
    int o = ghost_put(req, 0, "\x1b]qpu;");
    req[o++] = op;
    req[o++] = ';';
    o = ghost_put(req, o, tag);
    if (body[0]) {
        req[o++] = ';';
        for (int i = 0; body[i] && o < (int)sizeof(req) - 2; i++) {
            req[o++] = body[i];
        }
    }
    req[o++] = '\x07';
    out_bytes(req, o);
    return qpu_await(tag, reply, max_ticks);
}

/* Print the reply's text field ("<state>;<job>;<line>|<line>...") and
 * return its state letter; job (if non-null) gets the job id. */
static char qpu_show(const char *reply, char *job, int job_max) {
    char state = reply[0];
    int i = 0;
    while (reply[i] && reply[i] != ';') {
        i++;
    }
    if (reply[i] == ';') {
        i++;
    }
    int j = 0;
    while (reply[i] && reply[i] != ';') {
        if (job && j < job_max - 1) {
            job[j++] = reply[i];
        }
        i++;
    }
    if (job) {
        job[j] = '\0';
    }
    if (reply[i] == ';') {
        i++;
    }
    char b[160];
    int o = ghost_put(b, 0, "qpu: ");
    for (; reply[i]; i++) {
        char c = reply[i];
        if (c == '|' || o >= (int)sizeof(b) - 3) {
            b[o++] = '\r';
            b[o++] = '\n';
            out_bytes(b, o);
            o = ghost_put(b, 0, "     ");
            if (c == '|') {
                continue;
            }
        }
        if (c >= 32 && c < 127) {
            b[o++] = c;
        }
    }
    b[o++] = '\r';
    b[o++] = '\n';
    out_bytes(b, o);
    return state;
}

static void qpu_no_answer(int r) {
    if (r == -2) {
        out("qpu: stopped waiting (the job, if any, keeps its id on the host)\r\n");
    } else {
        out("qpu: no answer from a host quantum broker (none is attached to this machine)\r\n");
    }
}

static void cmd_qpu_usage(void) {
    out(A_CMD "qpu run" A0 " <bell|ghz3|grover3> [shots]   " A_DIM
              "run on qBraid's simulator (free)" A0 "\r\n");
    out(A_CMD "qpu real" A0 " <bell|ghz3|grover3> [shots]  " A_DIM
              "queue on a real QPU, within the daily budget" A0 "\r\n");
    out(A_CMD "qpu job" A0 " <id>                          " A_DIM "status or counts of a job" A0
              "\r\n");
}

static void cmd_qpu(const char *args) {
    static char reply[QPU_REPLY_MAX];
    const char *a;
    char job[48];
    if (args == 0 || args[0] == '\0') {
        cmd_qpu_usage();
        int r = qpu_ask('I', "", reply, QPU_WAIT_TICKS);
        if (r < 0) {
            qpu_no_answer(r);
        } else {
            qpu_show(reply, 0, 0);
        }
        return;
    }
    if ((a = arg_of(args, "job")) != 0) {
        char body[64];
        int o = 0;
        for (; a[o] && a[o] != ' ' && o < (int)sizeof(body) - 1; o++) {
            body[o] = a[o];
        }
        body[o] = '\0';
        int r = qpu_ask('J', body, reply, QPU_WAIT_TICKS);
        if (r < 0) {
            qpu_no_answer(r);
        } else {
            qpu_show(reply, 0, 0);
        }
        return;
    }
    int real = 0;
    if ((a = arg_of(args, "real")) != 0) {
        real = 1;
    } else if ((a = arg_of(args, "run")) == 0) {
        cmd_qpu_usage();
        return;
    }
    char name[16];
    int nl = 0;
    while (a[nl] && a[nl] != ' ' && nl < (int)sizeof(name) - 1) {
        name[nl] = a[nl];
        nl++;
    }
    name[nl] = '\0';
    unsigned shots = 0;
    const char *sp = a + nl;
    while (*sp == ' ') {
        sp++;
    }
    while (*sp >= '0' && *sp <= '9' && shots < 100000) {
        shots = shots * 10 + (unsigned)(*sp - '0');
        sp++;
    }
    unsigned char circ[QC_HDR + 3 * 8];
    int cl = qpu_circuit_of(name, circ);
    if (cl == 0) {
        out("qpu: circuits: bell, ghz3, grover3\r\n");
        return;
    }
    char body[160];
    int o = 0;
    body[o++] = real ? 'q' : 's';
    body[o++] = ';';
    o = ghost_put_u(body, o, shots);
    body[o++] = ';';
    o = qpu_put_hex(body, o, circ, cl);
    body[o] = '\0';
    int r = qpu_ask('S', body, reply, QPU_WAIT_TICKS);
    if (r < 0) {
        qpu_no_answer(r);
        return;
    }
    char st = qpu_show(reply, job, sizeof(job));
    if (real || (st != 'Q' && st != 'R') || job[0] == '\0') {
        return;
    }
    /* A simulator job usually finishes in well under a minute: poll it. */
    long t0 = ticks();
    while (ticks() - t0 < QPU_SIM_TICKS) {
        long w = ticks();
        while (ticks() - w < 300) {
            heartbeat();
            yield();
        }
        r = qpu_ask('J', job, reply, QPU_WAIT_TICKS);
        if (r < 0) {
            qpu_no_answer(r);
            return;
        }
        if (reply[0] != 'Q' && reply[0] != 'R') {
            qpu_show(reply, 0, 0);
            return;
        }
    }
    out("qpu: still running; check later with: qpu job ");
    out(job);
    out("\r\n");
}

static void execute(const char *line) {
    /* strip leading spaces */
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    const char *a;
    if (is_cmd(line, "help")) {
        cmd_help();
    } else if ((a = arg_of(line, "echo")) != 0) {
        char b[LINE_MAX + 16];
        int o = ghost_put(b, 0, "qsh: ");
        for (int i = 0; a[i] && o < (int)sizeof(b) - 4; i++) {
            b[o++] = a[i];
        }
        o = ghost_put(b, o, "\r\n");
        out_bytes(b, o);
    } else if (is_cmd(line, "echo")) {
        out("qsh: \r\n");
    } else if (is_cmd(line, "ps")) {
        cmd_ps();
    } else if (is_cmd(line, "sched")) {
        cmd_sched();
    } else if ((a = arg_of(line, "ls")) != 0) {
        cmd_ls(a);
    } else if (is_cmd(line, "ls")) {
        cmd_ls("/");
    } else if ((a = arg_of(line, "cat")) != 0) {
        cmd_cat(a);
    } else if (is_cmd(line, "cat")) {
        out("qsh: cat: usage: cat <path>\r\n");
    } else if ((a = arg_of(line, "run")) != 0) {
        cmd_run(a);
    } else if (is_cmd(line, "run")) {
        out("qsh: run: usage: run <path>\r\n");
    } else if ((a = arg_of(line, "write")) != 0) {
        cmd_write(a);
    } else if (is_cmd(line, "write")) {
        out("qsh: write: usage: write <path> <text>\r\n");
    } else if ((a = arg_of(line, "rm")) != 0) {
        cmd_rm(a);
    } else if (is_cmd(line, "sync")) {
        cmd_sync();
    } else if ((a = arg_of(line, "imprint")) != 0) {
        cmd_imprint(a);
    } else if (is_cmd(line, "imprint")) {
        out("qsh: imprint: usage: imprint <text>\r\n");
    } else if ((a = arg_of(line, "recall")) != 0) {
        cmd_recall(a);
    } else if (is_cmd(line, "recall")) {
        out("qsh: recall: usage: recall <probe text>\r\n");
    } else if (is_cmd(line, "fieldtest")) {
        cmd_fieldtest();
    } else if (is_cmd(line, "field")) {
        cmd_field();
    } else if (is_cmd(line, "audit")) {
        cmd_audit();
    } else if (is_cmd(line, "egress")) {
        cmd_egress();
    } else if (is_cmd(line, "manifest")) {
        cmd_manifest();
    } else if ((a = arg_of(line, "net2")) != 0) {
        cmd_net2(a);
    } else if (is_cmd(line, "net2")) {
        out("qsh: net2: usage: net2 <peer-ip>\r\n");
    } else if ((a = arg_of(line, "nslookup")) != 0) {
        cmd_nslookup(a);
    } else if (is_cmd(line, "nslookup")) {
        out("qsh: nslookup: usage: nslookup <host>\r\n");
    } else if ((a = arg_of(line, "udping")) != 0) {
        cmd_udping(a);
    } else if (is_cmd(line, "udping")) {
        out("qsh: udping: usage: udping <host>\r\n");
    } else if ((a = arg_of(line, "http")) != 0) {
        cmd_http(a);
    } else if (is_cmd(line, "http")) {
        out("qsh: http: usage: http <host> [port]\r\n");
    } else if (is_cmd(line, "free")) {
        cmd_free();
    } else if (is_cmd(line, "uptime")) {
        cmd_uptime();
    } else if (is_cmd(line, "date")) {
        cmd_date();
    } else if (is_cmd(line, "pid")) {
        cmd_pid();
    } else if (is_cmd(line, "qrand")) {
        cmd_qrand();
    } else if ((a = arg_of(line, "qpu")) != 0) {
        cmd_qpu(a);
    } else if (is_cmd(line, "qpu")) {
        cmd_qpu(0);
    } else if (is_cmd(line, "qseed")) {
        cmd_qseed();
    } else if ((a = arg_of(line, "ghost")) != 0) {
        cmd_ghost_arg(a);
    } else if (is_cmd(line, "ghost")) {
        cmd_ghost();
    } else if (is_cmd(line, "clear")) {
        out("\x1b[2J\x1b[H");
    } else if (is_cmd(line, "exit")) {
        out("qsh: exiting — the watchdog will restart me\r\n");
        exit_(0);
    } else {
        cmd_unknown(line);
    }
}

/* ------------------------------------------------------------------ *
 * Main loop: heartbeat, drain input, edit the line, execute.
 * ------------------------------------------------------------------ */

void _start(void) {
    long restarts = svc_restarts();
    if (restarts > 0) {
        char b[48];
        int o = ghost_put(b, 0, "QSH: reborn (restart=");
        o = ghost_put_u(b, o, (unsigned)restarts);
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    } else {
        /* The phrase up to "ready" is the browser demo's boot marker and a CI
         * gate — keep it intact; colour codes sit outside it. */
        out(A_TITLE "QSH: QuantumOS interactive shell ready" A0 A_DIM " — type " A_KEY "help" A_DIM
                    "\r\n" A0);
    }

    /* Greet with the message of the day — read through the VFS, off the
     * embedded initrd. The first file the OS ever serves to a user. */
    {
        long err = print_file("/etc/motd");
        if (err != 0) {
            char b[48];
            int o = ghost_put(b, 0, "QSH: motd open failed (err ");
            o = ghost_put_u(b, o, (unsigned)(-err));
            o = ghost_put(b, o, ")\r\n");
            out_bytes(b, o);
        }
    }

    /* A one-line nudge so a first-time visitor knows what to try. */
    if (restarts == 0) {
        out(A_DIM "  try " A_KEY "ghost" A0 A_DIM "   " A_KEY "run /bin/qtop" A0 A_DIM
                  "   or " A_KEY "imprint hello world" A0 A_DIM " then " A_KEY
                  "recall hxllo wxrld" A0 "\r\n");
    }

    prompt();

    char line[LINE_MAX];
    int len = 0;
    int esc = 0;      /* 0 none, 1 got ESC, 2 inside CSI, 3 inside OSC */
    char prev_cr = 0; /* last byte was CR (suppress the LF of a CRLF pair) */

    for (;;) {
        heartbeat();

        unsigned char chunk[32];
        long n = cons_read(chunk, sizeof(chunk));
        if (n < 0) {
            /* No console capability — nothing a shell can do. The plain
             * SYS_WRITE path needs no cap, so the failure is loggable. */
            write_str("QSH: console read denied — shell exiting");
            exit_(1);
        }
        if (n == 0) {
            yield();
            continue;
        }

        for (long i = 0; i < n; i++) {
            char c = (char)chunk[i];

            /* Swallow ANSI escape sequences (arrow keys over serial send
             * ESC [ A etc.) instead of letting the residue pollute the
             * line — mirrors the PS/2 path's unmapped extended keys. */
            if (esc == 1) {
                esc = (c == '[') ? 2 : (c == ']') ? 3 : 0;
                continue;
            }
            if (esc == 3) {
                /* An OSC string (ESC ] ... BEL), e.g. a late answer from the
                 * host's quantum broker: never part of a command line. */
                if (c == 7 || c == '\r' || c == '\n') {
                    esc = 0;
                } else if (c == 27) {
                    esc = 1;
                }
                continue;
            }
            if (esc == 2) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                    esc = 0;
                }
                continue;
            }
            if (c == 27) {
                esc = 1;
                continue;
            }

            /* A CRLF pair is ONE Enter: the CR executed the line, so the
             * trailing LF must not run it again (double prompt). */
            if (c == '\n' && prev_cr) {
                prev_cr = 0;
                continue;
            }
            prev_cr = (c == '\r');

            if (c == '\r' || c == '\n') {
                out("\r\n");
                /* Trim trailing spaces so "ps " matches the builtin. */
                while (len > 0 && line[len - 1] == ' ') {
                    len--;
                }
                line[len] = '\0';
                execute(line);
                len = 0;
                prompt();
            } else if (c == 8 || c == 127) { /* backspace / DEL */
                if (len > 0) {
                    len--;
                    out("\b \b");
                }
            } else if (c >= 32 && c < 127 && len < LINE_MAX - 1) {
                line[len++] = c;
                out_bytes(&c, 1); /* echo */
            }
        }
    }
}
