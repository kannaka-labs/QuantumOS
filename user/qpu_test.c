/**
 * QuantumOS qpu_test — the QPU broker proof citizen (epic #148, A2+A3).
 *
 * Holds the QPU SUBMIT cap (grant_qpu_submit) with a manifest qsub quota of 2.
 * Proves the broker end to end, un-echoably: it submits OPAQUE circuits, polls
 * the kernel for the result, and checks the EXACT integer values the executor
 * (qpud) computed — qpu_test does NOT include the engine, so a printed
 * probability can only have come back through the broker from qpud. Then it
 * proves the quota (a third submit is refused) and the cross-perm partition (a
 * submitter cannot COMPLETE). Any failure prints a "QPU BROKEN" line the CI
 * gate greps for; on success it idles forever (a submitted-but-served proof,
 * like quota_test).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "usys.h"
#include "qpu_circuit.h"

static unsigned int get_u16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static unsigned int get_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

/* Submit a circuit and poll it to completion. Returns the job id (>0) with the
 * DONE result in *out, or <=0 on a submit/poll failure. */
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
    /* Bounded poll: qpud must be scheduled to FETCH, run, and COMPLETE. */
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
    return -100; /* timed out */
}

void _start(void) {
    int broken = 0;
    qpu_poll_out_t out;

    /* --- Bell via the broker: p(00) = 1/2 exactly --- */
    unsigned char bell[QC_HDR + 2 * 3];
    bell[0] = QC_VERSION;
    bell[1] = 2; /* qubits */
    bell[2] = 2; /* ops */
    bell[3] = 0; /* probe = |00> */
    bell[4] = 0;
    bell[5] = bell[6] = bell[7] = 0;
    bell[8] = QC_OP_H;
    bell[9] = 0;
    bell[10] = 0;
    bell[11] = QC_OP_CNOT;
    bell[12] = 0;
    bell[13] = 1;
    long j1 = submit_and_wait(bell, sizeof(bell), &out);
    if (j1 <= 0 || out.status != QPU_STATUS_OK || out.result_len != QC_RESULT_LEN) {
        printf("QPU BROKEN bell submit/poll j=%ld status=%u\n", j1, (unsigned)out.status);
        broken = 1;
    } else {
        unsigned int h = get_u16(out.result + 2);
        unsigned int num = get_u32(out.result + 4);
        unsigned int den = get_u32(out.result + 8);
        int amp_re = (int)get_u32(out.result + 12);
        if (h == 1 && num == 1 && den == 2 && amp_re == 1) {
            printf(QPU_BACKEND_LABEL ": bell via broker job=%ld p=1/2 (exact)\n", j1);
        } else {
            printf("QPU BROKEN bell h=%u p=%u/%u amp=%d\n", h, num, den, amp_re);
            broken = 1;
        }
    }

    /* --- Grover-3q (target |101>=5, 2 iterations): p = 121/128 exactly --- */
    unsigned char grov[QC_HDR + 7 * 3];
    grov[0] = QC_VERSION;
    grov[1] = 3; /* qubits */
    grov[2] = 7; /* ops */
    grov[3] = 5; /* probe = |101> */
    grov[4] = 0;
    grov[5] = grov[6] = grov[7] = 0;
    {
        unsigned char *op = grov + QC_HDR;
        op[0] = QC_OP_H;
        op[1] = 0;
        op[2] = 0;
        op[3] = QC_OP_H;
        op[4] = 1;
        op[5] = 0;
        op[6] = QC_OP_H;
        op[7] = 2;
        op[8] = 0;
        op[9] = QC_OP_ORACLE;
        op[10] = 5;
        op[11] = 0;
        op[12] = QC_OP_DIFFUSION;
        op[13] = 0;
        op[14] = 0;
        op[15] = QC_OP_ORACLE;
        op[16] = 5;
        op[17] = 0;
        op[18] = QC_OP_DIFFUSION;
        op[19] = 0;
        op[20] = 0;
    }
    long j2 = submit_and_wait(grov, sizeof(grov), &out);
    if (j2 <= 0 || out.status != QPU_STATUS_OK || out.result_len != QC_RESULT_LEN) {
        printf("QPU BROKEN grover3 submit/poll j=%ld status=%u\n", j2, (unsigned)out.status);
        broken = 1;
    } else {
        unsigned int h = get_u16(out.result + 2);
        unsigned int num = get_u32(out.result + 4);
        unsigned int den = get_u32(out.result + 8);
        int amp_re = (int)get_u32(out.result + 12);
        if (h == 15 && num == 121 && den == 128 && amp_re == 176) {
            printf(QPU_BACKEND_LABEL ": grover3 via broker job=%ld p=121/128\n", j2);
        } else {
            printf("QPU BROKEN grover3 h=%u p=%u/%u amp=%d\n", h, num, den, amp_re);
            broken = 1;
        }
    }

    /* --- Quota: qsub_max=2, so a THIRD submit must be refused EPERM (-4) --- */
    {
        qpu_submit_req_t req;
        req.circuit_len = sizeof(bell);
        for (unsigned int i = 0; i < sizeof(bell); i++) {
            req.circuit[i] = bell[i];
        }
        long r = qpu_submit_(&req);
        if (r == -4) {
            write_str("QPU: quota ENFORCED (third submit refused)");
        } else {
            printf("QPU BROKEN third=%ld\n", r);
            broken = 1;
        }
    }

    /* --- Cross-perm partition: a submitter (WRITE) cannot COMPLETE (EXECUTE) --- */
    {
        qpu_complete_req_t crep;
        crep.job_id = 1;
        crep.status = QPU_STATUS_OK;
        crep.result_len = 0;
        long r = qpu_complete_(&crep);
        if (r == -4) {
            write_str("QPU: submitter complete denied (EPERM)");
        } else {
            printf("QPU BROKEN complete=%ld\n", r);
            broken = 1;
        }
    }

    if (!broken) {
        write_str("QPU: broker proof complete (submit/execute/poll + quota + partition)");
    }
    /* Idle forever: a monitored respawn would double the QSUBMIT ledger count
     * and re-run the proof; not monitored + never exiting keeps the qsub=2/2
     * manifest row and the ledger evidence stable for the MCP gate. */
    for (;;) {
        yield();
    }
}
