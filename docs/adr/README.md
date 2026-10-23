# Architecture Decision Records

This directory records the architectural decisions behind QuantumOS — both **as-built**
(decisions already shipped and gated in CI, status *Accepted*) and **next-phase**
(decisions being adopted, status *Accepted* for work in flight or *Proposed* for planned
work). Each ADR carries `file:line` and PR evidence you can check against the tree.

The as-built ADRs were reconciled from a full ground-truth inventory of the 126 merged
PRs (#7–#183) at main. The next-phase ADRs were produced by an adversarial design panel
(2026-07-11) that attacked eight candidate directions against the real code; its
verdicts — adopt the authenticated swarm plane, the v1 freeze, and honest memory; reject
SMP; defer UEFI, a libagent extraction, and a block filesystem — are recorded here.

## As-built (Accepted)

| ADR | Title |
|-----|-------|
| [0001](0001-capability-based-microkernel.md) | Capability-based security as the sole kernel authority model |
| [0002](0002-declarative-citizen-grants.md) | Declarative citizen grants resolved at spawn |
| [0003](0003-user-pointer-guard.md) | Universal user-pointer copy guard at the syscall boundary |
| [0004](0004-integer-only-math.md) | Integer-only math (no kernel FPU) |
| [0005](0005-single-core-by-design.md) | Single-core by design (the SMP rejection) |
| [0006](0006-holographic-field-syscalls.md) | Holographic field — associative memory as a kernel syscall |
| [0007](0007-qdsk-persistence.md) | QDSK persistence — fixed homes, superblock-last commit, cold-start |
| [0008](0008-intent-manifests.md) | Explicit intent manifests + enforced quotas above capabilities |
| [0009](0009-authority-audit-ledger.md) | The authority audit ledger |
| [0010](0010-one-hop-delegation.md) | Cross-ring one-hop capability delegation (SYS_CAP_DERIVE) |
| [0011](0011-spawn-channels.md) | Spawn channels — societies self-assemble |
| [0012](0012-quantum-front-dont-port.md) | Quantum stack — front real backends, don't port them |
| [0013](0013-qpu-broker-opaque.md) | The QPU broker brokers opaque payloads |
| [0014](0014-field-societies.md) | Distributed field societies over N VMs |
| [0015](0015-agent-native-host-surface.md) | The agent-native host surface (MCP + attested bridge) |
| [0016](0016-anti-vacuous-ci-gates.md) | Anti-vacuous CI gate discipline |
| [0017](0017-honest-slice-engineering.md) | Honest-slice engineering |

## Next-phase

| ADR | Status | Title |
|-----|--------|-------|
| [0018](0018-versioned-releases-and-package.md) | Accepted | Versioned releases and a published host package |
| [0019](0019-authenticated-swarm-plane.md) | Accepted | Authenticate the swarm plane (FSYN + COM2 reply-auth shipped) |
| [0020](0020-v1-contract-freeze.md) | Accepted | Freeze the v1 agent-surface contracts |
| [0021](0021-honest-memory-1gb.md) | Accepted | Honest memory to the 1 GB identity window |
| [0022](0022-com2-latency-scheduler-bound.md) | Accepted | COM2 round-trip latency is scheduler-cadence-bound |
| [0023](0023-ipc-peer-rewire-on-rebirth.md) | Accepted | IPC peer re-wiring on watchdog rebirth + dead-target IPC unlink |
| [0024](0024-virtio-core-and-transports.md) | Accepted | virtio-net: one device driver over virtio-pci and virtio-mmio |
| [0025](0025-console-session-reseed.md) | Accepted | Reseed each console session after a snapshot restore |

## Sequencing of the next phases

1. **ADR-0018 (now)** — release + package at 0.x. No contract freeze.
2. **ADR-0019** — authenticate the swarm plane. Extends the COM2/attestation contracts,
   so it must precede any freeze.
3. **ADR-0020** — freeze the v1 contracts, after 0019's wire changes settle and the
   pre-freeze fixes land.
4. **ADR-0021** — honest memory to 1 GB, standalone; also closes two verified latent
   defects (the PMM/kheap reservation gap and the Multiboot2-magic trap).

Deferred (panel verdicts, no ADR yet): UEFI boot (revisit at a real CSM-less target),
a `libagent` extraction (needs a second orchestrator consumer + actor-bearing audit
records), and a block/log filesystem (no agent-native consumer needs partial writes).

## Format

Each ADR follows: a number and title; a date and status; **Context** (the forces),
**Decision** (what was chosen), **Consequences** (positive, negative, and residual risks —
honest negatives are mandatory), and **Evidence** (shipping PRs, `file:line` anchors, and
the CI gates that prove it). New ADRs take the next free number and never renumber existing
ones.
