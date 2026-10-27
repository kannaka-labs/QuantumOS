#!/usr/bin/env python3
"""Boot build/x86_64/kernel.elf as a Firecracker microVM and require the qsh greeting fast.

Firecracker models COM1 only, and an unhandled port read returns 0 (QEMU returns 0xFF),
so this is the one gate where COM2 is absent in the "reads 0" way. Before com2_init's
scratch-register absence check, the first COM2 write spun its full THR-drain bound in VM
exits with interrupts off and froze the guest ~75 s before qsh could greet. The bound
here fails that regression by a wide margin and passes the fixed kernel (~1.5 s).

The VM uses the public console's shape (tools/qos-console): 1 vCPU, 128 MiB, no drives,
no NIC, `quiet`. Firecracker's own log goes to a file at Error level, so stdout is the
guest's COM1 and nothing else.

  python3 scripts/ci_firecracker_boot.py --firecracker ./firecracker --kernel build/x86_64/kernel.elf
"""
from __future__ import annotations

import argparse
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

GREETING = b"QSH: QuantumOS interactive shell ready"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--firecracker", required=True)
    ap.add_argument("--kernel", default="build/x86_64/kernel.elf")
    ap.add_argument("--max-seconds", type=float, default=10.0,
                    help="fail if qsh has not greeted by then (the regression took ~75 s)")
    a = ap.parse_args()
    # Firecracker runs with cwd set to the temp dir, so a relative path would not resolve.
    a.firecracker = os.path.abspath(a.firecracker)
    a.kernel = os.path.abspath(a.kernel)

    d = tempfile.mkdtemp(prefix="qos-fc-ci-")
    try:
        shutil.copyfile(a.kernel, os.path.join(d, "kernel.elf"))
        cfg = {
            "boot-source": {"kernel_image_path": os.path.join(d, "kernel.elf"), "boot_args": "quiet"},
            "drives": [],
            "machine-config": {"vcpu_count": 1, "mem_size_mib": 128},
        }
        with open(os.path.join(d, "vm.json"), "w") as f:
            json.dump(cfg, f)
        log_path = os.path.join(d, "fc.log")
        open(log_path, "w").close()
        t0 = time.monotonic()
        vm = subprocess.Popen(
            [a.firecracker, "--no-api", "--config-file", os.path.join(d, "vm.json"),
             "--log-path", log_path, "--level", "Error"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=d)
        buf = b""
        greeted_at = None
        deadline = t0 + a.max_seconds
        while time.monotonic() < deadline:
            r, _, _ = select.select([vm.stdout], [], [], 0.25)
            if not r:
                if vm.poll() is not None:
                    break
                continue
            chunk = os.read(vm.stdout.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            if GREETING in buf:
                greeted_at = time.monotonic() - t0
                break
        # prove the shell answers, not only that it printed a banner
        answered = False
        if greeted_at is not None:
            vm.stdin.write(b"help\n")
            vm.stdin.flush()
            end = time.monotonic() + 5
            while time.monotonic() < end and b"qsh commands:" not in buf:
                r, _, _ = select.select([vm.stdout], [], [], 0.25)
                if r:
                    chunk = os.read(vm.stdout.fileno(), 4096)
                    if not chunk:
                        break
                    buf += chunk
            answered = b"qsh commands:" in buf
        vm.kill()
        vm.wait()
        err = vm.stderr.read().decode("utf-8", "replace")[-2000:]
        tail = buf.decode("utf-8", "replace")[-3000:]
        if greeted_at is None:
            print(f"FAIL: no qsh greeting within {a.max_seconds:.0f} s under Firecracker", file=sys.stderr)
            print("---- guest console tail ----\n" + tail, file=sys.stderr)
            if err.strip():
                print("---- firecracker stderr ----\n" + err, file=sys.stderr)
            return 1
        if not answered:
            print(f"FAIL: qsh greeted at {greeted_at:.2f} s but did not answer 'help'", file=sys.stderr)
            print("---- guest console tail ----\n" + tail, file=sys.stderr)
            return 1
        print(f"PASS: qsh greeted {greeted_at:.2f} s after Firecracker start and answered 'help'")
        return 0
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
