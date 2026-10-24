/**
 * QuantumOS swarm_svc — the COM2 serial swarm bridge, a fourth isolated
 * ring-3 service (ghostd phase 4, #51; epic #47).
 *
 * Two jobs, both over the capability-guarded COM2 UART (it is the only
 * ring-3 process granted the DEVICE_ID_COM2 device cap, plus a quantum-pool
 * read cap and a single IPC send-cap to ghostd):
 *
 *   1. Boot attestation. At startup swarm_svc draws a 32-byte master seed
 *      from the quantum pool, deterministically expands it into a 256-pair
 *      Lamport key (SHA-256 counter mode), and emits — as CRC8-framed COM2
 *      frames — a public-key-digest commitment, the attestation string
 *      "QOS-BOOT|qseed=<hex|none>|ticks=<n>", and the Lamport signature over
 *      it. A host reading the COM2 stream can verify the signature against the
 *      committed public key and confirm the attested qseed. See swarm.h for
 *      the frame format and the exact Lamport construction.
 *
 *   2. Bridge traffic. It then speaks the length-prefixed frame protocol:
 *      PING is answered with PONG, and a DATA frame is a remote request routed
 *      over capability-checked IPC to ghostd (SYS_SEND_TO) and answered with a
 *      DATA frame carrying ghostd's reply.
 *
 * No floats anywhere; all crypto is integer SHA-256 (sha256.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stddef.h> /* NULL (reply-auth nonce presence flag) */
#include "swarm.h"
#include "sha256.h"
#include "ghost.h"       /* ghost_req_t/ghost_rep_t, GHOST_*, usys.h, string builders */
#include "qpu_circuit.h" /* QC_RESULT_LEN, QC_STATUS_* (epic #149 B1) */

/* The frozen QSUBMIT DONE body (swarm.h, ADR-0020 lane C) must stay derived
 * from the executor's result length — a QC_RESULT_LEN change that forgets the
 * wire contract fails HERE, in the emitter's TU, not silently on the host. */
_Static_assert(SWARM_QSUBMIT_BODY_OK == 2 + QC_RESULT_LEN,
               "QSUBMIT DONE body = op + status + QC result");

/* ---- state (zeroed .bss) ---- */
static uint8_t master_seed[LAMPORT_SEED_LEN];
static uint8_t framebuf[SWARM_HDR_LEN + SWARM_MAX_PAYLOAD + 1];
static long ghostd_pid = 0;

/* The host-admitted swarm-plane session key (ADR-0019). swarm_svc both FORWARDS
 * it to fieldsyncd (the field-coupling wire) and KEEPS a copy here to
 * authenticate COM2 DATA replies (the reply-auth Extension): a keyed STATUS
 * reply carries a nonce echo + HMAC so an agent's tool result is provably fresh,
 * not a replay. The same 32 bytes fieldsyncd stores (one SWARM_OP_KEY channel,
 * two consumers). In memory: a swarm_svc rebirth loses it and the host
 * re-admits; a FIELDSYNCD rebirth is handled HERE (ADR-0023) — the main loop
 * watches for a fieldsyncd pid change and re-forwards the cached key, because
 * ADR-0023's declarative re-mint restores only the DELIVERY PATH: a reborn
 * fieldsyncd with restored ghostd wiring but no key would emit seq=0 zero-tag
 * frames that keyed peers silently reject (a keyless-but-wired partition). */
static uint8_t session_key[SWARM_KEY_LEN];
static int have_key = 0;
/* The fieldsyncd pid the key was last successfully forwarded to (0 = never). */
static long key_fwd_pid = 0;

/* COM2 receive accumulator for the inbound frame parser. */
static uint8_t rxbuf[SWARM_HDR_LEN + SWARM_MAX_PAYLOAD + 1];
static uint32_t rxlen = 0;

/* Outstanding QPU job submitted over the wire (0 = none). The QSUBMIT handler
 * submits and RETURNS; the main loop polls this one job a single step per pass
 * so poll_com2()+heartbeat() keep firing — an in-handler poll-to-completion
 * would starve the 2s watchdog and, past max_restarts, permanently kill the
 * sole COM2 bridge (a remote ~3-frame DoS). We NEVER abandon an owned job:
 * only a DONE poll frees the broker slot, so abandoning would ratchet the
 * per-owner in-flight cap to a permanent EIO. Serialized to one in flight. */
static long qsub_jid = 0;

/* Reply-auth context for the outstanding QSUBMIT (ADR-0019 Extension). The
 * QSUBMIT reply is ASYNC (framed later in qsub_poll_step), so the request's
 * nonce must be STASHED atomically with qsub_jid at accept-time and echoed +
 * MAC'd when the job completes. qsub_authed snapshots have_key at accept (NOT
 * read live at DONE-time) so a key admitted mid-flight cannot desync the reply
 * framing from what the host is awaiting. Scrubbed on completion. */
static uint8_t qsub_nonce[SWARM_REPLYAUTH_NONCE_LEN];
static int qsub_authed = 0;

static void logline(const char *s) {
    write_str(s);
}

/* ---- little formatting helpers (no libc) ---- */
static int put_hex_u64(char *b, int o, uint64_t v) {
    if (v == 0) {
        b[o++] = '0';
        return o;
    }
    char t[16];
    int n = 0;
    while (v) {
        int d = (int)(v & 0xFu);
        t[n++] = (char)(d < 10 ? ('0' + d) : ('A' + d - 10));
        v >>= 4;
    }
    while (n)
        b[o++] = t[--n];
    return o;
}

/* ---- COM2 output: send every byte, chunked under the kernel's per-call cap.
 * SYS_COM2 moves at most COM2_MAX_BYTES (256) per call, so loop until done. */
static void com2_send_all(const uint8_t *buf, long len) {
    long off = 0;
    while (off < len) {
        long chunk = len - off;
        if (chunk > 256)
            chunk = 256;
        long w = com2_write_bytes(buf + off, chunk);
        if (w <= 0)
            return; /* no cap / error — nothing more we can do */
        off += w;
    }
}

/* Build one framed message and push it out COM2. */
static void emit_frame(uint8_t type, const uint8_t *payload, uint32_t len) {
    if (len > SWARM_MAX_PAYLOAD)
        len = SWARM_MAX_PAYLOAD;
    framebuf[0] = SWARM_MAGIC;
    framebuf[1] = type;
    framebuf[2] = (uint8_t)(len & 0xFFu);
    framebuf[3] = (uint8_t)((len >> 8) & 0xFFu);
    for (uint32_t i = 0; i < len; i++)
        framebuf[SWARM_HDR_LEN + i] = payload[i];
    /* crc8 covers type + length + payload (everything but magic and crc) */
    framebuf[SWARM_HDR_LEN + len] = swarm_crc8(&framebuf[1], 3 + len);
    com2_send_all(framebuf, (long)(SWARM_HDR_LEN + len + 1));
}

/* ---- Lamport key material (counter-mode expansion of the master seed) ----
 * sk[i][b] = SHA-256(master_seed ‖ be32(i) ‖ b). */
static void expand_sk(uint32_t i, uint8_t b, uint8_t out[LAMPORT_HASH_LEN]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, master_seed, LAMPORT_SEED_LEN);
    uint8_t ib[5];
    ib[0] = (uint8_t)(i >> 24);
    ib[1] = (uint8_t)(i >> 16);
    ib[2] = (uint8_t)(i >> 8);
    ib[3] = (uint8_t)i;
    ib[4] = b;
    sha256_update(&c, ib, 5);
    sha256_final(&c, out);
}

/* pubkey digest = SHA-256(pk[0][0] ‖ pk[0][1] ‖ … ‖ pk[255][1]),
 * pk[i][b] = SHA-256(sk[i][b]). Streamed so no 16 KB buffer is needed. */
static void compute_pubkey_digest(uint8_t out[LAMPORT_HASH_LEN]) {
    sha256_ctx pk_ctx;
    sha256_init(&pk_ctx);
    for (uint32_t i = 0; i < LAMPORT_BITS; i++) {
        for (uint8_t b = 0; b < 2; b++) {
            uint8_t sk[LAMPORT_HASH_LEN], pk[LAMPORT_HASH_LEN];
            expand_sk(i, b, sk);
            sha256(sk, LAMPORT_HASH_LEN, pk);
            sha256_update(&pk_ctx, pk, LAMPORT_HASH_LEN);
        }
    }
    sha256_final(&pk_ctx, out);
}

/* Sign md and emit the signature as FRAME_SIG chunks. Element i (for message
 * bit b_i) is the revealed preimage sk[i][b_i] followed by the complementary
 * public-key hash pk[i][1-b_i], so the verifier can rebuild the whole public
 * key and match it to the committed digest. */
static void emit_signature(const uint8_t md[LAMPORT_HASH_LEN]) {
    uint8_t chunk[SWARM_MAX_PAYLOAD]; /* 512 = 8 elements of 64 bytes */
    uint32_t cpos = 0;
    for (uint32_t i = 0; i < LAMPORT_BITS; i++) {
        uint8_t bit = (uint8_t)((md[i >> 3] >> (i & 7u)) & 1u);
        uint8_t sk[LAMPORT_HASH_LEN], sko[LAMPORT_HASH_LEN], pko[LAMPORT_HASH_LEN];
        expand_sk(i, bit, sk); /* revealed preimage */
        expand_sk(i, (uint8_t)(1u - bit), sko);
        sha256(sko, LAMPORT_HASH_LEN, pko); /* complementary pk hash */
        for (int j = 0; j < LAMPORT_HASH_LEN; j++)
            chunk[cpos + j] = sk[j];
        for (int j = 0; j < LAMPORT_HASH_LEN; j++)
            chunk[cpos + LAMPORT_HASH_LEN + j] = pko[j];
        cpos += LAMPORT_SIG_ELEM;
        if (cpos == SWARM_MAX_PAYLOAD) {
            emit_frame(FRAME_SIG, chunk, cpos);
            cpos = 0;
        }
    }
    if (cpos > 0)
        emit_frame(FRAME_SIG, chunk, cpos);
}

/* Compose the attestation string into `msg`, returning its length. */
static int build_attestation(char *msg, int seed_present, uint64_t qseed, uint64_t tk) {
    int o = 0;
    o = ghost_put(msg, o, SWARM_ATTEST_HEAD);
    if (seed_present)
        o = put_hex_u64(msg, o, qseed);
    else
        o = ghost_put(msg, o, "none");
    o = ghost_put(msg, o, SWARM_ATTEST_TICKS);
    o = ghost_put_u(msg, o, (unsigned)tk);
    return o;
}

/* Draw the Lamport master seed, sign a boot attestation, and emit it on COM2.
 * The console gate line (grepped by CI) is printed on COM1 afterwards. */
static void emit_boot_attestation(void) {
    /* 32 bytes of true entropy from the quantum pool; the 16 KB key is
     * SHA-256-expanded from it (security = SHA-256 + this 256-bit seed). */
    long got = qrand_fill(master_seed, LAMPORT_SEED_LEN);
    if (got < (long)LAMPORT_SEED_LEN) {
        /* No quantum cap / short draw: fall back to a tick-stirred filler so
         * the key is still well-defined. Honest — the attestation itself
         * records qseed provenance separately below. */
        uint32_t x = (uint32_t)ticks() ^ 0x9E3779B9u;
        for (int i = (got > 0 ? (int)got : 0); i < LAMPORT_SEED_LEN; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            master_seed[i] = (uint8_t)(x & 0xFFu);
        }
    }

    long sp = qrand_seed_present(); /* 1 present, 0 absent, <0 no cap */
    long qs = qseed_value();        /* seed value, or <0 no cap */
    int seed_present = (sp == 1);
    uint64_t qseed = seed_present ? (uint64_t)qs : 0;
    uint64_t tk = (uint64_t)ticks();

    char msg[96];
    int msglen = build_attestation(msg, seed_present, qseed, tk);

    /* HANDSHAKE opens the stream (protocol id), then the attestation trio. */
    emit_frame(FRAME_HANDSHAKE, (const uint8_t *)"QOS-SWARM/1", 11);

    uint8_t pkdigest[LAMPORT_HASH_LEN];
    compute_pubkey_digest(pkdigest);
    emit_frame(FRAME_PKDIGEST, pkdigest, LAMPORT_HASH_LEN);

    emit_frame(FRAME_ATTEST, (const uint8_t *)msg, (uint32_t)msglen);

    uint8_t md[LAMPORT_HASH_LEN];
    sha256((const uint8_t *)msg, (uint32_t)msglen, md);
    emit_signature(md);

    /* COM1 console gate line (CI greps "SWARM: boot attestation emitted"). */
    char line[96];
    int o = ghost_put(line, 0, "SWARM: boot attestation emitted (lamport-signed, seed=");
    if (seed_present)
        o = put_hex_u64(line, o, qseed);
    else
        o = ghost_put(line, o, "none");
    o = ghost_put(line, o, ")");
    line[o] = 0;
    logline(line);
}

/* ---- ghostd routing (learned pid + SYS_SEND_TO) ---- */

/* One-time discovery: send a STATUS via the single ghostd send-cap (first
 * match is unambiguous) and record the replying pid for targeted sends. */
static int discover_ghostd(void) {
    ghost_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++)
        ((uint8_t *)&req)[i] = 0;
    req.op = GHOST_STATUS;
    if (send_msg((const char *)&req, (long)sizeof(req)) != 0)
        return 0;

    char buf[sizeof(ghost_rep_t) + 8];
    for (long spins = 0; spins < 4000000; spins++) {
        long snd = recv_msg(buf, sizeof(buf));
        if (snd != 0) {
            ghostd_pid = snd;
            return 1;
        }
        yield();
    }
    return 0;
}

/* Forward a request to ghostd (targeted send) and block, bounded, for the
 * reply. Returns 1 on success. */
static int ghost_query(const ghost_req_t *req, ghost_rep_t *rep) {
    if (ghostd_pid == 0)
        return 0;
    if (send_to(ghostd_pid, (const char *)req, (long)sizeof(*req)) != 0)
        return 0;
    char buf[sizeof(ghost_rep_t) + 8];
    for (long spins = 0; spins < 2000000; spins++) {
        long snd = recv_msg(buf, sizeof(buf));
        if (snd != 0) {
            for (unsigned i = 0; i < sizeof(*rep); i++)
                ((uint8_t *)rep)[i] = (uint8_t)buf[i];
            return 1;
        }
        yield();
    }
    return 0;
}

/* Emit a COM2 reply, optionally authenticated (ADR-0019 Extension). ONE emit
 * path for every op (STATUS + QSUBMIT): when `nonce` is non-NULL (keyed request)
 * append the 16-byte nonce echo + HMAC-SHA256(key, op || nonce || body[1..]) so
 * the host proves the reply FRESH (echo) and UNFORGED (tag); a NULL nonce emits
 * the legacy plain body, byte-for-byte identical to the pre-auth wire (the
 * unkeyed ci-smoke path). body[0] is the op; body_len is the op's reply body
 * size (6 for STATUS, 2 or 2+QC_RESULT_LEN for QSUBMIT), so the tag covers the
 * exact body bytes and a truncation is caught by the host's length + MAC checks.
 * The out/macin buffers are sized for the largest body (QSUBMIT DONE). */
static void emit_authed_reply(const uint8_t *body, uint32_t body_len, const uint8_t *nonce) {
    if (!nonce) {
        emit_frame(FRAME_DATA, body, body_len);
        return;
    }
    /* max body(22) + nonce(16) + tag(32) */
    uint8_t out[SWARM_QSUBMIT_BODY_OK + SWARM_REPLYAUTH_NONCE_LEN + SWARM_REPLYAUTH_TAG_LEN];
    for (uint32_t i = 0; i < body_len; i++) {
        out[i] = body[i];
    }
    for (int i = 0; i < SWARM_REPLYAUTH_NONCE_LEN; i++) {
        out[body_len + i] = nonce[i]; /* nonce echo */
    }
    /* MAC input: op(1) || nonce(16) || body[1..] — identical form to STATUS. */
    uint8_t macin[1 + SWARM_REPLYAUTH_NONCE_LEN + 1 + QC_RESULT_LEN];
    macin[0] = body[0];
    for (int i = 0; i < SWARM_REPLYAUTH_NONCE_LEN; i++) {
        macin[1 + i] = nonce[i];
    }
    for (uint32_t i = 1; i < body_len; i++) {
        macin[1 + SWARM_REPLYAUTH_NONCE_LEN + (i - 1)] = body[i];
    }
    hmac_sha256(session_key, SWARM_KEY_LEN, macin, 1 + SWARM_REPLYAUTH_NONCE_LEN + (body_len - 1),
                &out[body_len + SWARM_REPLYAUTH_NONCE_LEN]);
    emit_frame(FRAME_DATA, out, body_len + SWARM_REPLYAUTH_NONCE_LEN + SWARM_REPLYAUTH_TAG_LEN);
}

/* ---- session-key delivery to fieldsyncd (ADR-0019 + ADR-0023) ---- */

/* fieldsyncd's current pid from the uncapped SYSINFO_PS text (the agentd/qtop
 * pattern): scan for a "PS: <pid> fieldsy..." row. 0 = not present. */
static long find_fieldsyncd_pid(void) {
    static char ps[2048];
    long pn = sysinfo(SYSINFO_PS, ps, sizeof(ps) - 1);
    ps[(pn > 0) ? pn : 0] = '\0';
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
                return p;
            }
        }
    }
    return 0;
}

/* Send the cached session key to fieldsyncd as "K"+32 bytes over the declared
 * swarm_svc->fieldsyncd IPC cap. Records the delivered-to pid on success so
 * the main-loop watch can detect a fieldsyncd REBIRTH (new pid) and re-send. */
static int forward_key_to(long fs_pid) {
    char km[1 + SWARM_KEY_LEN];
    km[0] = 'K';
    for (int i = 0; i < SWARM_KEY_LEN; i++) {
        km[i + 1] = (char)session_key[i];
    }
    if (send_to(fs_pid, km, 1 + SWARM_KEY_LEN) == 0) {
        key_fwd_pid = fs_pid;
        return 1;
    }
    return 0;
}

/* ADR-0023: a fieldsyncd watchdog rebirth restores the swarm->fieldsyncd
 * DELIVERY PATH (the declarative re-mint) but not the key — the reborn
 * fieldsyncd would be keyless-but-wired, emitting zero-tag frames its keyed
 * peers silently reject. swarm_svc is the only citizen that caches the key
 * AND can observe the pid change, so it re-forwards. Paced to ~every 2 s
 * (the PS scan is a syscall + 2 KB text walk — not per-pass work). */
static void key_reforward_step(void) {
    static uint64_t last_check;
    if (!have_key) {
        return;
    }
    uint64_t now = (uint64_t)ticks();
    if (now - last_check < 200) { /* 10 ms ticks -> ~2 s */
        return;
    }
    last_check = now;
    long fs_pid = find_fieldsyncd_pid();
    if (fs_pid != 0 && fs_pid != key_fwd_pid) {
        if (forward_key_to(fs_pid)) {
            logline("SWARM: swarm-plane key re-forwarded to reborn fieldsyncd");
        }
        /* On failure: the declared cap may not be re-minted yet (fieldsyncd
         * mid-restart) — the next 2 s pass retries; no log spam. */
    }
}

/* ---- inbound frame dispatch ---- */

static void handle_data(const uint8_t *payload, uint32_t len) {
    if (len < 1)
        return;
    uint8_t op = payload[0];

    ghost_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++)
        ((uint8_t *)&req)[i] = 0;
    ghost_rep_t rep;

    if (op == SWARM_OP_STATUS) {
        req.op = GHOST_STATUS;
        if (!ghost_query(&req, &rep))
            return;
        /* reply body (leading, so legacy 6-byte parsers still work): op,
         * r_q16 (LE u32), live (u8). */
        uint8_t body[SWARM_STATUS_BODY_LEN];
        body[0] = SWARM_OP_STATUS;
        body[1] = (uint8_t)(rep.r_q16);
        body[2] = (uint8_t)(rep.r_q16 >> 8);
        body[3] = (uint8_t)(rep.r_q16 >> 16);
        body[4] = (uint8_t)(rep.r_q16 >> 24);
        body[5] = rep.live;
        /* Authenticated when keyed AND the request carried a 16-byte host nonce
         * (OPTIONAL for STATUS: a plain 1-byte request keeps the legacy 6-byte
         * reply, so an unkeyed host / ci-smoke-mcp is unaffected). Same emit path
         * as QSUBMIT — one audited nonce-echo + HMAC(op||nonce||body[1..]). */
        const uint8_t *nonce =
            (have_key && len >= 1 + SWARM_REPLYAUTH_NONCE_LEN) ? &payload[1] : NULL;
        emit_authed_reply(body, sizeof(body), nonce);
    } else if (op == SWARM_OP_RECALL) {
        if (len < 1 + GHOST_PW * 4)
            return;
        req.op = GHOST_RECALL;
        for (int w = 0; w < GHOST_PW; w++) {
            req.bits[w] = (uint32_t)payload[1 + w * 4] | ((uint32_t)payload[1 + w * 4 + 1] << 8) |
                          ((uint32_t)payload[1 + w * 4 + 2] << 16) |
                          ((uint32_t)payload[1 + w * 4 + 3] << 24);
        }
        if (!ghost_query(&req, &rep))
            return;
        /* reply: op, match (s8), r_q16 (LE u32) */
        uint8_t out[SWARM_RECALL_BODY_LEN];
        out[0] = SWARM_OP_RECALL;
        out[1] = (uint8_t)rep.match;
        out[2] = (uint8_t)(rep.r_q16);
        out[3] = (uint8_t)(rep.r_q16 >> 8);
        out[4] = (uint8_t)(rep.r_q16 >> 16);
        out[5] = (uint8_t)(rep.r_q16 >> 24);
        emit_frame(FRAME_DATA, out, sizeof(out));
    } else if (op == SWARM_OP_QSUBMIT) {
        /* Submit-ONLY (non-blocking): copy the opaque circuit into the broker
         * and stash the job id. The main loop polls it a step per pass and
         * frames the reply on DONE — NEVER poll here (heartbeat starvation).
         *
         * Reply-auth (ADR-0019 Extension): ENFORCE-WHEN-KEYED. The circuit is
         * VARIABLE-length so an optional trailing nonce is unframeable; instead
         * a keyed guest ALWAYS expects op|nonce(16)|circuit and authenticates
         * every reply (both a synchronous error AND the async DONE), while an
         * unkeyed guest keeps the legacy op|circuit wire byte-for-byte. The
         * host's key-aware qsubmit() is the SOLE emitter, so the two sides never
         * disagree on whether a nonce is present. */
        const uint8_t *nonce = NULL;
        const uint8_t *circuit;
        uint32_t clen;
        if (have_key) {
            /* Too short to carry a nonce -> we could not authenticate any reply,
             * so drop SILENTLY (no OOB read, no plaintext downgrade oracle); the
             * keyed host fail-closes on the transaction timeout. */
            if (len < 1 + SWARM_REPLYAUTH_NONCE_LEN) {
                return;
            }
            nonce = &payload[1];
            circuit = &payload[1 + SWARM_REPLYAUTH_NONCE_LEN];
            clen = len - 1 - SWARM_REPLYAUTH_NONCE_LEN;
        } else {
            circuit = &payload[1];
            clen = len - 1;
        }
        if (clen == 0 || clen > QPU_CIRCUIT_MAX) {
            /* malformed → broker/EINVAL */
            uint8_t err[SWARM_QSUBMIT_BODY_ERR] = {SWARM_OP_QSUBMIT, 2};
            emit_authed_reply(err, sizeof(err), nonce);
            return;
        }
        if (qsub_jid != 0) {
            /* A job is already outstanding (unreachable under a synchronous
             * host that waits for each reply; defensive). Report refused with
             * the IN-HAND nonce — a synchronous error NEVER touches the stash. */
            uint8_t busy[SWARM_QSUBMIT_BODY_ERR] = {SWARM_OP_QSUBMIT, 1};
            emit_authed_reply(busy, sizeof(busy), nonce);
            return;
        }
        qpu_submit_req_t sreq;
        sreq.circuit_len = clen;
        for (uint32_t i = 0; i < clen; i++) {
            sreq.circuit[i] = circuit[i];
        }
        long r = qpu_submit_(&sreq);
        if (r >= 1) {
            qsub_jid = r; /* reply deferred to the main-loop poll */
            /* Stash this job's auth context ATOMICALLY with qsub_jid so the
             * async DONE echoes THIS request's nonce, keyed off the accept-time
             * snapshot (not live have_key). */
            qsub_authed = (nonce != NULL);
            if (nonce) {
                for (int i = 0; i < SWARM_REPLYAUTH_NONCE_LEN; i++) {
                    qsub_nonce[i] = nonce[i];
                }
            }
        } else {
            /* -4 EPERM/quota → refused(1); other broker errors → 2. Synchronous
             * error → in-hand nonce, no stash write. */
            uint8_t err[SWARM_QSUBMIT_BODY_ERR] = {SWARM_OP_QSUBMIT, (uint8_t)(r == -4 ? 1 : 2)};
            emit_authed_reply(err, sizeof(err), nonce);
        }
    } else if (op == SWARM_OP_KEY) {
        /* Host admits the swarm-plane group session key (ADR-0019): forward it
         * to fieldsyncd over the declared swarm_svc->fieldsyncd IPC send-cap
         * (ipc_peers, ADR-0023) so the field-coupling wire can be
         * HMAC-authenticated. fieldsyncd's pid comes from the uncapped
         * SYSINFO_PS text (the agentd/qtop pattern). No reply — the host does
         * not depend on a guest ack for the key. */
        if (len < 1 + SWARM_KEY_LEN) {
            return;
        }
        /* Store the key for reply-auth FIRST — independent of the fieldsyncd
         * forward below, so a lone MCP VM (or one whose fieldsyncd hasn't yet
         * appeared in SYSINFO_PS) can still authenticate its COM2 replies. */
        for (int i = 0; i < SWARM_KEY_LEN; i++) {
            session_key[i] = payload[1 + i];
        }
        have_key = 1;
        long fs_pid = find_fieldsyncd_pid();
        if (fs_pid == 0) {
            logline("SWARM: key admit failed — fieldsyncd not in PS");
            return; /* the main-loop watch (ADR-0023) retries once it appears */
        }
        if (forward_key_to(fs_pid)) {
            logline("SWARM: swarm-plane key forwarded to fieldsyncd");
        } else {
            logline("SWARM: key forward to fieldsyncd FAILED (no IPC cap?)");
        }
    }
}

/* Poll the one outstanding wire-submitted QPU job a SINGLE step. Called once
 * per main-loop pass so heartbeat() keeps firing; on DONE it frames the reply
 * and clears qsub_jid. Never abandons the job (only DONE frees the slot). */
static void qsub_poll_step(void) {
    if (qsub_jid == 0) {
        return;
    }
    qpu_poll_out_t out;
    long st = qpu_poll_(qsub_jid, &out);
    if (st == QPU_POLL_PENDING || st == QPU_POLL_RUNNING) {
        return; /* keep waiting — next pass */
    }
    /* Authenticate the async reply with the STASHED accept-time context (this
     * job's nonce), never live have_key — a mid-flight key admit must not change
     * the framing of a reply the host is already awaiting. COM2 RX is polled in
     * this same single-threaded loop (no ISR), so the pointer stays valid until
     * the scrub below; a future move to interrupt-driven RX re-triggers review. */
    const uint8_t *nonce = qsub_authed ? qsub_nonce : NULL;
    uint8_t reply[SWARM_QSUBMIT_BODY_OK];
    reply[0] = SWARM_OP_QSUBMIT;
    if (st == QPU_POLL_DONE && out.status == QPU_STATUS_OK && out.result_len == QC_RESULT_LEN) {
        /* status keys on the EXECUTOR's circuit-level result[0], not just the
         * broker's slot status — a malformed circuit is QC_STATUS_EINVAL. */
        reply[1] = (out.result[0] == QC_STATUS_OK) ? 0 : 2;
        for (uint32_t i = 0; i < QC_RESULT_LEN; i++) {
            reply[2 + i] = out.result[i];
        }
        emit_authed_reply(reply, sizeof(reply), nonce);
    } else {
        /* EXECFAIL, a short/oversized result, or a poll error → broker error. */
        reply[1] = 2;
        emit_authed_reply(reply, SWARM_QSUBMIT_BODY_ERR, nonce);
    }
    /* Clear-on-completion: free the broker slot AND scrub the auth context so no
     * later path can emit with a stale nonce (a replayed DONE re-poll is already
     * blocked by qsub_jid==0, this is defense-in-depth). */
    qsub_jid = 0;
    qsub_authed = 0;
    for (int i = 0; i < SWARM_REPLYAUTH_NONCE_LEN; i++) {
        qsub_nonce[i] = 0;
    }
}

static void dispatch_frame(uint8_t type, const uint8_t *payload, uint32_t len) {
    switch (type) {
    case FRAME_PING:
        emit_frame(FRAME_PONG, payload, len); /* echo payload back as PONG */
        break;
    case FRAME_DATA:
        handle_data(payload, len);
        break;
    case FRAME_DISCONNECT:
    case FRAME_HANDSHAKE:
    default:
        break; /* nothing to do */
    }
}

/* Pull whatever COM2 has, append to the accumulator, and parse out every
 * complete, CRC-valid frame. Resynchronises byte-by-byte on the magic. */
static void poll_com2(void) {
    uint8_t in[128];
    long got = com2_read_bytes(in, (long)sizeof(in));
    for (long i = 0; i < got; i++) {
        if (rxlen < sizeof(rxbuf))
            rxbuf[rxlen++] = in[i];
    }

    for (;;) {
        /* resync: drop leading bytes until the buffer starts with the magic */
        uint32_t start = 0;
        while (start < rxlen && rxbuf[start] != SWARM_MAGIC)
            start++;
        if (start > 0) {
            for (uint32_t k = start; k < rxlen; k++)
                rxbuf[k - start] = rxbuf[k];
            rxlen -= start;
        }
        if (rxlen < SWARM_HDR_LEN)
            return; /* need a full header */

        uint32_t len = (uint32_t)rxbuf[2] | ((uint32_t)rxbuf[3] << 8);
        if (len > SWARM_MAX_PAYLOAD) { /* bogus length: drop the magic, resync */
            for (uint32_t k = 1; k < rxlen; k++)
                rxbuf[k - 1] = rxbuf[k];
            rxlen -= 1;
            continue;
        }
        uint32_t total = SWARM_HDR_LEN + len + 1;
        if (rxlen < total)
            return; /* frame not fully arrived yet */

        uint8_t crc = swarm_crc8(&rxbuf[1], 3 + len);
        if (crc == rxbuf[SWARM_HDR_LEN + len]) {
            dispatch_frame(rxbuf[1], &rxbuf[SWARM_HDR_LEN], len);
        }
        /* consume the frame (valid or crc-bad — either way it is complete) */
        for (uint32_t k = total; k < rxlen; k++)
            rxbuf[k - total] = rxbuf[k];
        rxlen -= total;
    }
}

/* HMAC-SHA256 self-test (ADR-0019): prove the integer HMAC in sha256.h matches
 * the standard against RFC 4231 test case 2, so the swarm-plane frame MAC will
 * agree byte-for-byte with the host verifier (python hmac.new). A broken HMAC
 * drops the OK marker the gate greps (revert-and-confirm). */
static void hmac_selftest(void) {
    static const uint8_t key[4] = {'J', 'e', 'f', 'e'};
    static const char msg[] = "what do ya want for nothing?"; /* 28 bytes */
    static const uint8_t expect[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
                                       0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
                                       0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
                                       0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
    uint8_t out[32];
    hmac_sha256(key, 4, (const uint8_t *)msg, 28, out);
    if (hmac_sha256_equal(out, expect)) {
        logline("HMAC256: self-test OK (RFC4231 tc2) — swarm-plane MAC ready");
    } else {
        logline("HMAC256: SELF-TEST FAILED");
    }
}

void _start(void) {
    long restarts = svc_restarts();
    if (restarts > 0) {
        logline("SWARM: bridge REBORN — re-attesting after watchdog restart");
    } else {
        logline("SWARM: bridge online in ring 3 — COM2 serial swarm bridge");
    }

    /* Verify the HMAC primitive the ADR-0019 swarm-plane auth is built on. */
    hmac_selftest();

    /* Job 1: the Lamport-signed boot attestation, out COM2 + console gate. */
    emit_boot_attestation();

    /* Learn ghostd's pid for DATA routing (best-effort; PING still works). */
    discover_ghostd();

    /* Job 2: serve the bridge protocol. poll_com2() accepts frames (a QSUBMIT
     * submits and returns); qsub_poll_step() advances the one outstanding QPU
     * job a single step; key_reforward_step() re-delivers the session key if
     * fieldsyncd was reborn (ADR-0023, ~2 s pacing); heartbeat() fires EVERY
     * pass so a slow circuit can never starve the watchdog. */
    while (1) {
        poll_com2();
        qsub_poll_step();
        key_reforward_step();
        heartbeat();
        yield();
    }
}
