/**
 * QuantumOS qpud — the QPU executor service (epic #148, quantum-stack A2).
 *
 * The RING-3 half of the QPU broker: the kernel (SYS_QPU) enforces authority,
 * quota, and job-slot lifecycle but never parses a circuit; qpud is the sole
 * holder of the QPU EXECUTE cap, so it is the only process that may FETCH a
 * queued job and COMPLETE it. It parses the opaque circuit (qpu_circuit.h),
 * runs it on the EXACT integer engine shared with the qsv boot proof
 * (qsv_engine.h — zero rounding), and returns a bounded result (a probe
 * amplitude + its exact rational probability). Later backends (Phase 2/3)
 * are dispatched from HERE to the host gateway; the kernel surface is unchanged.
 *
 * Authority: grant_qpu_execute mints CAP_READ|CAP_EXECUTE only — NEVER
 * CAP_WRITE. qpud proves at startup that it CANNOT submit (the cross-perm
 * partition), then serves.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "usys.h"
#include "qsv_engine.h"
#include "qpu_circuit.h"

static void put_u16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void put_u32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Parse + execute an opaque circuit into a QC_RESULT_LEN result. Fails CLOSED
 * to QC_STATUS_EINVAL on anything malformed — a hostile/garbage circuit can
 * crash nothing and produces an honest error result, never a wrong answer. */
static void run_circuit(const unsigned char *c, unsigned int len, unsigned char *out) {
    for (int i = 0; i < QC_RESULT_LEN; i++) {
        out[i] = 0;
    }
    if (len < QC_HDR || c[0] != QC_VERSION) {
        out[0] = QC_STATUS_EINVAL;
        return;
    }
    int n = c[1];
    int n_ops = c[2];
    int probe = (int)c[3] | ((int)c[4] << 8);
    if (n < 1 || n > QSV_MAX_QUBITS || probe >= (1 << n) ||
        (unsigned int)(QC_HDR + n_ops * 3) > len) {
        out[0] = QC_STATUS_EINVAL;
        return;
    }
    qsv_reset(n);
    for (int i = 0; i < n_ops; i++) {
        const unsigned char *op = c + QC_HDR + i * 3;
        int a = op[1], b = op[2];
        switch (op[0]) {
        case QC_OP_H:
            if (a >= n)
                goto einval;
            qsv_gate_h(n, a);
            break;
        case QC_OP_X:
            if (a >= n)
                goto einval;
            qsv_gate_x(n, a);
            break;
        case QC_OP_Z:
            if (a >= n)
                goto einval;
            qsv_gate_z(n, a);
            break;
        case QC_OP_S:
            if (a >= n)
                goto einval;
            qsv_gate_s(n, a);
            break;
        case QC_OP_CNOT:
            if (a >= n || b >= n || a == b)
                goto einval;
            qsv_gate_cnot(n, a, b);
            break;
        case QC_OP_CZ:
            if (a >= n || b >= n || a == b)
                goto einval;
            qsv_gate_cz(n, a, b);
            break;
        case QC_OP_ORACLE: {
            int target = a | (b << 8);
            if (target >= (1 << n))
                goto einval;
            qsv_oracle(n, target);
            break;
        }
        case QC_OP_DIFFUSION:
            qsv_diffusion(n);
            break;
        default:
            goto einval;
        }
    }
    /* An amplitude overflowed int32 during an H butterfly (a deep host circuit):
     * the state is corrupt, reject rather than report a wrapped value. Checked
     * BEFORE qsv_norm_ok, which runs after the wrap and would miss it. */
    if (qsv_overflow) {
        goto einval;
    }
    /* Unitarity must hold exactly (integer identity) or the result is not
     * trustworthy — reject rather than report a non-physical amplitude. */
    if (!qsv_norm_ok(n)) {
        goto einval;
    }
    {
        int amp_re = qsv_re[probe];
        int amp_im = qsv_im[probe];
        unsigned long long mag2 =
            (unsigned long long)((long long)amp_re * amp_re + (long long)amp_im * amp_im);
        /* A legal ZERO-probability probe (the amplitude at probe is 0) reduces
         * to 0/1 — report it directly, before the hh>31 guard which would
         * otherwise falsely reject a p=0 result on a circuit with h>31. */
        if (mag2 == 0) {
            out[0] = QC_STATUS_OK;
            out[1] = (unsigned char)n;
            put_u16(out + 2, qsv_h);
            put_u32(out + 4, 0); /* num = 0 */
            put_u32(out + 8, 1); /* den = 1 -> p = 0 */
            put_u32(out + 12, (unsigned int)amp_re);
            put_u32(out + 16, (unsigned int)amp_im);
            return;
        }
        unsigned int hh = qsv_h;
        while ((mag2 & 1) == 0 && hh > 0) {
            mag2 >>= 1;
            hh--;
        }
        if (hh > 31) {
            goto einval; /* reduced denominator would not fit u32 */
        }
        out[0] = QC_STATUS_OK;
        out[1] = (unsigned char)n;
        put_u16(out + 2, qsv_h);
        put_u32(out + 4, (unsigned int)mag2);
        put_u32(out + 8, (unsigned int)1u << hh);
        put_u32(out + 12, (unsigned int)amp_re);
        put_u32(out + 16, (unsigned int)amp_im);
    }
    return;
einval:
    for (int i = 0; i < QC_RESULT_LEN; i++) {
        out[i] = 0;
    }
    out[0] = QC_STATUS_EINVAL;
}

void _start(void) {
    /* Cross-perm partition proof (once per boot, not on watchdog rebirth): the
     * executor holds EXECUTE, not WRITE, so a SUBMIT must be refused EPERM. */
    if (svc_restarts() == 0) {
        qpu_submit_req_t sreq;
        sreq.circuit_len = 4;
        sreq.circuit[0] = QC_VERSION;
        sreq.circuit[1] = 1; /* 1 qubit */
        sreq.circuit[2] = 0; /* 0 ops */
        sreq.circuit[3] = 0; /* probe 0 */
        long r = qpu_submit_(&sreq);
        write_str("QPUD: backend " QPU_BACKEND_LABEL);
        if (r == -4) {
            write_str("QPU: executor submit denied (EPERM)");
        } else {
            write_str("QPU: WARNING - executor submit was NOT denied");
        }
    }

    for (;;) {
        qpu_fetch_out_t job;
        long jid = qpu_fetch_(&job);
        if (jid <= 0) {
            heartbeat();
            yield();
            continue;
        }
        unsigned char result[QC_RESULT_LEN];
        run_circuit(job.circuit, job.circuit_len, result);

        qpu_complete_req_t crep;
        crep.job_id = job.job_id;
        crep.status = QPU_STATUS_OK; /* execution ran; circuit validity is in result[0] */
        crep.result_len = QC_RESULT_LEN;
        for (int i = 0; i < QC_RESULT_LEN; i++) {
            crep.result[i] = result[i];
        }
        qpu_complete_(&crep);

        /* Provenance line: the owner pid is the kernel's, not something qpud
         * can forge — the CI gate cross-references this against the submitter's
         * "job=<id>" line so a brokered result cannot be faked by either side
         * alone. */
        printf("QPUD: executed job=%u owner=%u\n", (unsigned)job.job_id, (unsigned)job.owner_pid);
        heartbeat();
    }
}
