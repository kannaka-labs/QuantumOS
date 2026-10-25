# quantumos-host-tools

Host-side toolkit for [QuantumOS](https://github.com/kannaka-labs/QuantumOS) — the
capability-secure, quantum-aware microkernel that boots
[in your browser](https://kannaka-labs.github.io/QuantumOS/).

This package is how an **agent operates a running QuantumOS from the host**: it owns
the QEMU VM lifecycle, scripts the guest shell, drives the kernel holographic-memory
field, and — critically — carries a **verified boot identity** on every result.

## Install

```bash
pip install quantumos-host-tools
# quantum-gateway mirror (PennyLane) and qBraid entropy handoff are optional:
pip install "quantumos-host-tools[quantum,qbraid]"
# the provider broker's OCI plugin (buckets, Always Free VMs):
pip install "quantumos-host-tools[oci]"
```

Requires Python ≥ 3.10 and a `qemu-system-x86_64` on `PATH`. You also need a QuantumOS
`kernel.elf32` (build it from the repo with `make`, or take one from a
[release](https://github.com/kannaka-labs/QuantumOS/releases) and check it against `SHA256SUMS.kernel`).
Point the tools at it with `QOS_KERNEL=/path/to/kernel.elf32`: an installed package has no build
directory to find one in, and the society tools take no kernel argument.

## What's inside

- **`qos-mcp`** — an MCP server exposing a running QuantumOS to agents as tools
  (boot, qsh scripting, field imprint/recall, fs, fetch, quantum entropy, N-VM
  societies). Every tool result carries the Lamport-verified boot identity.
- **`qos_bridge`** — the VM bridge (`QosVM`, `QosSociety`). Stdlib-only *by contract*:
  it never imports `mcp`. COM1 carries shell scripting; COM2 carries the attested data
  channel.
- **`qos-verify-attestation`** — stand-alone verifier for a QuantumOS boot
  attestation stream (integer SHA-256 Lamport one-time signature).
- **`qos-qbraid-boot`** — draw real-QPU entropy (via qBraid) and hand it to the kernel
  as a `qseed=` boot token.

### For agents that act on your behalf (`quantumos_agent`)

- **`qos-broker`** — the provider broker (ADR-0026). It quotes, provisions and tears down
  real resources (OCI buckets and Always Free VMs, qBraid jobs) on leases, inside a
  budget you set and the agent cannot raise. `qos-broker envelope` sets the budget and
  shows your approver key once, `qos-broker approve <id>` approves a request, and
  `qos-broker setup oci` prints the least-privilege OCI setup. Real spending is off until
  you start it with `--allow-spend`.
- **`qos-broker-mcp`** — the broker's MCP tools for an agent: quote, provision, status,
  teardown, budget, ledger. None of them can approve or raise the budget.
- **`qos-hub`** — one MCP endpoint in front of the ecosystem's MCP servers and CLIs
  (ADR-0027), with `allow` / `ask` / `deny` per tool. An `ask` call is stored exactly and
  runs only after you approve it with the same key. `qos-hub example` prints a starting
  config.

```bash
qos-broker envelope --db broker.db --per-day 2 --per-action 1 --classes storage,compute --max-lease 24h
qos-hub example > hub.json          # edit the paths and policies
claude mcp add quantumos -- qos-hub serve --config $PWD/hub.json --db $PWD/broker.db
```

## Honesty note

The boot attestation is *verified ≠ live*: the signature covers the boot string, not
the liveness of any later reply. Authenticating per-reply and the society wire is
planned work (see the repo's ADR-0019/0020). This is alpha software; the tool
contracts are **not frozen** below 1.0.

## License

GPL-2.0-only, matching QuantumOS.
