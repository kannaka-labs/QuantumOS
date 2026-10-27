#!/usr/bin/env python3
"""virtio-rng gate (#254): the entropy device binds, feeds the generator, and is
labelled as host randomness, never quantum.

Three QEMU boots, each piping `qseed` to qsh:
  1. -device virtio-rng-pci, no qseed= : RNG up (pci); "Entropy: virtio-rng (host
     randomness; no QPU attached)"; NOT the unseeded line; qsh qseed reports 0,
     because the device bytes are not a host-handed seed (SYS_QSEED and the swarm
     attestation bind only to what the host handed in).
  2. the device AND qseed= : the handoff line stays, and the device is "mixed in".
  3. no device : no RNG line, and the unseeded line is unchanged.

The "virtio-rng" Entropy line prints only when the driver bound AND an 8-byte
fill completed (quantum_init), so a dead queue or a probe miss turns boot 1 red
(it falls back to the unseeded line). Boot 3 keeps the gate from passing on a
kernel that prints the line unconditionally.

  QOS_KERNEL=build/x86_64/kernel.elf32 python3 scripts/test_qos_rng.py
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

KERNEL = os.environ.get("QOS_KERNEL", "build/x86_64/kernel.elf32")
QSEED = "0123456789ABCDEF"

RNG_UP = "RNG: virtio-rng up (pci)"
RNG_ONLY = "Entropy: virtio-rng (host randomness, not a QPU)"
RNG_MIXED = "Entropy: virtio-rng mixed in (host randomness)"
UNSEEDED = "Entropy: unseeded at boot (timer default, not from a QPU)"
HANDOFF = "Quantum entropy: seeded from boot handoff"


def boot(extra, append=None, seconds=16):
    cmd = ["timeout", f"{seconds}s", "qemu-system-x86_64", "-kernel", KERNEL,
           "-serial", "stdio", "-m", "128M", "-display", "none", "-no-reboot"] + extra
    if append:
        cmd += ["-append", append]
    # qsh reads stdin from t=0; the command waits in the UART until the shell polls.
    p = subprocess.run(cmd, input=b"qseed\n", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.stdout.decode("utf-8", "replace")


def qseed_value(log):
    m = re.search(r"qsh: qseed ([0-9A-Fa-f]{16})", log)
    return m.group(1).upper() if m else None


def entropy_lines(log):
    return [ln.strip() for ln in log.splitlines() if "Entropy:" in ln or "entropy:" in ln]


def fail(msg, log):
    print(f"FAIL: {msg}")
    print("---- entropy lines ----")
    print("\n".join(entropy_lines(log)) or "(none)")
    print("---- console tail ----")
    print(log[-2500:])
    sys.exit(1)


def require_ready(log, what):
    if "QuantumOS ready" not in log:
        fail(f"{what}: kernel did not reach 'QuantumOS ready'", log)


def main():
    dev = ["-device", "virtio-rng-pci"]

    log = boot(dev)
    require_ready(log, "device")
    if RNG_UP not in log:
        fail("device: virtio-rng did not bind over pci", log)
    if RNG_ONLY not in log:
        fail(f"device: missing {RNG_ONLY!r}", log)
    if UNSEEDED in log:
        fail("device: the unseeded line printed although the device fed the generator", log)
    for ln in entropy_lines(log):
        if "virtio-rng" in ln and "quantum" in ln.lower():
            fail(f"device: the virtio-rng line calls itself quantum: {ln!r}", log)
    if qseed_value(log) != "0" * 16:
        fail(f"device: qsh qseed reported {qseed_value(log)}, want 0 (device bytes are not a qseed)", log)
    print(f"OK: virtio-rng bound (pci), seeded the generator, labelled host randomness; qseed=0")

    log = boot(dev, append=f"qseed={QSEED}")
    require_ready(log, "device+qseed")
    if HANDOFF not in log:
        fail("device+qseed: the qseed handoff line is missing", log)
    if RNG_MIXED not in log:
        fail(f"device+qseed: missing {RNG_MIXED!r}", log)
    if qseed_value(log) != QSEED:
        fail(f"device+qseed: qsh qseed reported {qseed_value(log)}, want {QSEED}", log)
    print("OK: with a qseed handoff the handoff keeps its label and virtio-rng is mixed in")

    log = boot([])
    require_ready(log, "no device")
    if "RNG: virtio-rng" in log or "Entropy: virtio-rng" in log:
        fail("no device: a virtio-rng line printed without the device", log)
    if UNSEEDED not in log:
        fail("no device: the unseeded line changed", log)
    print("OK: without the device the entropy lines are unchanged")

    print("=== virtio-rng gate PASSED ===")


if __name__ == "__main__":
    main()
