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

## Update 2026-10-23: say where the seed came from

The console used to print "quantum-seeded dice" (motd, qsh `help`) and "real quantum
entropy" (quantumd), but a console VM has no QPU: its seed is the host's OS randomness
sent by this escape. The kernel now reports the actual source:

- `quantum_set_boot_entropy` (the `qseed=` cmdline handoff) marks the boot seed as a
  handoff; quantum_init logs "seeded from boot handoff (qBraid quantum lab)" as before, or
  "Entropy: unseeded at boot (timer default; no QPU attached)" when there is none.
- `quantum_session_reseed` runs in the COM1 IRQ and must not print, so it only sets a
  one-shot flag. `const char *quantum_take_reseed_notice(void)` (quantum.h) returns the line
  once and clears it: "Entropy: host-seeded for this session (no QPU attached)", or
  "Entropy: session reseed mixed into the boot qseed handoff" when a boot qseed is also
  present. `SYS_CONS_READ` drains it before reading, so the visitor sees it right after the
  restore, before typing. No ABI change: `SYS_QSEED`/`SYS_QRAND` are untouched.
- quantumd, citizens, qsh `help` and the motd no longer call pool entropy quantum unless a
  qseed handoff happened.

## Update 2026-10-23: name the circuit backend too

Every console visitor's boot transcript also showed "QPU: bell via broker" and
"QPU: grover3 via broker" from the `qpu-test` self-test. The broker is real (the kernel's
SYS_QPU queue, caps and quota), but what executes a brokered circuit is `qpud` running the
exact integer state-vector engine (`qsv_engine.h`) in ring 3: a local simulator. No QPU
is attached. The result lines now carry `QPU_BACKEND_LABEL` from `user/qpu_circuit.h`,
the one place the label lives, so they read
"QSIM (local simulator, no QPU attached): grover3 via broker job=2 p=121/128". `qpud`
prints "QPUD: backend QSIM (local simulator, no QPU attached)" when it starts. When a real
backend is dispatched from `qpud` (#262), that string changes, and only for builds where
it is true. The CI gates match the text after the label ("bell via broker job="). The
authority lines ("QPU: quota ENFORCED", "QPU: capless submit denied") are about the
broker and are unchanged.

## Update 2026-10-24: "no QPU attached" is no longer true on the console

Since #269 the public console's qsh has `qpu real`, which sends a circuit through the host
broker and qBraid to a real QPU. The boot lines that said "no QPU attached" then contradicted
the `qpu` command's own "real QPU: on". The words now say what is true: the seed is not from
a QPU, whether or not a QPU is reachable.

- Session reseed: "Entropy: host-seeded for this session (host OS random, not a QPU)".
- Cold boot without a qseed: "Entropy: unseeded at boot (timer default, not from a QPU)".
- motd: "dice seeded by the host, not by a QPU."
- `QPU_BACKEND_LABEL`: "QSIM (in-guest simulator, not a QPU)". The in-guest self-test still
  runs on qpud's exact engine; `qpu real` never goes through qpud.
- The swarm console gate line prints `seed=` instead of `qseed=`, because on the console
  that value is the host session seed. The COM2 wire format (`QOS-BOOT|qseed=...`), the
  verifier and every CI gate are unchanged (the gates match "SWARM: boot attestation emitted").
