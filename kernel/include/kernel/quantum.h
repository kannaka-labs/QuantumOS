/**
 * QuantumOS Quantum Resource Management (#4)
 *
 * Qubits, coherence windows, and quantum execution contexts as
 * first-class, capability-guarded OS resources. The current backend is
 * a deterministic in-kernel simulator (integer-only: fidelity in
 * basis points, coherence in timer ticks, measurements from a seeded
 * xorshift PRNG); the API is written against the quantum_types.h
 * shapes so hardware backends slot in behind it later.
 *
 * Access control: every operation takes (pid, cap_id) and validates a
 * CAP_RESOURCE_QUANTUM capability on the shared pool via cap_check —
 * no ambient authority.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef QUANTUM_H
#define QUANTUM_H

#include <kernel/types.h>
#include <kernel/quantum_types.h>
#include <kernel/capability.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define QUANTUM_MAX_QUBITS 64 /* simulated pool size */
#define QUANTUM_MAX_CONTEXTS 16
#define QUANTUM_MAX_CTX_QUBITS 16  /* qubits per context */
#define QUANTUM_POOL_RESOURCE_ID 0 /* resource_id of the shared pool */

/* Default coherence window for a fresh allocation, in timer ticks
 * (10 ms each at TIMER_DEFAULT_HZ) */
#define QUANTUM_COHERENCE_TICKS 500 /* 5 seconds */

/* Fidelity cost per gate, in basis points (fidelity is 0..10000) */
#define QUANTUM_GATE_FIDELITY_COST 2

/* ============================================================================
 * API
 * ============================================================================ */

/* Provide externally sourced entropy (e.g. real QPU bits handed in by
 * the qBraid boot harness via the qseed cmdline) to seed the PRNG.
 * Call before quantum_init(); a zero/absent seed falls back to the
 * internal default. */
void quantum_set_boot_entropy(uint64_t seed);

/* Mix host-supplied entropy into the generator AFTER boot: used when a VM is
 * restored from a snapshot, so every restored copy draws a different stream
 * instead of replaying the snapshot's generator state (ADR-0025). Always mixes
 * (never replaces) the live state, so a hostile caller cannot reduce entropy.
 * If the kernel booted WITHOUT a qseed, the seed is also recorded as the
 * host seed that quantum_boot_seed()/SYS_QSEED report; a kernel that booted
 * WITH a qseed keeps reporting that boot qseed unchanged (attestation binds to
 * it). seed == 0 is ignored. Safe to call from IRQ context. */
void quantum_session_reseed(uint64_t seed);

/* One-shot, non-IRQ: the line describing the seed source after a session reseed
 * ("host-seeded ... (host OS random, not a QPU)" unless a boot qseed is mixed in),
 * or NULL when there is nothing new to say. Printed from the console read path. */
const char *quantum_take_reseed_notice(void);

/* Number of session reseeds accepted since boot (diagnostics). */
uint32_t quantum_session_reseeds(void);

/* Initialize the quantum resource manager (before process_init) */
quantum_result_t quantum_init(void);

/* Mint a capability for the shared qubit pool, owned by pid.
 * Kernel-side helper used at process creation for quantum-aware
 * processes; permissions should include CAP_QUANTUM. */
quantum_result_t quantum_grant(uint32_t pid, uint32_t permissions, uint32_t *cap_id_out);

/* Allocate `count` qubits for pid. Requires CAP_QUANTUM | CAP_WRITE. */
quantum_result_t quantum_alloc_qubits(uint32_t pid, uint32_t cap_id, uint32_t count,
                                      uint32_t *qubit_ids_out);

/* Release qubits previously allocated by pid. */
quantum_result_t quantum_release_qubits(uint32_t pid, uint32_t cap_id, const uint32_t *qubit_ids,
                                        uint32_t count);

/* Remaining coherence window of a qubit in timer ticks (0 = decohered
 * or unallocated). Coherence is tracked lazily against the tick
 * counter — no per-tick bookkeeping. */
uint64_t quantum_coherence_remaining(uint32_t qubit_id);

/* Create an execution context with `qubits_required` freshly
 * allocated qubits. Requires CAP_QUANTUM | CAP_WRITE. */
quantum_result_t quantum_context_create(uint32_t pid, uint32_t cap_id, uint32_t qubits_required,
                                        uint32_t priority, uint32_t *context_id_out);

/* Destroy a context and release its qubits. */
quantum_result_t quantum_context_destroy(uint32_t pid, uint32_t cap_id, uint32_t context_id);

/* Execute a gate sequence on a context (simulated). Gates target
 * context-relative qubit indices. GATE_MEASURE appends a
 * measurement_event_t to results. Requires CAP_QUANTUM | CAP_EXECUTE.
 * Fails with QUANTUM_ERROR_DECOHERED if any context qubit's coherence
 * window has closed. */
quantum_result_t quantum_execute_circuit(uint32_t pid, uint32_t cap_id, uint32_t context_id,
                                         const quantum_gate_t *gates, uint32_t num_gates,
                                         measurement_event_t *results, uint32_t max_results,
                                         uint32_t *num_results_out);

/* Capability-gated random draw for user space (backs SYS_QRAND). The
 * caller must hold a CAP_RESOURCE_QUANTUM capability with CAP_READ on the
 * shared pool (capability-as-address — no cap_id handle is threaded through
 * the syscall, exactly like the IPC send path). On success draws `count`
 * bytes from the qseed-mixed generator into `out` and, if seed_present_out
 * is non-NULL, reports whether a boot qseed was mixed into that generator.
 * Returns QUANTUM_ERROR_HARDWARE_FAULT (denied) when the capability is
 * absent, so the syscall layer can map it to EPERM. `count` may be 0 (a
 * provenance-only query); `out` is then unused and may be NULL. */
quantum_result_t quantum_user_random(uint32_t pid, uint8_t *out, uint32_t count,
                                     uint8_t *seed_present_out);

/* Kernel-internal draw from the same qseed-mixed generator (integer only),
 * plus the running total of quantum-derived bits it has served. Used by the
 * optional lottery scheduler (SCHED_LOTTERY); harmless if never called. */
uint32_t quantum_kernel_rand(void);
uint64_t quantum_bits_consumed(void);

/* The boot qseed the kernel accepted on the cmdline (0 = none). Backs the
 * capability-gated SYS_QSEED so a ring-3 attestation service can bind its
 * boot record to the exact entropy the system started with. For a kernel
 * booted without a qseed and later reseeded after a snapshot restore
 * (quantum_session_reseed), this is the first session seed the host supplied
 * (ADR-0025): same syscall number, shape and errno contract as frozen in v1. */
uint64_t quantum_boot_seed(void);

/* Pool statistics */
void quantum_get_pool_stats(qubit_pool_t *stats);

/* Boot-time self test; logs results, QUANTUM_SUCCESS if all pass */
quantum_result_t quantum_selftest(void);

#endif /* QUANTUM_H */
