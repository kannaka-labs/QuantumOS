#!/usr/bin/env python3
"""The vsock shell channel under Firecracker (ADR-0029, #259).

Boots kernel.elf as a Firecracker microVM in the public console's shape plus a vsock
device, waits for qsh on COM1, then:

  1. connects through the device's Unix socket (`CONNECT 1024`), sends `help` over vsock
     and times it to the next prompt (best of --runs, and the p50);
  2. checks the shell's output arrived on vsock and NOT on COM1 (the ADR's routing), while
     COM1 still carries the kernel log;
  3. closes the connection and checks `help` typed on COM1 answers on COM1 again.

With --expect-no-vsock it boots WITHOUT the device and requires the connect to fail. That
is the negative control: the gate cannot pass against a kernel or VM with no vsock.

  python3 scripts/ci_firecracker_vsock.py --firecracker ./firecracker --kernel build/x86_64/kernel.elf
"""
from __future__ import annotations

import argparse
import json
import os
import re
import select
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time

GREETING = b"QSH: QuantumOS interactive shell ready"
HELP_END = b"qsh commands:"
PROMPT = re.compile(rb"qsh(?:\x1b\[[0-9;]*m)?> ")
PORT = 1024


class Com1:
    def __init__(self, proc):
        self.proc = proc
        self.buf = b""

    def pump(self, timeout=0.0):
        r, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if r:
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            self.buf += chunk
            return len(chunk)
        return 0

    def wait_for(self, needle, start, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.buf.find(needle, start) >= 0:
                return True
            self.pump(0.05)
        return self.buf.find(needle, start) >= 0


def vsock_connect(uds, timeout=5.0):
    """Host-initiated connect (Firecracker's protocol): CONNECT <port>, expect OK."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.settimeout(2.0)
            s.connect(uds)
            s.sendall(f"CONNECT {PORT}\n".encode())
            line = b""
            while not line.endswith(b"\n"):
                c = s.recv(1)
                if not c:
                    break
                line += c
            if line.startswith(b"OK "):
                s.settimeout(None)
                return s
            last = line
        except OSError as e:
            last = e
        s.close()
        time.sleep(0.1)
    raise ConnectionError(f"vsock connect failed: {last!r}")


def vsock_read_until(s, buf, start, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        m = PROMPT.search(buf, start) if buf.find(HELP_END, start) >= 0 else None
        if m:
            return buf, True
        r, _, _ = select.select([s], [], [], 0.05)
        if r:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return buf, False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--firecracker", required=True)
    ap.add_argument("--kernel", default="build/x86_64/kernel.elf")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--max-ms", type=float, default=300.0,
                    help="fail if the p50 help->prompt over vsock exceeds this")
    ap.add_argument("--expect-no-vsock", action="store_true",
                    help="negative control: boot without the device; the connect must fail")
    a = ap.parse_args()

    d = tempfile.mkdtemp(prefix="qos-fc-vsock-")
    cfg = {
        "boot-source": {"kernel_image_path": os.path.join(d, "kernel.elf"), "boot_args": "quiet"},
        "drives": [],
        "machine-config": {"vcpu_count": 1, "mem_size_mib": 128},
    }
    if not a.expect_no_vsock:
        cfg["vsock"] = {"guest_cid": 3, "uds_path": "v.sock"}  # relative: the VM's own dir
    shutil.copyfile(os.path.abspath(a.kernel), os.path.join(d, "kernel.elf"))
    with open(os.path.join(d, "vm.json"), "w") as f:
        json.dump(cfg, f)
    open(os.path.join(d, "fc.log"), "w").close()
    vm = subprocess.Popen(
        [os.path.abspath(a.firecracker), "--no-api", "--config-file", "vm.json",
         "--log-path", "fc.log", "--level", "Error"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=d)
    com1 = Com1(vm)
    try:
        if not com1.wait_for(GREETING, 0, 15):
            print("FAIL: no qsh greeting on COM1")
            print(com1.buf.decode("utf-8", "replace")[-2000:])
            return 1
        com1.wait_for(b"qsh", com1.buf.find(GREETING) + len(GREETING), 3)
        time.sleep(1.0)
        while com1.pump(0.1):
            pass

        uds = os.path.join(d, "v.sock")
        if a.expect_no_vsock:
            try:
                vsock_connect(uds, timeout=2.0).close()
            except ConnectionError:
                print("PASS (negative control): no vsock device, the connect fails")
                return 0
            print("FAIL (negative control): connected although the VM has no vsock device")
            return 1

        if b"VSOCK: virtio-vsock up (mmio)" not in com1.buf:
            print("FAIL: the kernel did not bind the vsock device")
            print(com1.buf.decode("utf-8", "replace")[-2000:])
            return 1
        s = vsock_connect(uds)
        com1_mark = len(com1.buf)
        buf = b""
        times = []
        for _ in range(a.runs):
            start = len(buf)
            t0 = time.monotonic()
            s.sendall(b"help\n")
            buf, ok = vsock_read_until(s, buf, start, 10)
            if not ok:
                print("FAIL: help over vsock never reached the prompt")
                print(buf.decode("utf-8", "replace")[-1500:])
                return 1
            times.append((time.monotonic() - t0) * 1000)
            time.sleep(0.1)
        com1.pump(0.3)
        leaked = com1.buf[com1_mark:].count(HELP_END)
        p50 = statistics.median(times)
        print(f"vsock: help->prompt ms p50={p50:.0f} min={min(times):.0f} max={max(times):.0f} "
              f"runs={a.runs}; help output on COM1 meanwhile: {leaked}")
        if leaked:
            print("FAIL: shell output went to COM1 while a vsock peer was attached")
            return 1
        if p50 > a.max_ms:
            print(f"FAIL: p50 {p50:.0f} ms over vsock exceeds {a.max_ms:.0f} ms")
            return 1

        s.close()
        time.sleep(0.5)  # the guest sees the peer go away at its next poll
        mark = len(com1.buf)
        vm.stdin.write(b"help\n")
        vm.stdin.flush()
        if not com1.wait_for(HELP_END, mark, 15):
            print("FAIL: after the vsock peer left, help on COM1 did not answer on COM1")
            return 1
        print("PASS: shell over vsock (not COM1) while attached, back on COM1 after")
        return 0
    finally:
        vm.kill()
        vm.wait()
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
