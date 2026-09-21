# QuantumOS — Contributor & Agent Guide

Orientation for anyone (human or AI agent) working in this repo. For the *architecture*, read
[`docs/adr/`](docs/adr/) — 21 ADRs recording every major decision with `file:line` evidence and
honest limits. For the *history*, read [`CHANGELOG.md`](CHANGELOG.md).

## What this is

A capability-secure, quantum-aware x86-64 microkernel that boots under QEMU, on real hardware
(GRUB ISO), and [in a browser](https://kannaka-labs.github.io/QuantumOS/) (qemu-wasm). Ring-3
citizens hold only their granted capabilities; the host-side MCP toolkit
(`quantumos-host-tools`, `scripts/`) lets an agent operate a running kernel with a verified boot
identity on every result. North star: **agents operate the OS; safety is structural (kernel
capabilities + attestation), not bolted on — conscience before wallet.**

## Layout

- `kernel/src/` — the kernel (flat; there is **no** `kernel/{core,hal,quantum}` split despite
  older docs). Boot roster of ring-3 citizens is `kernel/src/citizens.c` (ADR-0002).
- `user/` — ring-3 citizens (C) + `libq` runtime; `rootfs/` — initrd contents; `/bin` ELFs.
- `scripts/` — the host Python toolkit (MCP server, `QosVM`/`QosSociety` bridge, gateways).
- `docs/adr/` — architecture decision records (the authoritative as-built reference).
- `.github/workflows/` — `ci.yml` (18 jobs), `browser-demo.yml`, `quantum-phase3.yml`, `release.yml`.

## Build & test (WSL/Linux)

```bash
make BUILD_TYPE=debug          # build the kernel (elf + elf32)
make run                       # boot it interactively in QEMU (-serial stdio)
make ci-smoke                  # the ~60-gate suite: boot + assert un-echoable signals
make build/x86_64/kernel.iso   # GRUB ISO (needs grub-pc-bin xorriso mtools)
make ci-smoke-iso              # boot the real GRUB handoff
```

The version string is single-sourced from the root `VERSION` file into the kernel banner via
`-DQOS_VERSION` (see the Makefile). Bump `VERSION`; do not hardcode a version anywhere.

## House rules (enforced by review + CI)

- **Integer-only where it matters** (ADR-0004): ring 0 runs with no `fxsave`/`fxrstor`, so no
  FPU in kernel/IRQ context. Fixed-point (Q15/Q16.16) and integer SHA-256 only.
- **Single-core by design** (ADR-0005): the concurrency model is `cli`'d syscalls + irqsave
  brackets against the IF=1 health monitor. Do not introduce code that assumes SMP.
- **Anti-vacuous gates** (ADR-0016): every feature lands with a CI gate that asserts a runtime
  signal it could not emit unless the feature works. **Revert-and-confirm-fail** on every gate.
  A boot-only/crash-free check is not a gate.
- **Honest slices** (ADR-0017): ship the narrow real thing, document its boundary, report
  negative results plainly. Never overclaim (e.g. "quantum" entropy is `prng` without a qseed).
- **Every code (`.c`/`.h`) PR touches a doc** (Documentation Sync CI) and is **clang-format-18
  clean** (Code Quality runs it on all C). `cppcheck --error-exitcode` must pass.
- Gate timeouts are single-sourced in the `Makefile`; never duplicate a `timeout` literal in
  `ci.yml`.

## Working discipline (how changes land clean here)

Recon (`file:line`) → design brief → **adversarial attack panel before writing risky code**
(hardware/protocol/concurrency/capability/CI lenses — it catches a blocker nearly every phase) →
implement with a meaningful gate → WSL build + gate + revert-and-confirm-fail → PR (trailer
`Co-Authored-By`) → merge on green → refresh the ISO if the kernel changed. Git operations run
from **Git Bash** (autocrlf=true), not `wsl` git (which sees every file as CRLF-modified).

## Known scaling walls (lift deliberately, per ADR)

`MANIFEST_MAX_ENTRIES` 8 (agentd at 7/8), `FIELD_REGION_COUNT` 8 (7/8 used), `MAX_PEERS` 4,
`MAX_SERVICES` 32, audit ring 256, PMM hardcoded to 128 MB. The QDSK disk homes are **frozen**
(audit at `count-27`); anything touching the disk layout must respect the `ci-smoke-disk-upgrade`
gate (ADR-0007).
