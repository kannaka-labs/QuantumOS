#!/usr/bin/env python3
"""Stand-in for Firecracker in the console's tests.

Two ways to start, like the real binary:
  --no-api --config-file <cfg>   reads the config (a broken one fails like the real
                                 binary) and boots at once
  --api-sock <path>              serves the API subset the launcher uses on a Unix socket:
                                 PUT /boot-source, /machine-config, /actions InstanceStart,
                                 PATCH /vm Paused, PUT /snapshot/create, PUT /snapshot/load

A boot prints a QuantumOS-like banner naming the kernel's sha256 (first 12 hex) and a
prompt. The guest's "memory" is a dict saved to the snapshot's memory file and read back
on load; like Firecracker's MAP_PRIVATE File backend, a restored guest never writes it
back. A restored guest prints nothing until it gets input.

Commands on stdin, one per line:
  echo <text>     prints <text>
  imprint <text>  stores <text> in guest memory
  recall          prints every stored text
  whoami          prints the kernel sha the memory was booted from
  exit            exits 0 (guest halt)
  crash           exits 3 (guest crash)
  hang            stops reading, keeps running (for the time cap test)
"""
import hashlib
import json
import os
import socket
import sys
import threading
import time

out = sys.stdout.buffer
mem = {"kernel": None, "imprints": []}
started = threading.Event()


def die(msg, code=4):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()
    os._exit(code)


def boot(kernel_path):
    mem["kernel"] = hashlib.sha256(open(kernel_path, "rb").read()).hexdigest()[:12]
    out.write(f"Welcome to QuantumOS (fake) kernel={mem['kernel']}\n".encode())
    out.write(b"\x1b[1;32mqsh> \x1b[0m")
    out.flush()
    started.set()


def serve_api(path):
    cfg = {}
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    while True:
        conn, _ = srv.accept()
        with conn:
            f = conn.makefile("rb")
            method, target, _ = f.readline().decode().split(" ", 2)
            length = 0
            while True:
                line = f.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                k, _, v = line.decode().partition(":")
                if k.strip().lower() == "content-length":
                    length = int(v.strip())
            body = json.loads(f.read(length)) if length else {}
            if target.startswith("/drives") or target.startswith("/network-interfaces"):
                die("console VMs must have no drives or NICs")
            if method == "PUT" and target == "/boot-source":
                cfg["kernel"] = body["kernel_image_path"]
            elif method == "PUT" and target == "/machine-config":
                cfg["machine"] = body
            elif method == "PUT" and target == "/actions" and body.get("action_type") == "InstanceStart":
                boot(cfg["kernel"])
            elif method == "PATCH" and target == "/vm":
                pass
            elif method == "PUT" and target == "/snapshot/create":
                open(body["snapshot_path"], "w").write(json.dumps({"fake": "vmstate"}))
                open(body["mem_file_path"], "w").write(json.dumps(mem))
            elif method == "PUT" and target == "/snapshot/load":
                if body.get("mem_backend", {}).get("backend_type") != "File":
                    die("expected the File memory backend")
                json.load(open(body["snapshot_path"]))
                mem.update(json.load(open(body["mem_backend"]["backend_path"])))  # private copy
                if body.get("resume_vm"):
                    started.set()
            else:
                conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                continue
            conn.sendall(b"HTTP/1.1 204 No Content\r\n\r\n")


args = sys.argv[1:]
if "--api-sock" in args:
    threading.Thread(target=serve_api, args=(args[args.index("--api-sock") + 1],), daemon=True).start()
else:
    cfg = json.load(open(args[args.index("--config-file") + 1]))
    assert cfg["drives"] == [] and "network-interfaces" not in cfg, "console VMs must have no drives or NICs"
    boot(cfg["boot-source"]["kernel_image_path"])

started.wait()
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
    elif line.startswith("imprint "):
        mem["imprints"].append(line[8:])
    elif line == "recall":
        out.write(("recall: " + ",".join(mem["imprints"]) + "\n").encode())
    elif line == "whoami":
        out.write(f"kernel={mem['kernel']}\n".encode())
    out.write(b"qsh> ")
    out.flush()
