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
```

Requires Python ≥ 3.10 and a `qemu-system-x86_64` on `PATH`. You also need a QuantumOS
`kernel.elf32` (build it from the repo with `make`, or take one from a
[release](https://github.com/kannaka-labs/QuantumOS/releases)).

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

## Honesty note

The boot attestation is *verified ≠ live*: the signature covers the boot string, not
the liveness of any later reply. Authenticating per-reply and the society wire is
planned work (see the repo's ADR-0019/0020). This is alpha software; the tool
contracts are **not frozen** below 1.0.

## License

GPL-2.0-only, matching QuantumOS.
