# QuantumOS × Claude — a one-pager

*A capability-secured microkernel with a wave-interference memory field, whose
entire agent surface is a version-frozen MCP contract — now driveable by Claude,
bring-your-own-key.*

---

## What QuantumOS is

QuantumOS is a from-scratch x86-64 microkernel (C, no libc, integer-only in ring 3)
built around three ideas that don't usually appear together:

- **Capability security as the *only* authority model.** Every ring-3 citizen holds
  explicit, unforgeable capabilities; there is no ambient authority. A process can
  drive the console, or draw quantum entropy, or talk to a peer over IPC, only if it
  holds the cap — and nothing else.
- **A holographic associative-memory field in the kernel.** `ghostd` is a
  Hopfield–Kuramoto field of 256 phase oscillators. "Remembering" is attractor
  relaxation: you imprint patterns, then recall from a *corrupted* probe and the
  field settles into the nearest stored basin. The kernel exposes this as syscalls.
- **Multi-node "field societies."** Three or four VMs couple their fields over the
  wire, mean-field-synchronize (a measurable order parameter R_x), and exchange
  qseed-salted aggregates — cross-node work, not just a shared clock. The coupling
  wire is authenticated with a host-admitted group session key (Lamport-signed boot
  attestation + per-frame HMAC).

It boots on QEMU and on real hardware, runs an interactive shell, a filesystem, a
TCP/IP stack, an in-OS quantum-circuit broker, and an agent-native surface. Every
increment ships behind an **anti-vacuous CI gate** — a test that greps a real
runtime signal and is revert-confirmed to fail when the feature is removed.

## Why the Claude fit is unusual

Most "LLM + system" demos wrap a shell tool around a model. QuantumOS is the
inverse: the integration surface was **frozen as a contract before any model
touched it.** ADR-0020 pins the 22-tool MCP schema in a committed golden file
(`contracts/mcp/v1-tools.json`) with a teeth-checked CI gate — the exact surface
an external agent consumes, versioned like an ABI.

Claude is that external consumer. `scripts/qos_claude_agent.py` reuses the frozen
MCP server verbatim (via the Anthropic SDK's MCP conversion helpers + tool runner),
so Claude drives the *same* tools CI diffs against the golden — one source of truth,
zero duplication. The model plans a field-society or associative-memory experiment,
runs it against a live VM, reads the real results, and iterates.

Two properties make this more than a toy:

1. **Honesty is enforced at the surface, not the prompt.** A tool result's
   `verified` flag means the boot attestation is internally consistent (frames +
   Lamport signature + qseed) — *not* that the VM is live at read time (a captured
   attestation replays). Only a key-admitted STATUS reply is per-request
   nonce+HMAC fresh. The agent's system prompt carries this distinction, so Claude
   reports what a result *actually* proves rather than overstating it.
2. **The blast radius is a capability, not a machine.** Because authority is
   capability-scoped, "what can the agent do" is a precise, auditable set — the
   kernel's authority ledger records every grant, denial, and revocation.

## How it works (all local, BYO key)

```
Claude (Anthropic API — your key)
   │  tool_runner drives the agentic loop
   ▼
anthropic MCP conversion helpers
   │  stdio
   ▼
scripts/qos_mcp.py  (FastMCP, unmodified — the frozen 22-tool surface)
   │  thin delegation
   ▼
QosVM / QosSociety  →  QEMU  →  QuantumOS
```

The key never leaves your machine; the VM never leaves your machine. Nothing is
hosted.

```bash
make kernel
export ANTHROPIC_API_KEY=sk-ant-...            # or: ant auth login
pip install -r requirements-claude-agent.txt
make qos-claude EXPERIMENT=society             # or: TASK="…your own experiment…"
make qos-claude-list                           # lists all 22 tools — no key, no API call
```

Curated presets (`--experiment`): `recall` (imprint → corrupt → recall, read R),
`society` (boot 3 nodes, sync, exchange aggregates), `quantum` (draw entropy, run a
Bell pair on the in-OS QPU), `explore` (Claude forms its own hypothesis and tests it).
Guardrails for a BYO key: a turn cap, and guaranteed VM shutdown on any exit path.

## What we're asking

Nothing gated. This is built and runnable today. We'd value:

- **A look, and a reaction** — is "Claude as an OS-experiment scientist over a frozen
  capability surface" interesting to the people building the agent platform?
- **Pointers**, if any, on the MCP integration shape (we reuse the stdio server via
  the SDK's conversion helpers rather than the URL connector — happy to hear the
  intended pattern for a local server like this).
- If there's appetite, a **conversation** about the science angle: wave-interference
  memory, multi-node field coherence, and agents that run reproducible experiments
  against a system whose authority is fully auditable.

**Repo:** github.com/kannaka-labs/QuantumOS · **Agent:** `scripts/qos_claude_agent.py`
· **Frozen surface:** `contracts/mcp/v1-tools.json` · **Design record:** `docs/adr/`

*Honest scope note: QuantumOS is a research OS, not production infrastructure. The
attestation is a boot-consistency proof, not a liveness proof — this document and
the agent both say so plainly. Claims here are limited to what the CI gates
demonstrate.*
