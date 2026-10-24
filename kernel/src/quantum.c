/**
 * QuantumOS Quantum Resource Management Implementation (#4)
 *
 * Simulated backend, integer arithmetic only (this file compiles
 * without SSE): fidelity in basis points, coherence in timer ticks,
 * measurement outcomes from xorshift64. All entry points validate a
 * CAP_RESOURCE_QUANTUM capability against the shared pool.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/quantum.h>
#include <kernel/interrupts.h>
#include <kernel/boot.h>

/* ============================================================================
 * Internal State
 * ============================================================================ */

typedef struct {
    qubit_handle_t handle;
    uint32_t owner_pid;
    uint32_t context_id;      /* 0 = not bound to a context */
    uint64_t alloc_tick;      /* tick at allocation (coherence anchor) */
    uint64_t coherence_ticks; /* window length */
} sim_qubit_t;

typedef struct {
    quantum_context_t ctx;
    uint32_t owner_pid;
    uint32_t qubit_ids[QUANTUM_MAX_CTX_QUBITS];
    uint8_t in_use;
    uint8_t qubit_state[QUANTUM_MAX_CTX_QUBITS]; /* classical shadow bit */
} sim_context_t;

static sim_qubit_t pool[QUANTUM_MAX_QUBITS];
static sim_context_t contexts[QUANTUM_MAX_CONTEXTS];
static qubit_pool_t pool_stats;
static uint32_t next_context_id = 1;
static uint32_t next_measurement_id = 1;
static uint64_t prng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t boot_entropy = 0;
static uint8_t boot_entropy_present = 0;
static uint8_t quantum_initialized = 0;
static uint64_t qrand_bits_served = 0;     /* total bits drawn via QRAND/lottery */
static uint8_t boot_qseed_handoff = 0;     /* a qseed= came in on the boot cmdline */
static volatile uint8_t reseed_notice = 0; /* a host session reseed not yet announced */
static uint32_t session_reseeds = 0;       /* host reseeds accepted after boot (ADR-0025) */

/* ============================================================================
 * Helpers
 * ============================================================================ */

static uint64_t xorshift64(void) {
    uint64_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng_state = x;
    return x;
}

/* SplitMix64 avalanche — diffuse every input bit before mixing a seed in. */
static uint64_t splitmix64(uint64_t x) {
    uint64_t z = x + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Mix `seed` into `state`, NEVER returning 0. xorshift64 has 0 as an ABSORBING
 * fixed point (xorshift64(0)==0 forever), so a zero state silently and
 * permanently disables all kernel randomness — SYS_QRAND returns zero bytes
 * (while still reporting provenance OK), quantum_kernel_rand() returns 0, the
 * DNS transaction-id/source-port anti-spoof randomization collapses to a
 * constant, and any service's Lamport key material becomes predictable. Because
 * splitmix64 is a bijection, an attacker who controls the untrusted `qseed=`
 * boot token can invert it and pick the ONE value whose avalanche equals the
 * golden init constant, cancelling the state to exactly 0. The guard closes it. */
static uint64_t mix_seed_nonzero(uint64_t state, uint64_t seed) {
    state ^= splitmix64(seed);
    if (state == 0) {
        state = 0x9E3779B97F4A7C15ULL;
    }
    return state;
}

static cap_result_t check_pool_cap(uint32_t pid, uint32_t cap_id, uint32_t required_perms) {
    return cap_check(cap_id, pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID, required_perms);
}

static int qubit_decohered(const sim_qubit_t *q) {
    if (!q->handle.is_allocated) {
        return 1;
    }
    return (timer_get_ticks() - q->alloc_tick) >= q->coherence_ticks;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void quantum_set_boot_entropy(uint64_t seed) {
    boot_entropy = seed;
    boot_entropy_present = 1;
    boot_qseed_handoff = 1;
}

quantum_result_t quantum_init(void) {
    memset(pool, 0, sizeof(pool));
    memset(contexts, 0, sizeof(contexts));
    memset(&pool_stats, 0, sizeof(pool_stats));

    for (uint32_t i = 0; i < QUANTUM_MAX_QUBITS; i++) {
        pool[i].handle.qubit_id = i;
        pool[i].handle.simulator_id = i;
        pool[i].handle.fidelity = FIDELITY_HIGH;
    }

    pool_stats.total_qubits = QUANTUM_MAX_QUBITS;
    pool_stats.available_qubits = QUANTUM_MAX_QUBITS;
    pool_stats.high_fidelity_qubits = QUANTUM_MAX_QUBITS;

    if (boot_entropy_present) {
        /* Mix the externally supplied quantum entropy into the PRNG state
         * (SplitMix avalanche + non-zero guard: an adversarial qseed must never
         * be able to zero the generator — see mix_seed_nonzero). */
        prng_state = mix_seed_nonzero(prng_state, boot_entropy);
        boot_log("Quantum entropy: seeded from boot handoff (qBraid quantum lab)");
    } else {
        prng_state = mix_seed_nonzero(prng_state, timer_get_ticks() + 1);
        boot_log("Entropy: unseeded at boot (timer default, not from a QPU)");
    }

    quantum_initialized = 1;
    boot_log("Quantum resource manager initialized (simulated backend)");
    return QUANTUM_SUCCESS;
}

quantum_result_t quantum_grant(uint32_t pid, uint32_t permissions, uint32_t *cap_id_out) {
    if (!quantum_initialized || !cap_id_out) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }
    if (cap_create(pid, CAP_RESOURCE_QUANTUM, QUANTUM_POOL_RESOURCE_ID, permissions, 0,
                   cap_id_out) != CAP_SUCCESS) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }
    return QUANTUM_SUCCESS;
}

/* ============================================================================
 * Qubit allocation
 * ============================================================================ */

quantum_result_t quantum_alloc_qubits(uint32_t pid, uint32_t cap_id, uint32_t count,
                                      uint32_t *qubit_ids_out) {
    if (!quantum_initialized || !qubit_ids_out || count == 0) {
        return QUANTUM_ERROR_NO_QUBITS;
    }
    if (check_pool_cap(pid, cap_id, CAP_QUANTUM | CAP_WRITE) != CAP_SUCCESS) {
        return QUANTUM_ERROR_HARDWARE_FAULT; /* denied */
    }
    if (count > pool_stats.available_qubits) {
        return QUANTUM_ERROR_NO_QUBITS;
    }

    uint32_t found = 0;
    for (uint32_t i = 0; i < QUANTUM_MAX_QUBITS && found < count; i++) {
        if (!pool[i].handle.is_allocated) {
            pool[i].handle.is_allocated = 1;
            pool[i].handle.fidelity = FIDELITY_HIGH;
            pool[i].handle.coherence_time = QUANTUM_COHERENCE_TICKS;
            pool[i].owner_pid = pid;
            pool[i].context_id = 0;
            pool[i].alloc_tick = timer_get_ticks();
            pool[i].coherence_ticks = QUANTUM_COHERENCE_TICKS;
            qubit_ids_out[found++] = i;
        }
    }

    pool_stats.available_qubits -= found;
    pool_stats.allocated_qubits += found;
    return QUANTUM_SUCCESS;
}

quantum_result_t quantum_release_qubits(uint32_t pid, uint32_t cap_id, const uint32_t *qubit_ids,
                                        uint32_t count) {
    if (!quantum_initialized || !qubit_ids) {
        return QUANTUM_ERROR_NO_QUBITS;
    }
    if (check_pool_cap(pid, cap_id, CAP_QUANTUM) != CAP_SUCCESS) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = qubit_ids[i];
        if (id >= QUANTUM_MAX_QUBITS) {
            continue;
        }
        if (pool[id].handle.is_allocated && pool[id].owner_pid == pid) {
            pool[id].handle.is_allocated = 0;
            pool[id].owner_pid = 0;
            pool[id].context_id = 0;
            pool_stats.available_qubits++;
            pool_stats.allocated_qubits--;
        }
    }
    return QUANTUM_SUCCESS;
}

uint64_t quantum_coherence_remaining(uint32_t qubit_id) {
    if (qubit_id >= QUANTUM_MAX_QUBITS || !pool[qubit_id].handle.is_allocated) {
        return 0;
    }
    uint64_t elapsed = timer_get_ticks() - pool[qubit_id].alloc_tick;
    if (elapsed >= pool[qubit_id].coherence_ticks) {
        return 0;
    }
    return pool[qubit_id].coherence_ticks - elapsed;
}

/* ============================================================================
 * Contexts
 * ============================================================================ */

quantum_result_t quantum_context_create(uint32_t pid, uint32_t cap_id, uint32_t qubits_required,
                                        uint32_t priority, uint32_t *context_id_out) {
    if (!quantum_initialized || !context_id_out || qubits_required == 0 ||
        qubits_required > QUANTUM_MAX_CTX_QUBITS) {
        return QUANTUM_ERROR_NO_QUBITS;
    }
    if (check_pool_cap(pid, cap_id, CAP_QUANTUM | CAP_WRITE) != CAP_SUCCESS) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }

    sim_context_t *slot = NULL;
    for (uint32_t i = 0; i < QUANTUM_MAX_CONTEXTS; i++) {
        if (!contexts[i].in_use) {
            slot = &contexts[i];
            break;
        }
    }
    if (!slot) {
        return QUANTUM_ERROR_NO_QUBITS;
    }

    quantum_result_t r = quantum_alloc_qubits(pid, cap_id, qubits_required, slot->qubit_ids);
    if (r != QUANTUM_SUCCESS) {
        return r;
    }

    slot->in_use = 1;
    slot->owner_pid = pid;
    memset(slot->qubit_state, 0, sizeof(slot->qubit_state));
    slot->ctx.context_id = next_context_id++;
    slot->ctx.priority = priority;
    slot->ctx.deadline = timer_get_ticks() + QUANTUM_COHERENCE_TICKS;
    slot->ctx.qubits_required = qubits_required;
    slot->ctx.qubit_ids = slot->qubit_ids;
    slot->ctx.circuit_depth = 0;
    slot->ctx.speculative = 0;

    for (uint32_t i = 0; i < qubits_required; i++) {
        pool[slot->qubit_ids[i]].context_id = slot->ctx.context_id;
    }

    *context_id_out = slot->ctx.context_id;
    return QUANTUM_SUCCESS;
}

static sim_context_t *find_context(uint32_t context_id, uint32_t pid) {
    for (uint32_t i = 0; i < QUANTUM_MAX_CONTEXTS; i++) {
        if (contexts[i].in_use && contexts[i].ctx.context_id == context_id &&
            contexts[i].owner_pid == pid) {
            return &contexts[i];
        }
    }
    return NULL;
}

quantum_result_t quantum_context_destroy(uint32_t pid, uint32_t cap_id, uint32_t context_id) {
    if (check_pool_cap(pid, cap_id, CAP_QUANTUM) != CAP_SUCCESS) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }

    sim_context_t *slot = find_context(context_id, pid);
    if (!slot) {
        return QUANTUM_ERROR_NO_QUBITS;
    }

    quantum_release_qubits(pid, cap_id, slot->qubit_ids, slot->ctx.qubits_required);
    memset(slot, 0, sizeof(*slot));
    return QUANTUM_SUCCESS;
}

/* ============================================================================
 * Circuit execution (simulated)
 * ============================================================================ */

quantum_result_t quantum_execute_circuit(uint32_t pid, uint32_t cap_id, uint32_t context_id,
                                         const quantum_gate_t *gates, uint32_t num_gates,
                                         measurement_event_t *results, uint32_t max_results,
                                         uint32_t *num_results_out) {
    if (!quantum_initialized || !gates || !num_results_out) {
        return QUANTUM_ERROR_MEASUREMENT_FAILED;
    }
    if (check_pool_cap(pid, cap_id, CAP_QUANTUM | CAP_EXECUTE) != CAP_SUCCESS) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }

    sim_context_t *slot = find_context(context_id, pid);
    if (!slot) {
        return QUANTUM_ERROR_NO_QUBITS;
    }

    /* Coherence gate: every context qubit must still be in-window */
    for (uint32_t i = 0; i < slot->ctx.qubits_required; i++) {
        if (qubit_decohered(&pool[slot->qubit_ids[i]])) {
            return QUANTUM_ERROR_DECOHERED;
        }
    }

    *num_results_out = 0;

    for (uint32_t g = 0; g < num_gates; g++) {
        const quantum_gate_t *gate = &gates[g];
        if (gate->num_targets == 0 || !gate->target_qubits) {
            continue;
        }
        uint32_t target = gate->target_qubits[0];
        if (target >= slot->ctx.qubits_required) {
            continue;
        }
        uint32_t pool_id = slot->qubit_ids[target];

        /* Every gate costs fidelity (in basis points) */
        if (pool[pool_id].handle.fidelity > QUANTUM_GATE_FIDELITY_COST) {
            pool[pool_id].handle.fidelity -= QUANTUM_GATE_FIDELITY_COST;
        }
        slot->ctx.circuit_depth++;

        switch (gate->gate_type) {
        case GATE_X:
            slot->qubit_state[target] ^= 1;
            break;
        case GATE_H:
            /* Superposition: shadow bit becomes PRNG-random at the
             * next measurement; mark with 0x80 */
            slot->qubit_state[target] |= 0x80;
            break;
        case GATE_CNOT: {
            uint32_t ctrl = gate->control_qubit;
            if (ctrl < slot->ctx.qubits_required && (slot->qubit_state[ctrl] & 1)) {
                slot->qubit_state[target] ^= 1;
            }
            break;
        }
        case GATE_MEASURE: {
            if (*num_results_out >= max_results || !results) {
                return QUANTUM_ERROR_MEASUREMENT_FAILED;
            }
            uint8_t bit;
            if (slot->qubit_state[target] & 0x80) {
                bit = (uint8_t)(xorshift64() & 1); /* collapse */
            } else {
                bit = slot->qubit_state[target] & 1;
            }
            slot->qubit_state[target] = bit;

            measurement_event_t *ev = &results[(*num_results_out)++];
            ev->measurement_id = next_measurement_id++;
            ev->qubit_id = pool_id;
            ev->result = bit;
            ev->probability = 0.5; /* constant store, no FP arithmetic */
            ev->timestamp = timer_get_ticks();
            ev->collapsed = 1;
            break;
        }
        default:
            /* Y/Z/rotations: fidelity cost only in the shadow model */
            break;
        }
    }

    return QUANTUM_SUCCESS;
}

void quantum_get_pool_stats(qubit_pool_t *stats) {
    if (stats) {
        *stats = pool_stats;
    }
}

/* ============================================================================
 * Capability-gated randomness (SYS_QRAND + lottery scheduler)
 * ============================================================================ */

quantum_result_t quantum_user_random(uint32_t pid, uint8_t *out, uint32_t count,
                                     uint8_t *seed_present_out) {
    if (!quantum_initialized) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }
    /* Pool guard: the caller must own a live CAP_RESOURCE_QUANTUM capability
     * carrying CAP_READ over the shared pool. cap_find enforces the read
     * right; we additionally pin the resource to the pool. No capability ->
     * denied (the syscall layer turns this into EPERM). This is the same
     * no-ambient-authority rule the alloc/execute paths use — a caller
     * without the cap gets nothing. */
    uint32_t rid = 0;
    if (cap_find(pid, CAP_RESOURCE_QUANTUM, CAP_READ, &rid) != CAP_SUCCESS ||
        rid != QUANTUM_POOL_RESOURCE_ID) {
        return QUANTUM_ERROR_HARDWARE_FAULT;
    }

    if (seed_present_out) {
        *seed_present_out = boot_entropy_present;
    }

    /* Draw `count` bytes, refreshing a 64-bit word every 8 bytes. */
    uint64_t word = 0;
    for (uint32_t i = 0; i < count; i++) {
        if ((i & 7u) == 0) {
            word = xorshift64();
        }
        if (out) {
            out[i] = (uint8_t)(word & 0xFFu);
        }
        word >>= 8;
    }
    qrand_bits_served += (uint64_t)count * 8u;
    return QUANTUM_SUCCESS;
}

uint32_t quantum_kernel_rand(void) {
    uint32_t r = (uint32_t)(xorshift64() & 0xFFFFFFFFu);
    qrand_bits_served += 32u;
    return r;
}

uint64_t quantum_bits_consumed(void) {
    return qrand_bits_served;
}

void quantum_session_reseed(uint64_t seed) {
    if (seed == 0) {
        return;
    }
    /* Mix, never replace: the restored generator state is unknown to the
     * caller, so folding its bits in cannot lower the state's entropy. */
    prng_state = mix_seed_nonzero(prng_state, seed);
    if (!boot_entropy_present) {
        /* No host seed at boot (a console snapshot boots with none): record
         * this one so SYS_QSEED names the entropy this session started with. */
        boot_entropy = seed;
        boot_entropy_present = 1;
    }
    session_reseeds++;
    reseed_notice = 1;
}

const char *quantum_take_reseed_notice(void) {
    if (!reseed_notice) {
        return NULL;
    }
    reseed_notice = 0;
    /* Say where this session's numbers actually come from (ADR-0025): the
     * host's OS random source, unless a qseed= boot handoff is also mixed in. */
    return boot_qseed_handoff ? "Entropy: session reseed mixed into the boot qseed handoff"
                              : "Entropy: host-seeded for this session (host OS random, not a QPU)";
}

uint32_t quantum_session_reseeds(void) {
    return session_reseeds;
}

uint64_t quantum_boot_seed(void) {
    return boot_entropy_present ? boot_entropy : 0;
}

/* ============================================================================
 * Boot self-test
 * ============================================================================ */

#define QT_ASSERT(cond, msg)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            boot_log("quantum-selftest FAIL: " msg);                                               \
            return QUANTUM_ERROR_HARDWARE_FAULT;                                                   \
        }                                                                                          \
    } while (0)

quantum_result_t quantum_selftest(void) {
    uint32_t cap = 0, bad_cap = 0, ctx = 0;
    uint32_t ids[4];

    QT_ASSERT(quantum_grant(1, CAP_QUANTUM | CAP_READ | CAP_WRITE | CAP_EXECUTE, &cap) ==
                  QUANTUM_SUCCESS,
              "grant");

    /* No ambient authority: allocation without a capability is denied */
    QT_ASSERT(quantum_alloc_qubits(1, CAP_ID_INVALID, 2, ids) != QUANTUM_SUCCESS,
              "alloc without cap denied");
    /* A read-only capability cannot allocate */
    QT_ASSERT(quantum_grant(2, CAP_QUANTUM, &bad_cap) == QUANTUM_SUCCESS, "grant read-only");
    QT_ASSERT(quantum_alloc_qubits(2, bad_cap, 2, ids) != QUANTUM_SUCCESS,
              "alloc without WRITE denied");

    /* Context + Bell-style circuit: H(0), CNOT(0->1), measure both */
    QT_ASSERT(quantum_context_create(1, cap, 2, 1, &ctx) == QUANTUM_SUCCESS, "context create");

    uint32_t t0 = 0, t1 = 1;
    quantum_gate_t circuit[4];
    memset(circuit, 0, sizeof(circuit));
    circuit[0].gate_type = GATE_H;
    circuit[0].target_qubits = &t0;
    circuit[0].num_targets = 1;
    circuit[1].gate_type = GATE_CNOT;
    circuit[1].target_qubits = &t1;
    circuit[1].num_targets = 1;
    circuit[1].control_qubit = 0;
    circuit[2].gate_type = GATE_MEASURE;
    circuit[2].target_qubits = &t0;
    circuit[2].num_targets = 1;
    circuit[3].gate_type = GATE_MEASURE;
    circuit[3].target_qubits = &t1;
    circuit[3].num_targets = 1;

    measurement_event_t events[4];
    uint32_t n_events = 0;
    QT_ASSERT(quantum_execute_circuit(1, cap, ctx, circuit, 4, events, 4, &n_events) ==
                  QUANTUM_SUCCESS,
              "circuit execute");
    QT_ASSERT(n_events == 2, "two measurements");
    QT_ASSERT(events[0].result <= 1 && events[1].result <= 1, "bits are bits");

    /* Coherence is tracked and within the initial window */
    uint32_t q0;
    {
        sim_context_t *probe = find_context(ctx, 1);
        QT_ASSERT(probe != NULL, "context lookup");
        q0 = probe->qubit_ids[0];
    }
    QT_ASSERT(quantum_coherence_remaining(q0) > 0, "coherence window open");

    /* Foreign pid cannot destroy the context */
    QT_ASSERT(quantum_context_destroy(2, bad_cap, ctx) != QUANTUM_SUCCESS,
              "foreign destroy denied");
    QT_ASSERT(quantum_context_destroy(1, cap, ctx) == QUANTUM_SUCCESS, "context destroy");

    /* Pool back to fully available */
    qubit_pool_t stats;
    quantum_get_pool_stats(&stats);
    QT_ASSERT(stats.allocated_qubits == 0, "pool drained back");

    /* QRAND capability gate: a read cap draws bytes; a caller with no
     * quantum cap — or a cap lacking CAP_READ (pid 2 holds CAP_QUANTUM
     * only) — is denied. Same no-ambient-authority rule as allocation. */
    uint8_t seedp = 0xFF;
    uint8_t rbytes[8];
    QT_ASSERT(quantum_user_random(1, rbytes, sizeof(rbytes), &seedp) == QUANTUM_SUCCESS,
              "qrand with read cap");
    QT_ASSERT(seedp <= 1, "qrand reports seed flag");
    QT_ASSERT(quantum_user_random(99, rbytes, sizeof(rbytes), 0) != QUANTUM_SUCCESS,
              "qrand denied without cap");
    QT_ASSERT(quantum_user_random(2, rbytes, sizeof(rbytes), 0) != QUANTUM_SUCCESS,
              "qrand denied without READ right");

    cap_revoke(cap, 1);
    cap_revoke(bad_cap, 2);

    /* Adversarial-qseed guard (bug-hunt of the untrusted boot cmdline): the
     * qseed= token must NEVER be able to zero the PRNG (xorshift64's absorbing
     * fixed point silently kills all randomness). mix_seed_nonzero is the exact
     * path quantum_init seeds through. Two cases: the specific attacker value
     * whose splitmix64 avalanche inverts to the golden constant (cancelling
     * state to 0), and the generic state == splitmix64(seed) cancellation. The
     * boot gates on the line below; without the guard the first assert trips and
     * the boot panics before "QuantumOS ready". */
    QT_ASSERT(mix_seed_nonzero(0x9E3779B97F4A7C15ULL, 0xCD36DA00563C5EF6ULL) != 0,
              "qseed cannot zero PRNG (attacker value)");
    QT_ASSERT(mix_seed_nonzero(splitmix64(0), 0) != 0, "qseed cannot zero PRNG (generic)");
    boot_log("QSEEDGUARD: PRNG survives adversarial qseed (non-zero)");

    boot_log("quantum self-test: PASS");
    return QUANTUM_SUCCESS;
}
