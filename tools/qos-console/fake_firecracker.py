#!/usr/bin/env python3
"""Stand-in for Firecracker in the console's tests.

Two ways to start, like the real binary:
  --no-api --config-file <cfg>   reads the config (a broken one fails like the real
                                 binary) and boots at once
  --api-sock <path>              serves the API subset the launcher uses on a Unix socket:
                                 PUT /boot-source, /machine-config, /drives/<id>,
                                 /actions InstanceStart, PATCH /vm Paused,
                                 PUT /snapshot/create, PUT /snapshot/load, PUT /vsock

A vsock device (ADR-0029) listens on its uds_path (relative paths against the cwd, and
recorded in the snapshot as given, like the drive). A host that connects and sends
"CONNECT 1024" gets "OK 1"; from then on the shell's output goes to that connection
instead of stdout, as the kernel does, until the peer closes it.

At most one drive and no NICs. Like Firecracker, a relative drive path is opened against
the process's cwd, and the snapshot records the path as given, so a restore in another
directory opens that directory's file. A boot whose drive starts with QDSK1 restores the
imprints `sync` stored there ("FS: restored persisted files from disk").

A boot prints a QuantumOS-like banner naming the kernel's sha256 (first 12 hex) and a
prompt. The guest's "memory" is a dict saved to the snapshot's memory file and read back
on load; like Firecracker's MAP_PRIVATE File backend, a restored guest never writes it
back. A restored guest prints nothing until it gets input.

Commands on stdin, one per line:
  echo <text>     prints <text>
  spew <n>        prints n bytes of output
  imprint <text>  stores <text> in guest memory
  recall          prints every stored text
  whoami          prints the kernel sha the memory was booted from
  sync            writes QDSK1 + the imprints to the drive ("qsh: sync ok"), or
                  "qsh: sync failed (no disk)"
  qseed           prints the session seeds received (ESC ] qseed ; <hex> BEL, stripped
                  from input like the kernel does) and whether the first came before
                  any other input
  exit            exits 0 (guest halt)
  crash           exits 3 (guest crash)
  hang            stops reading, keeps running (for the time cap test)
"""
import hashlib
import json
import os
import re
import socket
import sys
import threading
import time

class Out:
    """The shell's output: the vsock peer while one is attached, else COM1 (stdout)."""

    def __init__(self):
        self.peer = None

    def write(self, data):
        peer = self.peer
        if peer is not None:
            try:
                peer.sendall(data)
                return
            except OSError:
                self.peer = None
        sys.stdout.buffer.write(data)

    def flush(self):
        sys.stdout.buffer.flush()


out = Out()
mem = {"kernel": None, "imprints": [], "drive": None, "vsock": None}   # device state
started = threading.Event()
session = {"seeds": [], "input_before_seed": False}   # not guest memory: never snapshotted
RESEED = re.compile(rb"\x1b\]qseed;([0-9a-fA-F]{16})\x07")
QRES = re.compile(rb"\x1b\]qres;([^\x07]*)\x07")


def die(msg, code=4):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()
    os._exit(code)


QDSK = b"QDSK1"


def vsock_listen(path):
    """Serve the vsock device's host side: CONNECT 1024 -> OK, then shell output."""
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(2)

    def serve():
        while True:
            conn, _ = srv.accept()
            line = b""
            while not line.endswith(b"\n"):
                c = conn.recv(1)
                if not c:
                    break
                line += c
            if line != b"CONNECT 1024\n":
                conn.close()
                continue
            conn.sendall(b"OK 1\n")
            out.peer = conn
            threading.Thread(target=watch, args=(conn,), daemon=True).start()

    def watch(conn):
        while conn.recv(4096):   # the console sends nothing over vsock; EOF is the peer leaving
            pass
        if out.peer is conn:
            out.peer = None

    threading.Thread(target=serve, daemon=True).start()


def boot(kernel_path):
    mem["kernel"] = hashlib.sha256(open(kernel_path, "rb").read()).hexdigest()[:12]
    out.write(f"Welcome to QuantumOS (fake) kernel={mem['kernel']}\n".encode())
    if mem["drive"]:
        raw = open(mem["drive"], "rb").read()
        if raw.startswith(QDSK):
            mem["imprints"] = json.loads(raw[len(QDSK):].rstrip(b"\0") or b"[]")
            out.write(b"FS: restored persisted files from disk\n")
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
            if target.startswith("/network-interfaces"):
                die("console VMs must have no NICs")
            if method == "PUT" and target.startswith("/drives/"):
                if mem["drive"] is not None:
                    die("at most one drive")
                if not os.path.exists(body["path_on_host"]):
                    die("drive file missing")
                mem["drive"] = body["path_on_host"]
            elif method == "PUT" and target == "/vsock":
                mem["vsock"] = body["uds_path"]
            elif method == "PUT" and target == "/boot-source":
                cfg["kernel"] = body["kernel_image_path"]
            elif method == "PUT" and target == "/machine-config":
                cfg["machine"] = body
            elif method == "PUT" and target == "/actions" and body.get("action_type") == "InstanceStart":
                if mem["vsock"]:
                    vsock_listen(mem["vsock"])
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
                if mem["drive"] and not os.path.exists(mem["drive"]):
                    die("snapshot drive missing at load")
                if mem["vsock"]:
                    vsock_listen(mem["vsock"])
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
    assert len(cfg["drives"]) <= 1 and "network-interfaces" not in cfg, "console VMs: one drive at most, no NICs"
    if cfg["drives"]:
        mem["drive"] = cfg["drives"][0]["path_on_host"]
    if cfg.get("vsock"):
        mem["vsock"] = cfg["vsock"]["uds_path"]
        vsock_listen(mem["vsock"])
    boot(cfg["boot-source"]["kernel_image_path"])

started.wait()
for raw in sys.stdin.buffer:
    for m in RESEED.finditer(raw):
        session["seeds"].append(m.group(1).decode().lower())
    if RESEED.match(raw) is None and not session["seeds"]:
        session["input_before_seed"] = True
    for m in QRES.finditer(raw):   # an answer from the qpu relay (#262)
        out.write(b"answer: " + m.group(1) + b"\n")
    line = QRES.sub(b"", RESEED.sub(b"", raw)).decode(errors="replace").strip()
    if line == "exit":
        sys.exit(0)
    if line == "crash":
        sys.exit(3)
    if line == "hang":
        while True:
            time.sleep(1)
    if line.startswith("spew "):       # a program that prints a lot (output cap test)
        n = int(line[5:])
        while n > 0:
            chunk = min(n, 4096)
            out.write(b"x" * (chunk - 1) + b"\n")
            out.flush()
            n -= chunk
    elif line.startswith("echo "):
        out.write(line[5:].encode() + b"\n")
    elif line.startswith("imprint "):
        mem["imprints"].append(line[8:])
    elif line == "recall":
        out.write(("recall: " + ",".join(mem["imprints"]) + "\n").encode())
    elif line == "qseed":
        out.write(("qseed: " + ",".join(session["seeds"]) +
                   f" late={session['input_before_seed']}\n").encode())
    elif line.startswith("qpu "):      # emit a raw request, as qsh's `qpu` does
        out.write(b"\x1b]qpu;" + line[4:].encode() + b"\x07")
    elif line == "forge":              # a guest trying to show the visitor a fake answer
        out.write(b"before\x1b]qres;1;D;q0123456789;QPU fake\x07after\n")
    elif line == "sync":
        if mem["drive"]:
            with open(mem["drive"], "r+b") as f:
                f.write(QDSK + json.dumps(mem["imprints"]).encode())
            out.write(b"qsh: sync ok (1 files flushed)\n")
        else:
            out.write(b"qsh: sync failed (no disk)\n")
    elif line == "whoami":
        out.write(f"kernel={mem['kernel']}\n".encode())
    out.write(b"qsh> ")
    out.flush()
