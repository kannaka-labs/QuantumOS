#!/usr/bin/env python3
"""Console output benchmark (#259): time from sending `help` to the next qsh prompt.

Boots the kernel once under Firecracker (the public console's VM shape, as in
ci_firecracker_boot.py) or QEMU TCG, waits for the qsh greeting, then sends
`help` --runs times and reports the time until the prompt that follows its
output, plus the number of bytes that came back.

  python3 scripts/bench_console_output.py --firecracker /usr/local/bin/firecracker
  python3 scripts/bench_console_output.py --qemu
"""
from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

GREETING = b"QSH: QuantumOS interactive shell ready"
HELP_END = b"qsh commands:"
# qsh colours its prompt: "qsh" ESC[90m "> "
PROMPT = re.compile(rb"qsh(?:\x1b\[[0-9;]*m)?> ")


def _find(buf, needle, start):
    if isinstance(needle, bytes):
        return buf.find(needle, start)
    m = needle.search(buf, start)
    return m.start() if m else -1


def _read_until(vm, buf, needle, start, timeout):
    """Read guest output until `needle` appears at or after offset `start`."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if _find(buf, needle, start) >= 0:
            return buf, True
        r, _, _ = select.select([vm.stdout], [], [], 0.05)
        if r:
            chunk = os.read(vm.stdout.fileno(), 65536)
            if not chunk:
                break
            buf += chunk
    return buf, _find(buf, needle, start) >= 0


def _boot(a, d):
    if a.qemu:
        kernel = os.path.abspath(a.kernel32)
        return subprocess.Popen(
            ["qemu-system-x86_64", "-kernel", kernel, "-append", "quiet", "-serial", "stdio",
             "-m", "128M", "-display", "none", "-no-reboot"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
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
    return subprocess.Popen(
        [os.path.abspath(a.firecracker), "--no-api", "--config-file", os.path.join(d, "vm.json"),
         "--log-path", log_path, "--level", "Error"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=d)


def main() -> int:
    ap = argparse.ArgumentParser()
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--firecracker", help="path to the firecracker binary")
    mode.add_argument("--qemu", action="store_true", help="boot under QEMU TCG instead")
    ap.add_argument("--kernel", default="build/x86_64/kernel.elf", help="PVH ELF for Firecracker")
    ap.add_argument("--kernel32", default="build/x86_64/kernel.elf32", help="multiboot ELF for QEMU")
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    d = tempfile.mkdtemp(prefix="qos-bench-")
    vm = _boot(a, d)
    try:
        buf, ok = _read_until(vm, b"", GREETING, 0, 60)
        if not ok:
            print("FAIL: no qsh greeting", file=sys.stderr)
            return 1
        buf, _ = _read_until(vm, buf, PROMPT, buf.find(GREETING), 10)
        time.sleep(1.0)  # let boot services finish printing before timing
        r, _, _ = select.select([vm.stdout], [], [], 0)
        if r:
            buf += os.read(vm.stdout.fileno(), 65536)

        times, sizes = [], []
        for _ in range(a.runs):
            start = len(buf)
            t0 = time.monotonic()
            vm.stdin.write(b"help\n")
            vm.stdin.flush()
            buf, ok = _read_until(vm, buf, HELP_END, start, 30)
            if ok:
                buf, ok = _read_until(vm, buf, PROMPT, buf.find(HELP_END, start), 30)
            if not ok:
                print("FAIL: help output or prompt never arrived", file=sys.stderr)
                return 1
            times.append(time.monotonic() - t0)
            sizes.append(_find(buf, PROMPT, buf.find(HELP_END, start)) - start)
            time.sleep(0.2)

        ms = [t * 1000 for t in times]
        name = a.label or ("qemu-tcg" if a.qemu else "firecracker")
        print(f"{name}: help->prompt ms p50={statistics.median(ms):.0f} "
              f"min={min(ms):.0f} max={max(ms):.0f} bytes={statistics.median(sizes):.0f} "
              f"ms/byte={statistics.median(ms) / statistics.median(sizes):.3f} runs={a.runs}")
        return 0
    finally:
        vm.kill()
        vm.wait()
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
