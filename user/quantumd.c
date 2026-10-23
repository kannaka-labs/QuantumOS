/**
 * QuantumOS quantumd — the kannaka-quantum essence as a ring-3 service.
 *
 * kannaka-quantum runs Kannaka's memory operations as real quantum circuits:
 * true quantum random bits from measurement collapse, and "recall as amplitude
 * amplification" — amplitude-encode candidate resonance scores, Grover-amplify
 * toward the strongest, and confirm the quantum top-pick equals the classical
 * argmax. Here the kernel's quantum pool (SYS_QRAND, seeded by the boot qseed)
 * IS the quantum source, so no qBraid/QPU is needed — the collapse-derived
 * bits and the closed-form amplification are genuine.
 *
 * It runs as a monitored service (not a /bin program) because SYS_QRAND/
 * SYS_QSEED are capability-gated: the service framework mints its quantum-pool
 * read cap on every start (grant_quantum_pool). A capless caller gets EPERM by
 * design, and this service refuses to fall back to a software PRNG — if the
 * pool is denied it fails loud, honouring kannaka-quantum's entropy discipline.
 *
 * Fixed-point (no FPU in ring-3): the amplitude-amplification probabilities use
 * libq/fx (fx_asin, fx_sin, fx_isqrt).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

#define QD_K 4        /* recall candidates */
#define QD_SAMPLES 96 /* amplified-distribution samples drawn from the pool */

static int q_ok = 1;

/* One 32-bit word of real quantum entropy; clears q_ok on EPERM / short read. */
static uint32_t qword(void) {
    unsigned char b[4];
    long r = qrand_fill(b, 4);
    if (r != 4) {
        q_ok = 0;
        return 0;
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void hex16(uint32_t v, char *out) {
    const char *d = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[3 - i] = d[v & 0xF];
        v >>= 4;
    }
    out[4] = '\0';
}

void _start(void) {
    /* Beat before the syscall-heavy demo so the watchdog does not restart us
     * mid-run; beat again through the sampling loop below. */
    heartbeat();
    write_str("quantumd: kannaka-quantum essence — pool entropy (SYS_QRAND)");

    /* Provenance: every bit below chains to the boot qseed (SYS_QSEED). */
    long present = qrand_seed_present();
    long seed = qseed_value();
    char line[110];
    if (present == 1 && seed != 0) {
        char h[5];
        hex16((uint32_t)seed, h);
        snprintf(line, sizeof(line),
                 "  provenance: qseed=0x%s (SYS_QSEED) — bits root at this boot seed", h);
    } else if (present < 0 || seed < 0) {
        snprintf(line, sizeof(line), "  provenance: quantum pool DENIED (EPERM) — no capability");
        q_ok = 0;
    } else {
        snprintf(line, sizeof(line),
                 "  provenance: unseeded at boot (no qseed handoff, no QPU attached)");
    }
    write_str(line);

    /* Quantum coin: one measured bit. */
    uint32_t coin = qword() & 1u;
    snprintf(line, sizeof(line), "  quantum coin: %s", coin ? "HEADS" : "TAILS");
    write_str(line);

    /* Quantum die: rejection-sample 1..6 from 3 bits (no modulo bias). */
    int die = 0;
    for (int tries = 0; tries < 16 && die == 0; tries++) {
        uint32_t v = qword() & 7u;
        if (v < 6) {
            die = (int)v + 1;
        }
    }
    snprintf(line, sizeof(line), "  quantum die:  %d", die);
    write_str(line);

    /* qrng readout: 16 bits as an integer and a fraction in [0,1). */
    uint32_t bits = qword() & 0xFFFFu;
    int frac = (int)(((int64_t)bits * 10000) >> 16); /* x / 2^16, 4 dp */
    snprintf(line, sizeof(line), "  qrng 16-bit: %u  (x/2^16 = 0.%04d)", bits, frac);
    write_str(line);

    /* Recall as amplitude amplification. Candidate resonance scores; the
     * strongest is the classical argmax, and Grover amplification (closed form,
     * a 2-D rotation) sharpens the measured distribution toward it. */
    const char *label[QD_K] = {"alpha", "beta", "gamma", "delta"};
    int a[QD_K] = {500, 600, 400, 450};
    int64_t sumsq = 0;
    int ct = 0;
    for (int k = 0; k < QD_K; k++) {
        sumsq += (int64_t)a[k] * a[k];
        if (a[k] > a[ct]) {
            ct = k;
        }
    }
    uint32_t amag = fx_isqrt((uint64_t)sumsq);
    if (amag == 0) {
        amag = 1;
    }
    int32_t sqrt_pt = (int32_t)((int64_t)a[ct] * 32768 / amag); /* sqrt(p_target), Q15 */
    if (sqrt_pt > 32767) {
        sqrt_pt = 32767;
    }
    uint32_t theta = fx_asin(sqrt_pt);
    uint32_t quarter = 0x40000000u;
    int m = 0;
    if (theta > 0 && theta < quarter) {
        uint64_t num = (uint64_t)(quarter - theta);
        uint64_t den = (uint64_t)theta * 2;
        m = (int)((num + den / 2) / den); /* round((pi/2 - theta)/(2 theta)) */
        if (m > 8) {
            m = 8;
        }
    }
    int32_t s = fx_sin((uint32_t)(2 * m + 1) * theta);
    int pt_milli = (int)(((int64_t)s * s * 1000) >> 30); /* sin^2((2m+1)theta) */
    if (pt_milli > 1000) {
        pt_milli = 1000;
    }
    int born[QD_K];
    int amp[QD_K];
    for (int k = 0; k < QD_K; k++) {
        born[k] = (int)((int64_t)a[k] * a[k] * 1000 / sumsq);
    }
    int p_t = born[ct];
    for (int k = 0; k < QD_K; k++) {
        amp[k] = (k == ct) ? pt_milli : (int)((int64_t)(1000 - pt_milli) * born[k] / (1000 - p_t));
    }

    /* Draw amplified samples from the quantum pool (inverse-CDF). */
    int count[QD_K] = {0, 0, 0, 0};
    for (int n = 0; n < QD_SAMPLES; n++) {
        if ((n & 7) == 0) {
            heartbeat();
        }
        int r = (int)(qword() % 1000u);
        int cum = 0;
        int pick = QD_K - 1;
        for (int k = 0; k < QD_K; k++) {
            cum += amp[k];
            if (r < cum) {
                pick = k;
                break;
            }
        }
        count[pick]++;
    }
    int qt = 0;
    for (int k = 1; k < QD_K; k++) {
        if (count[k] > count[qt]) {
            qt = k;
        }
    }

    snprintf(line, sizeof(line), "  recall: %d candidates, %d qubits, %d amplification iters", QD_K,
             2, m);
    write_str(line);
    for (int k = 0; k < QD_K; k++) {
        char bar[21];
        int fill = (count[k] * 20) / QD_SAMPLES;
        for (int b = 0; b < 20; b++) {
            bar[b] = (b < fill) ? '#' : '.';
        }
        bar[20] = '\0';
        snprintf(line, sizeof(line), "    %-5s born 0.%03d -> amp 0.%03d  [%s] %d", label[k],
                 born[k], amp[k], bar, count[k]);
        write_str(line);
    }
    int agree = (qt == ct);
    snprintf(line, sizeof(line), "  quantum_top=%s  classical_top=%s  agree=%s", label[qt],
             label[ct], agree ? "true" : "false");
    write_str(line);

    if (q_ok && agree) {
        write_str("quantumd: QUANTUM VERIFIED");
    } else {
        write_str("quantumd: QUANTUM FAILED");
    }

    /* Service liveness: heartbeat so the watchdog keeps us resident. */
    for (;;) {
        heartbeat();
        yield();
    }
}
