#!/usr/bin/env python3
"""Stand-in for `firecracker --no-api --config-file <cfg>` in the console's tests.

Reads the config (so a broken one fails like the real binary), prints a QuantumOS-like
banner and prompt on stdout, and answers line by line on stdin:
  echo <text>  -> prints <text>
  exit         -> exits 0 (guest halt)
  crash        -> exits 3 (guest crash)
  hang         -> stops reading, keeps running (for the time cap test)
"""
import json
import sys
import time

cfg = json.load(open(sys.argv[sys.argv.index("--config-file") + 1]))
assert cfg["drives"] == [] and "network-interfaces" not in cfg, "console VMs must have no drives or NICs"
out = sys.stdout.buffer
out.write(b"Welcome to QuantumOS (fake)\nqsh> ")
out.flush()
for raw in sys.stdin.buffer:
    line = raw.decode(errors="replace").strip()
    if line == "exit":
        sys.exit(0)
    if line == "crash":
        sys.exit(3)
    if line == "hang":
        while True:
            time.sleep(1)
    if line.startswith("echo "):
        out.write(line[5:].encode() + b"\n")
    out.write(b"qsh> ")
    out.flush()
