/**
 * QuantumOS v1 WIRE freeze probe - GUEST ring (ADR-0020 lane C).
 *
 * This translation unit is NEVER linked or run. It is compiled to an object
 * file under the REAL user build flags (USER_CFLAGS); scripts/extract-wire.py
 * reads the compiler-measured values back out of the .abi_ents section with
 * objcopy and diffs them against contracts/wire/v1.golden. Emitting
 * compiler-measured values (macro expansions, sizeof, offsetof, packed
 * attestation bytes) - rather than parsing comments or _Static_assert
 * literals - is the whole point: a developer editing a frame struct and its
 * size-claim comment together cannot fool the golden.
 *
 * The HOST ring twin lives in scripts/qos_bridge.py (extract-wire.py imports
 * it); every twinned logical name must carry the same value on both rings, so
 * the guest emitter and the host verifier can never silently disagree on the
 * COM2/FSYN wire.
 *
 * Each entry is a fixed 64-byte record: a 56-char NUL-padded name plus a
 * little-endian u64 value, ring-namespaced (guest: here, host: in python).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "swarm.h"
#include "fsyn.h"

/* Teeth-check hook (inert in a normal build): the wire gate's selftest
 * recompiles this probe with -DSWARM_MAGIC_OVERRIDE=<n> and asserts the gate
 * reddens, proving a CHANGED wire value is actually caught (not just an added
 * or removed name). #undef-then-#define avoids a redefinition diagnostic. */
#ifdef SWARM_MAGIC_OVERRIDE
#undef SWARM_MAGIC
#define SWARM_MAGIC SWARM_MAGIC_OVERRIDE
#endif

/* A wrong data model (ILP32/LLP64) would silently mis-measure every size; fail
 * the compile loudly instead of emitting a plausible-but-wrong layout. */
_Static_assert(sizeof(long) == 8 && sizeof(void *) == 8, "wrong data model / ABI");

/* Attestation known-answer entries: the EXACT bytes build_attestation() emits,
 * packed 8 at a time as little-endian u64s from the SHARED swarm.h macros (not
 * from a re-typed copy of the string). The sizeof guards pin the string
 * lengths the [0..8)/[8..16) windows assume, so a lengthened literal cannot
 * silently truncate out of the KAT. */
_Static_assert(sizeof(SWARM_ATTEST_HEAD) == 16, "attest head is 15 chars + NUL");
_Static_assert(sizeof(SWARM_ATTEST_TICKS) == 8, "attest ticks key is 7 chars + NUL");

/* Pack 8 consecutive bytes of a string literal, starting at offset `o`, as an
 * LE u64 (NUL padding included). Direct subscripting — not pointer arithmetic
 * — keeps every element a compile-time constant under -Werror. */
#define PACK8_AT(s, o)                                                                             \
    ((uint64_t)(uint8_t)(s)[(o) + 0] | ((uint64_t)(uint8_t)(s)[(o) + 1] << 8) |                    \
     ((uint64_t)(uint8_t)(s)[(o) + 2] << 16) | ((uint64_t)(uint8_t)(s)[(o) + 3] << 24) |           \
     ((uint64_t)(uint8_t)(s)[(o) + 4] << 32) | ((uint64_t)(uint8_t)(s)[(o) + 5] << 40) |           \
     ((uint64_t)(uint8_t)(s)[(o) + 6] << 48) | ((uint64_t)(uint8_t)(s)[(o) + 7] << 56))
#define PACK8(s) PACK8_AT(s, 0)

struct abi_ent {
    char name[56];
    unsigned long long value;
};

#define ABI(n, v) {n, (unsigned long long)(v)}

__attribute__((used, section(".abi_ents"))) const struct abi_ent wire_guest[] = {
    /* --- COM2 framing --- */
    ABI("guest:wire:SWARM_MAGIC", SWARM_MAGIC),
    ABI("guest:wire:SWARM_HDR_LEN", SWARM_HDR_LEN),
    ABI("guest:wire:SWARM_MAX_PAYLOAD", SWARM_MAX_PAYLOAD),

    /* --- frame types --- */
    ABI("guest:wire:FRAME_HANDSHAKE", FRAME_HANDSHAKE),
    ABI("guest:wire:FRAME_DATA", FRAME_DATA),
    ABI("guest:wire:FRAME_PING", FRAME_PING),
    ABI("guest:wire:FRAME_PONG", FRAME_PONG),
    ABI("guest:wire:FRAME_DISCONNECT", FRAME_DISCONNECT),
    ABI("guest:wire:FRAME_PKDIGEST", FRAME_PKDIGEST),
    ABI("guest:wire:FRAME_ATTEST", FRAME_ATTEST),
    ABI("guest:wire:FRAME_SIG", FRAME_SIG),

    /* --- DATA routing opcodes --- */
    ABI("guest:wire:SWARM_OP_STATUS", SWARM_OP_STATUS),
    ABI("guest:wire:SWARM_OP_RECALL", SWARM_OP_RECALL),
    ABI("guest:wire:SWARM_OP_QSUBMIT", SWARM_OP_QSUBMIT),
    ABI("guest:wire:SWARM_OP_KEY", SWARM_OP_KEY),

    /* --- Lamport attestation parameters --- */
    ABI("guest:wire:LAMPORT_BITS", LAMPORT_BITS),
    ABI("guest:wire:LAMPORT_HASH_LEN", LAMPORT_HASH_LEN),
    ABI("guest:wire:LAMPORT_SEED_LEN", LAMPORT_SEED_LEN),
    ABI("guest:wire:LAMPORT_SIG_ELEM", LAMPORT_SIG_ELEM),
    ABI("guest:wire:LAMPORT_SIG_LEN", LAMPORT_SIG_LEN),

    /* --- CRC-8/CCITT parameters --- */
    ABI("guest:wire:SWARM_CRC8_POLY", SWARM_CRC8_POLY),
    ABI("guest:wire:SWARM_CRC8_INIT", SWARM_CRC8_INIT),

    /* --- reply-auth + DATA reply body geometry (ADR-0019) --- */
    ABI("guest:wire:SWARM_REPLYAUTH_NONCE_LEN", SWARM_REPLYAUTH_NONCE_LEN),
    ABI("guest:wire:SWARM_REPLYAUTH_TAG_LEN", SWARM_REPLYAUTH_TAG_LEN),
    ABI("guest:wire:SWARM_KEY_LEN", SWARM_KEY_LEN),
    ABI("guest:wire:SWARM_STATUS_BODY_LEN", SWARM_STATUS_BODY_LEN),
    ABI("guest:wire:SWARM_RECALL_BODY_LEN", SWARM_RECALL_BODY_LEN),
    ABI("guest:wire:SWARM_QSUBMIT_BODY_ERR", SWARM_QSUBMIT_BODY_ERR),
    ABI("guest:wire:SWARM_QSUBMIT_BODY_OK", SWARM_QSUBMIT_BODY_OK),
    ABI("guest:wire:SWARM_STATUS_R_SCALE", SWARM_STATUS_R_SCALE),

    /* --- FSYN/FSYP field-coupling UDP frames (fsyn.h) --- */
    ABI("guest:wire:FSYN_MAGIC", FSYN_MAGIC),
    ABI("guest:wire:FSYN_PORT", FSYN_PORT),
    ABI("guest:wire:FSYN_MAC_COVERED", FSYN_MAC_COVERED),
    ABI("guest:size:fsyn_frame_t", sizeof(fsyn_frame_t)),
    ABI("guest:off:fsyn.magic", __builtin_offsetof(fsyn_frame_t, magic)),
    ABI("guest:off:fsyn.seq", __builtin_offsetof(fsyn_frame_t, seq)),
    ABI("guest:off:fsyn.phase", __builtin_offsetof(fsyn_frame_t, phase)),
    ABI("guest:off:fsyn.tag", __builtin_offsetof(fsyn_frame_t, tag)),
    ABI("guest:wire:FSYP_MAGIC", FSYP_MAGIC),
    ABI("guest:size:fsyp_frame_t", sizeof(fsyp_frame_t)),
    ABI("guest:off:fsyp.magic", __builtin_offsetof(fsyp_frame_t, magic)),
    ABI("guest:off:fsyp.aggregate", __builtin_offsetof(fsyp_frame_t, aggregate)),
    ABI("guest:off:fsyp.reserved0", __builtin_offsetof(fsyp_frame_t, reserved0)),

    /* --- attestation string KATs (packed from the SHARED macros) --- */
    ABI("guest:kat:WIRE_ATTEST_HEAD_0", PACK8(SWARM_ATTEST_HEAD)),
    ABI("guest:kat:WIRE_ATTEST_HEAD_1", PACK8_AT(SWARM_ATTEST_HEAD, 8)),
    ABI("guest:kat:WIRE_ATTEST_TICKS_0", PACK8(SWARM_ATTEST_TICKS)),
};
