/**
 * QuantumOS QPU circuit wire format (epic #148) — the OPAQUE payload the
 * kernel broker carries between a submitter and the executor (qpud). The
 * KERNEL never parses this; only ring-3 does. Shared by the builder
 * (qpu_test, ghost_test) and the parser (qpud) so the two can never drift.
 *
 * Circuit bytes (little-endian):
 *   [0]      version (QC_VERSION)
 *   [1]      n_qubits (1..12)
 *   [2]      n_ops
 *   [3..4]   probe_index (u16) — the basis state whose probability is reported
 *   [5..7]   reserved (0)
 *   [8..]    n_ops x { u8 opcode, u8 a, u8 b }
 *
 * Result bytes (QC_RESULT_LEN = 20, little-endian):
 *   [0]      status (QC_STATUS_*)
 *   [1]      n_qubits
 *   [2..3]   h (u16) — Hadamard count == -log2(scale^2)
 *   [4..7]   prob_num  } reduced exact rational |amp|^2 / 2^h for probe_index
 *   [8..11]  prob_den  }
 *   [12..15] amp_re (i32)
 *   [16..19] amp_im (i32)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef QPU_CIRCUIT_H
#define QPU_CIRCUIT_H

#define QC_VERSION 1
#define QC_HDR 8
#define QC_RESULT_LEN 20

/* Opcodes. ORACLE/DIFFUSION are the Grover composites the exact engine
 * implements directly (a multi-controlled Z is not a 2-qubit primitive). */
#define QC_OP_H 1
#define QC_OP_X 2
#define QC_OP_Z 3
#define QC_OP_S 4
#define QC_OP_CNOT 5   /* a=control, b=target */
#define QC_OP_CZ 6     /* a, b */
#define QC_OP_ORACLE 7 /* a = low byte of target basis, b = high byte */
#define QC_OP_DIFFUSION 8

/* What actually executes a brokered circuit, as printed on every result line
 * (qpu_test, agentd) and by qpud at startup. Today that is qpud's exact integer
 * state-vector engine (qsv_engine.h) in ring 3: a simulator inside the guest. The
 * console's real-QPU path (qsh `qpu real`, #262) goes to the host broker and
 * never through qpud, so it does not change this label. This is the ONE place
 * the label lives. If qpud ever dispatches to a real backend, change it here,
 * and only if that is true for the build. CI gates match the text after the
 * label ("bell via broker job="), so the label can change without touching them. */
#define QPU_BACKEND_LABEL "QSIM (in-guest simulator, not a QPU)"

#define QC_STATUS_OK 0
#define QC_STATUS_EINVAL 1 /* malformed circuit — fail closed */

#endif /* QPU_CIRCUIT_H */
