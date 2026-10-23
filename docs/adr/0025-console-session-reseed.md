# 25. Reseed Each Console Session After a Snapshot Restore

Date: 2026-10-22
Status: Accepted

## Context

The public console (tools/qos-console) restores every visitor's VM from one Firecracker
snapshot of a booted kernel (#249). The snapshot includes the kernel's generator state
(`prng_state` in `kernel/src/quantum.c`). Console VMs boot with `quiet` and no `qseed=`, so
that state was fixed at snapshot time. As a result, every visitor saw the same `qseed`
(0, "no seed") and the same `qrand` sequence. Nothing on the console uses the generator for
secrets: the VM has no NIC. Still, a demo that rolls the same dice for everyone misrepresents
the generator.

ADR-0020 freezes the v1 syscall ABI (`contracts/abi/v1.golden`), the MCP tool schemas and the
COM2 frame format. ADR-0017 forbids calling host entropy quantum.

## Decision

After each successful restore, and before the visitor's first byte, the launcher sends one
escape sequence on COM1:

    ESC ] q s e e d ; <16 hex digits> BEL

The 16 hex digits are 8 bytes from `secrets.token_bytes`, and never all zero.

`console_com1_irq` passes each received byte through a small recognizer
(`com1_rx_byte`, `kernel/src/console.c`):
- A complete sequence is consumed. It never reaches the shell. Its value goes to
  `quantum_session_reseed`.
- Any byte that breaks the sequence releases the held bytes, in order, as ordinary input.
  Arrow keys (`ESC [`) are unaffected. At most 25 bytes are held.

`quantum_session_reseed` *mixes* the seed into the live state (`mix_seed_nonzero`); it never
replaces it. So a visitor who types the sequence by hand only adds entropy.

If the kernel booted without a `qseed=`, the first session seed is also recorded as the seed
`SYS_QSEED` reports. A kernel booted with a `qseed=` (the qBraid-attested path) keeps
reporting its boot qseed, because attestation binds to it.

No syscall number, struct, errno, MCP schema or COM2 format changes. `v1.golden` is untouched.

The shell's help text no longer says the numbers are "quantum-seeded". On the console the
seed comes from the host's OS random source, not a QPU.

## Consequences

Positive:
- Two visitors in a row get different `qseed` and `qrand` output.
- A kernel without the launcher (cold boot, QEMU, CI) never receives the escape. It boots
  and seeds exactly as before.
- Restore time is unchanged: one 25-byte write.

Negative:
- `SYS_QSEED` on a console VM now reports a host seed, not a boot cmdline seed. The meaning
  is widened, and documented in `quantum.h`.
- COM1 input gets an in-band control sequence. A paste containing the exact sequence is
  consumed rather than echoed, which is harmless but invisible.
- The visitor cannot verify that the seed is fresh. The launcher is trusted for that, as it
  already is for the VM itself.

Residual risk: the snapshot still shares every other piece of guest state (the timer tick
count at restore, heap layout). That is fine for a demo with no network and no secrets.

## Evidence

- `kernel/src/console.c`: `com1_rx_byte` and `RESEED_PREFIX`.
- `kernel/src/quantum.c`: `quantum_session_reseed`.
- `tools/qos-console/qos_console.py`: `reseed_escape`, sent in `session()` after `_restore`.
- `tools/qos-console/test_qos_console.py`:
  `test_each_restored_session_is_reseeded_before_the_visitor_types` (two restored sessions
  get one distinct seed each, before any visitor input) and `test_reseed_escape_format`.
