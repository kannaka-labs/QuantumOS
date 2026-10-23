/**
 * QuantumOS agentd — the agent-native end-to-end demo (the mission showcase).
 *
 * One ring-3 citizen that exercises the whole agentic stack in a single story,
 * each step with a verifiable outcome (agentd carries no engine of its own — the
 * results can only have come back through the kernel's brokers):
 *
 *   1. QPU      — submits a Bell circuit through the SYS_QPU broker and checks
 *                 the EXACT probability qpud computed, p(00) = 1/2.
 *   2. FIELD    — imprints a phrase into holographic field region 3 and recalls
 *                 it from a deterministically CORRUPTED probe (wave-interference
 *                 associative memory), verifying the exact stored bytes return.
 *   3. SPAWN    — starts /bin/hello as a sub-process and waits for it to exit
 *                 (real spawn authority — only a citizen holding the cap can).
 *   4. DELEGATE — SPAWNS its own society of AGENT_SUBS /bin/agentsub citizens
 *                 (each spawn mints a parent<->child IPC channel — the
 *                 spawn-channel grant, epic #175 — so the roster is the spawn
 *                 returns, with no kernel hand-wiring), then hands each a
 *                 strictly-NARROWED READ-only slice of its region-3 field cap
 *                 via SYS_CAP_DERIVE over that spawn-minted pair; every
 *                 sub-agent recalls the phrase from its OWN pid-salted noisy
 *                 probe and acks the DIGEST of what it recalled; the
 *                 orchestrator requires a matching digest from each distinct
 *                 delegate (content consensus, deduped by vouched sender pid).
 *                 This is "an orchestrator assembles sub-agents and hands each
 *                 a narrowed intent" — a one-hop fan-out tree (CAP_GRANT never
 *                 handed over, so no sub can re-delegate).
 *
 * On all four it prints the single CI merge-gate line
 *   AGENTD: DEMO OK qpu+field+spawn+society
 * Grants: qpu_submit(quota) + field(region 3) + field_delegable + spawn. NOT
 * monitored (a watchdog respawn would re-run the one-shot proof); exits after.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "usys.h"
#include "qpu_circuit.h"

#define AGENT_REGION 3
#define AGENT_SUBS 3 /* the society: agentd delegates to this many sub-agents */
static const unsigned char AGENT_PHRASE[] = "agentd end to end field phrase";
#define AGENT_PHRASE_LEN (sizeof(AGENT_PHRASE) - 1)

static unsigned int get_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

/* FNV-1a — MUST mirror fnv1a() in agentsub.c: each sub digests the winner it
 * actually recalled; the orchestrator digests the phrase it imprinted and
 * requires every ack to match (content consensus, not a bare "proven"). */
static unsigned int fnv1a(const unsigned char *p, unsigned len) {
    unsigned int h = 2166136261u;
    for (unsigned i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Parse the 8 lowercase-hex digits of a sub's "p<digest>" ack. Malformed input
 * returns a sentinel that cannot match the phrase digest in practice. */
static unsigned int hex32(const char *p) {
    unsigned int v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        unsigned int d;
        if (c >= '0' && c <= '9') {
            d = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned int)(c - 'a') + 10u;
        } else {
            return 0xffffffffu;
        }
        v = (v << 4) | d;
    }
    return v;
}

/* Submit an opaque circuit and poll it to completion (the qpu_test pattern).
 * Returns the job id (>0) with the DONE result in *out, or <=0 on failure. */
static long submit_and_wait(const unsigned char *circuit, unsigned int len, qpu_poll_out_t *out) {
    qpu_submit_req_t req;
    req.circuit_len = len;
    for (unsigned int i = 0; i < len; i++) {
        req.circuit[i] = circuit[i];
    }
    long jid = qpu_submit_(&req);
    if (jid <= 0) {
        return jid;
    }
    for (int spins = 0; spins < 2000000; spins++) {
        long st = qpu_poll_(jid, out);
        if (st == QPU_POLL_DONE) {
            return jid;
        }
        if (st < 0) {
            return st;
        }
        yield();
    }
    return -100; /* timed out waiting for qpud */
}

/* Step 1 — a Bell pair through the broker: p(00) must be exactly 1/2. */
static int do_qpu(void) {
    unsigned char bell[QC_HDR + 2 * 3];
    bell[0] = QC_VERSION;
    bell[1] = 2; /* qubits */
    bell[2] = 2; /* ops */
    bell[3] = 0; /* probe |00> */
    bell[4] = 0;
    bell[5] = bell[6] = bell[7] = 0;
    bell[8] = QC_OP_H;
    bell[9] = 0;
    bell[10] = 0;
    bell[11] = QC_OP_CNOT;
    bell[12] = 0;
    bell[13] = 1;
    qpu_poll_out_t out;
    long j = submit_and_wait(bell, sizeof(bell), &out);
    if (j <= 0 || out.status != QPU_STATUS_OK || out.result_len != QC_RESULT_LEN) {
        printf("AGENT BROKEN qpu submit/poll j=%ld\n", j);
        return 0;
    }
    unsigned int num = get_u32(out.result + 4);
    unsigned int den = get_u32(out.result + 8);
    if (num == 1 && den == 2) {
        printf("AGENT: " QPU_BACKEND_LABEL ": bell via broker job=%ld p=1/2 (exact)\n", j);
        return 1;
    }
    printf("AGENT BROKEN qpu p=%u/%u\n", num, den);
    return 0;
}

/* Step 2 — imprint region 3, then recall it from a corrupted probe. */
static int do_field(void) {
    field_imprint_req_t ireq;
    for (unsigned i = 0; i < sizeof(ireq); i++) {
        ((unsigned char *)&ireq)[i] = 0;
    }
    ireq.region = AGENT_REGION;
    ireq.len = AGENT_PHRASE_LEN;
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        ireq.pattern[n] = AGENT_PHRASE[n];
    }
    if (imprint_(&ireq) < 0) {
        write_str("AGENT BROKEN field imprint");
        return 0;
    }

    field_recall_req_t rreq;
    field_recall_out_t rout;
    for (unsigned i = 0; i < sizeof(rreq); i++) {
        ((unsigned char *)&rreq)[i] = 0;
    }
    rreq.region = AGENT_REGION;
    rreq.len = AGENT_PHRASE_LEN;
    rreq.k = 1;
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        rreq.probe[n] = AGENT_PHRASE[n];
    }
    rreq.probe[1] ^= 0x20; /* corrupt two bytes — the probe is deliberately noisy */
    rreq.probe[5] ^= 0x20;
    if (recall_(&rreq, &rout) != 0 || rout.winner_len != AGENT_PHRASE_LEN) {
        write_str("AGENT BROKEN field recall");
        return 0;
    }
    for (unsigned n = 0; n < AGENT_PHRASE_LEN; n++) {
        if (rout.winner[n] != AGENT_PHRASE[n]) {
            write_str("AGENT BROKEN field winner mismatch");
            return 0;
        }
    }
    write_str("AGENT: field recalled the phrase from a noisy probe");
    return 1;
}

/* Step 3 — spawn /bin/hello and wait for it to exit. */
static int do_spawn(void) {
    long pid = spawn_("/bin/hello");
    if (pid <= 0) {
        printf("AGENT BROKEN spawn=%ld\n", pid);
        return 0;
    }
    for (long spins = 0; spins < 8000000L; spins++) {
        long w = waitpid_(pid);
        if (w != WAITPID_RUNNING) {
            printf("AGENT: spawned /bin/hello pid=%ld exit=%ld\n", pid, w);
            return 1;
        }
        yield();
    }
    write_str("AGENT BROKEN spawn waitpid timeout");
    return 0;
}

/* Step 4 — the SOCIETY, self-assembled: SPAWN AGENT_SUBS /bin/agentsub
 * citizens (each spawn mints the parent<->child IPC channel — agentd holds the
 * spawn-channel grant, so the roster IS the spawn returns; no kernel
 * hand-wiring), collect a liveness "ready" from EACH child, then delegate a
 * narrowed READ slice of region 3 to each (a one-hop fan-out tree — CAP_GRANT
 * is never handed over, so no sub can re-delegate) and collect a
 * digest-bearing ack from every one for content CONSENSUS. cap_derive's
 * IPC-peer requirement is satisfied by the spawn-minted pair. */
static int do_delegate(void) {
    char buf[16];
    long subs[AGENT_SUBS];

    /* Assemble our own society. */
    for (int i = 0; i < AGENT_SUBS; i++) {
        long p = spawn_("/bin/agentsub");
        if (p <= 0) {
            printf("AGENT BROKEN society spawn[%d]=%ld\n", i, p);
            return 0;
        }
        subs[i] = p;
    }

    /* Collect a liveness "ready" from EACH spawned child — the kernel-vouched
     * sender pid must be one of OUR spawns (an unknown or duplicate sender
     * never counts toward the roster). */
    int ready_from[AGENT_SUBS] = {0};
    int n = 0;
    for (long spins = 0; spins < 20000000L && n < AGENT_SUBS; spins++) {
        long s = recv_msg(buf, sizeof(buf));
        if (s != 0 && buf[0] == 'r') {
            for (int i = 0; i < AGENT_SUBS; i++) {
                if (subs[i] == s && !ready_from[i]) {
                    ready_from[i] = 1;
                    n++;
                }
            }
        } else {
            yield();
        }
    }
    if (n < AGENT_SUBS) {
        printf("AGENT BROKEN society only %d/%d ready\n", n, AGENT_SUBS);
        return 0;
    }

    /* Delegate to each specialist — TWO narrowed slices (epic #177): the
     * shared region 3 READ-only (no WRITE, no GRANT), and its PRIVATE
     * workspace region 4+i READ|WRITE (still no GRANT — one-hop tree). Both
     * derives ride the spawn-minted IPC pair (the peer requirement). Only
     * then release it with an ADDRESSED "g<digit>" naming its workspace. */
    for (int i = 0; i < AGENT_SUBS; i++) {
        cap_derive_req_t req;
        for (unsigned b = 0; b < sizeof(req); b++) {
            ((unsigned char *)&req)[b] = 0;
        }
        req.resource_type = CAP_RESOURCE_FIELD;
        req.resource_id = AGENT_REGION;
        req.permissions = CAP_READ; /* narrowed: no WRITE, no GRANT */
        req.target_pid = (unsigned)subs[i];
        req.expiration = 0;
        long d = cap_derive_(&req);
        if (d != 0) {
            printf("AGENT BROKEN cap_derive[%d]=%ld\n", i, d);
            return 0;
        }
        for (unsigned b = 0; b < sizeof(req); b++) {
            ((unsigned char *)&req)[b] = 0;
        }
        req.resource_type = CAP_RESOURCE_FIELD;
        req.resource_id = 4u + (unsigned)i;     /* the specialist's own workspace */
        req.permissions = CAP_READ | CAP_WRITE; /* it PUBLISHES here; still no GRANT */
        req.target_pid = (unsigned)subs[i];
        req.expiration = 0;
        d = cap_derive_(&req);
        if (d != 0) {
            printf("AGENT BROKEN workspace cap_derive[%d]=%ld\n", i, d);
            return 0;
        }
        char go[3];
        go[0] = 'g';
        go[1] = (char)('4' + i);
        go[2] = 0;
        send_to(subs[i], go, 2);
    }

    /* Collect a digest-bearing "p<hex>" ack from EVERY delegated sub-agent —
     * deduped by the kernel-vouched sender pid (N acks from one sub are NOT a
     * society) — and check each digest against our own digest of the phrase:
     * consensus over the CONTENT each sub actually recalled, not a bare ack. */
    unsigned int want = fnv1a(AGENT_PHRASE, AGENT_PHRASE_LEN);
    int proven_from[AGENT_SUBS] = {0};
    int proven = 0;
    for (long spins = 0; spins < 60000000L && proven < AGENT_SUBS; spins++) {
        for (unsigned b = 0; b < sizeof(buf); b++) {
            buf[b] = 0;
        }
        long s = recv_msg(buf, sizeof(buf));
        if (s == 0 || buf[0] != 'p') {
            yield();
            continue;
        }
        int idx = -1;
        for (int i = 0; i < AGENT_SUBS; i++) {
            if (subs[i] == s) {
                idx = i;
            }
        }
        if (idx < 0 || proven_from[idx]) {
            continue; /* not one of our delegates, or a duplicate — never counts */
        }
        unsigned int dig = hex32(buf + 1);
        if (dig != want) {
            printf("AGENT BROKEN society digest %08x != %08x from pid=%ld\n", dig, want, s);
            return 0;
        }
        proven_from[idx] = 1;
        proven++;
    }
    if (proven < AGENT_SUBS) {
        printf("AGENT BROKEN society only %d/%d proven\n", proven, AGENT_SUBS);
        return 0;
    }
    printf("AGENT: society consensus %d/%d on region 3 (digest %08x, READ-only delegates)\n",
           proven, AGENT_SUBS, want);

    /* DIVISION OF LABOR verification (epic #177) — INSIDE do_delegate so the
     * DEMO OK line stays a conjunction over EVERY step. For each workspace:
     * (a) field_info must show EXACTLY one live slot (the region was scrubbed
     * at grant and the specialist imprinted once — live==1 is what makes the
     * recall below meaningful, since a 1-slot recall always returns that
     * slot: the byte compare plus this count are the load-bearing checks);
     * (b) a noisy recall must return the EXACT result this orchestrator
     * recomputes independently from the shared phrase + workspace digit. */
    for (int i = 0; i < AGENT_SUBS; i++) {
        unsigned ws = 4u + (unsigned)i;
        field_info_out_t fi;
        for (unsigned b = 0; b < sizeof(fi); b++) {
            ((unsigned char *)&fi)[b] = 0;
        }
        if (field_info_(ws, &fi) != 0 || fi.live != 1) {
            printf("AGENT BROKEN division region %u live=%u (want 1)\n", ws, fi.live);
            return 0;
        }
        unsigned int rd = fnv1a(AGENT_PHRASE, AGENT_PHRASE_LEN);
        rd = (rd ^ (unsigned int)('0' + ws)) * 16777619u; /* mirrors agentsub */
        char expect[12];
        snprintf(expect, sizeof(expect), "w%08x", rd);
        field_recall_req_t rreq;
        field_recall_out_t rout;
        for (unsigned b = 0; b < sizeof(rreq); b++) {
            ((unsigned char *)&rreq)[b] = 0;
        }
        rreq.region = ws;
        rreq.len = 9;
        rreq.k = 1;
        for (unsigned n = 0; n < 9; n++) {
            rreq.probe[n] = (unsigned char)expect[n];
        }
        rreq.probe[2] ^= 0x01; /* noisy probe — two corrupted hex positions */
        rreq.probe[6] ^= 0x01;
        if (recall_(&rreq, &rout) != 0 || rout.winner_len != 9) {
            printf("AGENT BROKEN division recall region %u len=%u\n", ws, rout.winner_len);
            return 0;
        }
        for (unsigned n = 0; n < 9; n++) {
            if (rout.winner[n] != (unsigned char)expect[n]) {
                printf("AGENT BROKEN division region %u content mismatch\n", ws);
                return 0;
            }
        }
    }
    printf("AGENT: division of labor %d/%d - every specialist published in its own region\n",
           AGENT_SUBS, AGENT_SUBS);

    /* SOCIETY OF SOCIETIES (epic #178): hand this society's AGGREGATE to
     * fieldsyncd, which broadcasts it to coupled peer kernels (and prints any
     * peer society's aggregate it receives, for host-side verification). The
     * aggregate is fnv1a over the three specialist results, salted with the
     * boot identity (qseed) so two VMs' aggregates provably differ — the host
     * knows both qseeds and recomputes both expected values. Inside
     * do_delegate so DEMO OK stays a conjunction over this step too. */
    unsigned int agg = 2166136261u;
    for (int i = 0; i < AGENT_SUBS; i++) {
        unsigned ws = 4u + (unsigned)i;
        unsigned int rd = fnv1a(AGENT_PHRASE, AGENT_PHRASE_LEN);
        rd = (rd ^ (unsigned int)('0' + ws)) * 16777619u;
        char rs[12];
        snprintf(rs, sizeof(rs), "w%08x", rd);
        for (unsigned n = 0; n < 9; n++) {
            agg = (agg ^ (unsigned char)rs[n]) * 16777619u;
        }
    }
    /* Identity salt: FNV-1a over the qseed's 8 LE bytes — NOT hi32^lo32,
     * which collapses to ZERO for any qseed with equal halves (both CI
     * society qseeds are palindromic; the gate's vacuity self-check caught
     * exactly that). */
    unsigned long long qs = (unsigned long long)qseed_value();
    unsigned char qb[8];
    for (unsigned i = 0; i < 8; i++) {
        qb[i] = (unsigned char)(qs >> (8u * i));
    }
    agg ^= fnv1a(qb, 8);

    /* fieldsyncd's pid comes from the uncapped SYSINFO_PS text (the qtop
     * pattern): one "PS: <pid> <name> STATE" line per live process. */
    static char ps[2048];
    long pn = sysinfo(SYSINFO_PS, ps, sizeof(ps) - 1);
    ps[(pn > 0) ? pn : 0] = '\0';
    long fs_pid = 0;
    for (long o = 0; ps[o]; o++) {
        if ((o == 0 || ps[o - 1] == '\n') && ps[o] == 'P' && ps[o + 1] == 'S' && ps[o + 2] == ':' &&
            ps[o + 3] == ' ') {
            long p = 0, k = o + 4;
            while (ps[k] >= '0' && ps[k] <= '9') {
                p = p * 10 + (ps[k] - '0');
                k++;
            }
            if (ps[k] == ' ' && ps[k + 1] == 'f' && ps[k + 2] == 'i' && ps[k + 3] == 'e' &&
                ps[k + 4] == 'l' && ps[k + 5] == 'd' && ps[k + 6] == 's' && ps[k + 7] == 'y') {
                fs_pid = p;
                break;
            }
        }
    }
    if (fs_pid == 0) {
        write_str("AGENT BROKEN aggregate: fieldsyncd not in PS");
        return 0;
    }
    char am[12];
    snprintf(am, sizeof(am), "A%08x", agg);
    if (send_to(fs_pid, am, 9) != 0) {
        printf("AGENT BROKEN aggregate send_to(%ld)\n", fs_pid);
        return 0;
    }
    printf("AGENT: aggregate a%08x handed to fieldsyncd (qseed-salted)\n", agg);
    return 1;
}

void _start(void) {
    /* Region 3 is imprinted in do_field() BEFORE do_delegate() hands the cap
     * over, so the sub-agent's recall has content. do_qpu/do_spawn yield, giving
     * the sub-agent time to send its buffered "ready". */
    int q = do_qpu();
    int f = do_field();
    int s = do_spawn();
    int d = do_delegate();

    if (q && f && s && d) {
        write_str("AGENTD: DEMO OK qpu+field+spawn+society");
    } else {
        printf("AGENTD: DEMO BROKEN qpu=%d field=%d spawn=%d society=%d\n", q, f, s, d);
    }
    /* One-shot proof: EXIT rather than idle-spin. A TERMINATED process leaves the
     * scheduler's ready queue, so the demo does not keep the idle-loop reaper (and
     * every other citizen's death cleanup) starved by an ever-ready yield loop.
     * Unmonitored, so it is reaped, not restarted; its caps cascade-revoke. */
    exit_(0);
    for (;;) {
        yield();
    }
}
