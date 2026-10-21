# What we'd build on qBraid OS

*Kannaka Labs, 2026-10-21. One page. "Exists" means running and in a repo today; "would build" means not yet started.*

qBraid OS (announced 2026-10-21) packages qBraid Runtime (scheduling, metering, access) and qBraid Gateway (the hardware connection) for operators who run quantum processors, alongside NVIDIA GPUs through CUDA-Q. We are an existing qBraid user (Lab CPU hours, BMA GPU pods, QPU jobs through qBraid and OpenQuantum). We want developer or sandbox access so our stack works with qBraid OS as it reaches developers and operators.

## What exists today

| Piece | What it is | Evidence |
|---|---|---|
| **QuantumOS** | A capability-secured x86-64 microkernel with an exact Gaussian-integer state-vector engine (`qsv`) in ring 3, an authority ledger, an MCP server exposing the kernel to agents, and real QPU entropy at boot (a Rigetti circuit run through qBraid hands measured bits to the kernel). Every claim is gated in CI. | this repo, `make ci-smoke` |
| **kannaka-quantum** | QPU labs with a spend guard, runnable on qBraid and OpenQuantum. Includes a quantum amplitude estimation (QAE) lab (#44) used for a neutron-transport warm-up. | kannaka-labs/kannaka-quantum |
| **Kannaka Scientist** | Pre-registered experiments filed on a public research ledger before data. One campaign ran a QAE hardware ladder on IQM Garnet (k = 0, 1, 2, 4 Grover steps): amplification is visible on the device but damped by noise from k = 1, and the noiseless estimator degrades as a result. | research.spacechild.love, campaign `ks-quantum-transport-warmup` |
| **kannaka-crystal** | Circuit-motif mining. On the ECDSA.fail benchmark's Toffoli circuit, one 4-gate ripple-carry motif covers 76.3% of Toffolis. | kannaka-crystal PR #11 |
| **kannaka-memory** | Persistent associative memory for agents (wave-interference store), with MCP and NATS interfaces. | kannaka-labs/kannaka-memory |
| **kannaka-bench** | Reproducible memory benchmarks (LongMemEval, LoCoMo) with paired statistics. | kannaka-bench, `docs/results-2026-10.md` |
| **kannaka-tui** | A terminal console over the Kannaka stack. | kannaka-labs/kannaka-tui |

## What we would build on qBraid OS (not started)

1. **QuantumOS as an operator and agent console.** A ring-3 service that talks to qBraid OS Runtime: list devices, reservations and metered usage, submit jobs under capability-checked authority, and record every grant and spend in the kernel's authority ledger. That gives agents qBraid OS's "agent-run operations within operator permissions" with a kernel-enforced audit trail.
2. **kannaka-quantum labs as acceptance tests for newly connected QPUs.** Small, priced, pre-registered circuits (Bell, decay, QAE ladders) that an operator can run when a system joins qBraid OS, giving a reproducible first-day characterization filed with hashes.
3. **Pre-registered hardware ladders as an in-job decoding benchmark.** Our Garnet result is exactly where CUDA-Q's in-job error-correction decoding should help. Running the same pre-registered ladder on a qBraid OS system with decoding on and off would measure whether amplification survives to deeper k. That's a concrete, falsifiable test of the qBraid OS + CUDA-Q value proposition, published with its data.
4. **kannaka-crystal as an optimizer plug-in** through the defined integration points for compilers and optimizers: motif detection and substitution on submitted circuits, reported as gate-count deltas.
5. **kannaka-memory as agents' persistent state** across scheduling sessions, and kannaka-bench-style measurement for any agent tooling we ship.

## What we're asking for

Developer or sandbox access to qBraid OS (Runtime and Gateway APIs, or a simulator-backed operator sandbox), the integration-point docs (docs.qbraid.com/gateway currently returns 404), and any early-access, research or partner program we fit. We will publish what we build and measure, and file results on our public ledger.

Contact: Nick Flach (Kannaka Labs), nflach78@gmail.com · Kannaka, kannaka@spacechild.love
