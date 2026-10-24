#!/usr/bin/env python3
"""QuantumOS public console: one throwaway Firecracker microVM per visitor.

Each WebSocket session gets its own microVM (docs/FIRECRACKER.md: PVH entry, serial
console on Firecracker's stdio). The VM has no network interface; its serial console is
the only I/O, and it is bridged to the WebSocket byte for byte. The VM and its directory
are deleted when the visitor disconnects, when the hard time cap runs out, or when the
guest exits.

Saved machines (#256, with --save-dir): every VM also gets a small blank virtio-blk
disk (--save-mib) in its own directory. A visitor who sends {"type": "keep"} gets a
resume token, shown once; whatever they `sync` inside QuantumOS is kept when the session
ends (saves.py: retention, caps, what is stored). A later session opened with
/ws?resume=1 sends {"type": "resume", "token": ...} as its first frame (never in the URL,
so no proxy log holds it) and cold-boots with that disk, so its files, field and audit
ledger come back. Without --save-dir, VMs have no drives at all, as before.

Fast start (--snapshot auto, the default): at startup the launcher boots one VM to the
shell prompt, pauses it and writes a Firecracker snapshot (vmstate + guest memory) under
the run dir. A visitor's VM is then restored from that snapshot in tens of milliseconds
instead of booting, and the visitor is sent the boot transcript the snapshot VM printed.
The memory file is mapped copy-on-write (MAP_PRIVATE) by every restored VM, so visitors
share its clean pages and each one's writes stay private to its own VM. The snapshot is
rebuilt when the kernel image changes; until a snapshot for the current kernel exists,
sessions cold-boot as before, and a failed restore falls back to a cold boot.

Wire protocol (one WebSocket, path /ws):
  server -> client  binary frames: raw bytes from the guest's COM1
  server -> client  text frames:   JSON control ({"type": "hello"|"expired"|"idle"|"output_cap"|
                                   "refused"|"exited", ...})
  client -> server  binary frames: keystrokes for COM1 (max 1 KiB per frame); text frames
                   are keystrokes too, except the JSON control messages {"type": "keep"}
                   and (first frame of a /ws?resume=1 session) {"type": "resume", ...}

Caps (#257): --max-vms concurrent VMs in total, --per-ip per client address (taken
from X-Real-IP, which only the fronting nginx sets; the launcher is reachable
only through a Unix socket that nginx reaches over an SSH tunnel), --max-seconds per
session, --idle-seconds with no input and no output (a warning line goes to the terminal
--idle-warn-seconds before), --input-frames-per-s and --input-bytes-per-s per session
(excess input is dropped with one notice, never a disconnect), and --max-output-bytes
per session. When every VM is in use the visitor is refused with the number of machines
and a retry-after: the time until the soonest running session reaches its hard cap, an
upper bound since idle sessions end sooner. There is no queue, so nothing is held open
waiting. A TCP listen address other than loopback is refused unless
--trust-forwarded-for says the listener really is behind a proxy that sets X-Real-IP.

The log records session start, end, duration, reason and a salted hash of the
client address. It never records anything the visitor types or the guest prints.

Metrics (#258) are counts and timings only. They are kept in memory and, with
--state-file, in a small JSON file. GET /metrics answers in Prometheus text format and
GET /stats in JSON (the public site proxies /stats; /metrics answers only on the
launcher's own socket). The page reports two events with GET /beacon?e=load|boot.
With --calibration-file, GET /calibration.json and GET /calibration serve the daily QPU
calibration series (#275) read-only, as JSON and as a plain-text table.
Unique visitors per UTC day are counted from an HMAC of the address under a salt that
lives only in memory and is replaced every UTC day. Those hashes are never written
anywhere and are dropped when the day ends.
Our own traffic (ad captures, smoke tests, demos) carries the token from
--internal-token-file, as an X-QOS-Internal header or an ?internal= query parameter on
/ws and /beacon. Such sessions and beacons count only as internal_sessions and
internal_beacons, never in the visitor funnel, so /stats shows outside visitors alone.
The token is compared in constant time and never logged.

  qos_console.py --kernel /opt/qos-console/kernel.elf --run-dir /run/qos-console \
                 --listen-unix /run/qos-console/ws.sock
"""
from __future__ import annotations

import argparse
import asyncio
import collections
import contextlib
import datetime
import hashlib
import hmac
import http
import json
import logging
import os
import re
import secrets
import shutil
import signal
import sys
import time
import uuid
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import ServerConnection, serve, unix_serve
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

from qpu_relay import QpuRelay
from saves import SaveRefused, SaveStore

log = logging.getLogger("qos-console")

MAX_INPUT = 1024          # bytes per client frame
READ_CHUNK = 4096         # bytes per guest read
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
SAVE_EVERY_S = 30         # how often changed metrics are written to --state-file
SNAPSHOT_RETRY_S = 300    # after a failed snapshot build, cold-boot this long before retrying
SAVE_SWEEP_S = 3600       # how often expired saved machines are deleted
RESUME_WAIT_S = 10        # a /ws?resume=1 session must send its token this soon
DISK = "disk.img"         # the VM's disk, relative to its own directory (and Firecracker's cwd)
LOOPBACK = ("127.0.0.1", "::1", "[::1]", "localhost")

IDLE_WARNING = "\r\n\x1b[33m[console] no activity: this machine closes in {s} s unless you type something\x1b[0m\r\n"
INPUT_NOTICE = ("\r\n\x1b[33m[console] input is arriving faster than this console accepts; "
                "some of it was dropped\x1b[0m\r\n")


class TokenBucket:
    """A rate with one second of burst. take() is False when the cost does not fit."""

    def __init__(self, rate: float, clock=time.monotonic):
        self.rate = float(rate)
        self.level = float(rate)
        self.clock = clock
        self.t = clock()

    def take(self, cost: float = 1.0) -> bool:
        now = self.clock()
        self.level = min(self.rate, self.level + (now - self.t) * self.rate)
        self.t = now
        if cost > self.level:
            return False
        self.level -= cost
        return True


class FirecrackerError(RuntimeError):
    pass


def end_reason_of(done: set[asyncio.Future]) -> str:
    """Why a session ended, from its finished bridge tasks.

    Both directions can finish in the same tick (the visitor closes while the guest is
    printing). EVERY finished task's outcome is collected here, so none is left as an
    unretrieved exception (seen live: "Task exception was never retrieved" with
    ConnectionClosedOK). The guest exiting takes precedence over a disconnect."""
    reasons = []
    for t in done:
        try:
            reasons.append(t.result())
        except ConnectionClosed:
            reasons.append("disconnect")
        except Exception as e:  # noqa: BLE001 - recorded as the reason
            reasons.append(f"error:{type(e).__name__}")
    if "guest-exited" in reasons:
        return "guest-exited"
    return sorted(reasons)[0] if reasons else "disconnect"


async def fc_api(sock: Path, method: str, path: str, body: dict | None = None, timeout: float = 10) -> None:
    """One request to a Firecracker API socket; raises FirecrackerError on a non-2xx answer."""
    r, w = await asyncio.wait_for(asyncio.open_unix_connection(str(sock)), timeout)
    try:
        data = json.dumps(body).encode() if body is not None else b""
        head = (f"{method} {path} HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n"
                f"Content-Type: application/json\r\nContent-Length: {len(data)}\r\n\r\n")
        w.write(head.encode() + data)
        await w.drain()
        status = await asyncio.wait_for(r.readline(), timeout)
        length = 0
        while True:
            line = await asyncio.wait_for(r.readline(), timeout)
            if line in (b"\r\n", b"\n", b""):
                break
            k, _, v = line.decode("latin-1").partition(":")
            if k.strip().lower() == "content-length":
                length = int(v.strip() or 0)
        answer = await asyncio.wait_for(r.readexactly(length), timeout) if length else b""
        parts = status.split()
        code = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
        if not 200 <= code < 300:
            raise FirecrackerError(f"{method} {path} -> {code} {answer[:200]!r}")
    finally:
        w.close()
        with contextlib.suppress(Exception):
            await w.wait_closed()


def reseed_escape(seed: bytes) -> bytes:
    """The COM1 escape the kernel consumes to mix a session seed into its generator
    (ADR-0025): ESC ] qseed ; <16 hex> BEL. It never reaches the shell."""
    assert len(seed) == 8 and any(seed), "an all-zero seed is ignored by the kernel"
    return b"\x1b]qseed;" + seed.hex().encode() + b"\x07"


class Snapshot:
    """A booted, paused VM saved on disk, that sessions restore from."""

    def __init__(self, d: Path, kernel_sha: str, transcript: bytes):
        self.dir = d
        self.kernel_sha = kernel_sha
        self.transcript = transcript
        self.users = 0          # sessions whose VM maps this snapshot's memory file
        self.retired = False    # superseded; delete once users reaches 0

    @property
    def vmstate(self) -> Path:
        return self.dir / "vm.snap"

    @property
    def mem(self) -> Path:
        return self.dir / "vm.mem"


def utc_day(now: float) -> str:
    return datetime.datetime.fromtimestamp(now, datetime.timezone.utc).strftime("%Y-%m-%d")


class Histogram:
    """Prometheus-style buckets, plus the most recent values for percentiles."""
    BUCKETS = (0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60, 120, 300, 600)
    RECENT = 1000

    def __init__(self):
        self.counts = [0] * len(self.BUCKETS)   # per bucket (made cumulative on output)
        self.inf = 0
        self.sum = 0.0
        self.n = 0
        self.recent: collections.deque[float] = collections.deque(maxlen=self.RECENT)

    def observe(self, v: float) -> None:
        v = max(0.0, float(v))
        for i, b in enumerate(self.BUCKETS):
            if v <= b:
                self.counts[i] += 1
                break
        else:
            self.inf += 1
        self.sum += v
        self.n += 1
        self.recent.append(round(v, 4))

    def summary(self) -> dict:
        vals = sorted(self.recent)
        if not vals:
            return {"n": self.n}

        def pct(p: float) -> float:
            return round(vals[min(len(vals) - 1, int(p * len(vals)))], 3)
        return {"n": self.n, "p50": pct(0.5), "p90": pct(0.9), "max": round(vals[-1], 3)}

    def prometheus(self, name: str) -> list[str]:
        out, acc = [f"# TYPE {name} histogram"], 0
        for b, c in zip(self.BUCKETS, self.counts):
            acc += c
            out.append(f'{name}_bucket{{le="{b}"}} {acc}')
        out.append(f'{name}_bucket{{le="+Inf"}} {acc + self.inf}')
        out.append(f"{name}_sum {self.sum:.6f}")
        out.append(f"{name}_count {self.n}")
        return out

    def dump(self) -> dict:
        return {"counts": self.counts, "inf": self.inf, "sum": self.sum, "n": self.n,
                "recent": list(self.recent)}

    def load(self, d: dict) -> None:
        if len(d.get("counts", [])) == len(self.BUCKETS):
            self.counts = [int(x) for x in d["counts"]]
        self.inf, self.sum, self.n = int(d.get("inf", 0)), float(d.get("sum", 0)), int(d.get("n", 0))
        self.recent.extend(float(x) for x in d.get("recent", [])[-self.RECENT:])


class Metrics:
    """The visitor funnel, as counts and timings only (#258).

    page loads -> Boot clicks -> /ws sessions started (or refused) -> boots completed
    (the shell prompt reached the visitor) -> sessions where the visitor typed -> ends.

    Nothing here holds an address, a session id, or anything typed. The one piece of
    per-visitor state is the set of today's visitor hashes: an HMAC under a salt that
    exists only in memory and is replaced each UTC day. It counts unique visitors and is
    then forgotten. It is never saved, so a restart can count a visitor twice that day.

    Our own traffic (the internal token) is counted apart, in internal_sessions and
    internal_beacons, and touches nothing else: not the funnel, the timings or the
    unique visitors."""
    EVENTS = ("load", "boot")
    COUNTERS = ("page_loads", "boot_clicks", "sessions_started", "sessions_refused",
                "boots_completed", "sessions_with_input", "sessions_ended",
                "internal_sessions", "internal_beacons", "machines_kept", "machines_resumed")
    LABELLED = ("sessions_started", "sessions_refused", "boots_completed", "sessions_ended")
    DAY_FIELDS = ("page_loads", "boot_clicks", "unique_visitors", "sessions_started",
                  "sessions_refused", "boots_completed", "sessions_with_input",
                  "internal_sessions", "internal_beacons", "machines_kept", "machines_resumed")
    KEEP_DAYS = 60
    INTERNAL_SINCE = "2026-10-24"   # tagging began here; earlier counts include our own traffic

    def __init__(self, state_file: Path | None = None, clock=time.time):
        self.clock = clock
        self.state_file = state_file
        self.since = clock()
        self.c = {k: collections.Counter() for k in self.COUNTERS}
        self.h = {"first_output_seconds": Histogram(), "ready_seconds": Histogram(),
                  "session_seconds": Histogram()}
        self.days: dict[str, dict[str, int]] = {}
        self._day = ""
        self._salt = b""
        self._seen: set[bytes] = set()
        self.dirty = False
        if state_file is not None:
            self._load()

    # ---- recording ----
    def _today(self) -> dict[str, int]:
        day = utc_day(self.clock())
        if day != self._day:      # a new UTC day: a new salt, and yesterday's hashes are dropped
            self._day, self._salt, self._seen = day, secrets.token_bytes(32), set()
        row = self.days.setdefault(day, dict.fromkeys(self.DAY_FIELDS, 0))
        while len(self.days) > self.KEEP_DAYS:
            del self.days[min(self.days)]
        return row

    def _bump(self, counter: str, label: str = "", day_field: str | None = None) -> None:
        self.c[counter][label] += 1
        if day_field:
            self._today()[day_field] += 1
        self.dirty = True

    def visitor(self, ip: str) -> None:
        if not ip or ip == "unknown":
            return
        row = self._today()
        key = hmac.new(self._salt, ip.encode(), hashlib.sha256).digest()[:16]
        if key not in self._seen:
            self._seen.add(key)
            row["unique_visitors"] += 1
            self.dirty = True

    def event(self, name: str, ip: str, internal: bool = False) -> bool:
        """A page beacon. Only the two known events count; anything else is ignored."""
        if name not in self.EVENTS:
            return False
        if internal:
            self._bump("internal_beacons", day_field="internal_beacons")
            return True
        self.visitor(ip)
        if name == "load":
            self._bump("page_loads", day_field="page_loads")
        else:
            self._bump("boot_clicks", day_field="boot_clicks")
        return True

    def internal_session(self) -> None:
        self._bump("internal_sessions", day_field="internal_sessions")

    def refused(self, reason: str) -> None:
        self._bump("sessions_refused", reason, "sessions_refused")

    def started(self, mode: str) -> None:
        self._bump("sessions_started", mode, "sessions_started")

    def first_output(self, seconds: float) -> None:
        self.h["first_output_seconds"].observe(seconds)
        self.dirty = True

    def booted(self, mode: str, seconds: float) -> None:
        self._bump("boots_completed", mode, "boots_completed")
        self.h["ready_seconds"].observe(seconds)

    def typed(self) -> None:
        self._bump("sessions_with_input", day_field="sessions_with_input")

    def kept(self) -> None:
        """A saved machine was kept at the end of a session (it had been synced)."""
        self._bump("machines_kept", day_field="machines_kept")

    def resumed(self) -> None:
        self._bump("machines_resumed", day_field="machines_resumed")

    def ended(self, reason: str, seconds: float) -> None:
        self._bump("sessions_ended", "error" if reason.startswith("error") else reason)
        self.h["session_seconds"].observe(seconds)

    # ---- output ----
    def stats(self, active: int, max_vms: int, snapshot: bool, saved: int | None = None) -> dict:
        self._today()
        now = {"active": active, "max_vms": max_vms, "snapshot": snapshot}
        if saved is not None:
            now["saved_machines"] = saved
        return {
            "about": ("QuantumOS public console funnel. Counts and timings only: no addresses, "
                      "no session ids, nothing typed. Unique visitors are counted per UTC day "
                      "from a hash under a salt that is never stored and is replaced daily. "
                      "Our own test and capture traffic is counted only in internal_sessions "
                      "and internal_beacons (tagged from internal_since on), never in the funnel."),
            "internal_since": self.INTERNAL_SINCE,
            "since": datetime.datetime.fromtimestamp(self.since, datetime.timezone.utc)
                     .isoformat(timespec="seconds"),
            "now": now,
            "totals": {k: dict(v) if k in self.LABELLED else sum(v.values()) for k, v in self.c.items()},
            "timings_s": {k.removesuffix("_seconds"): v.summary() for k, v in self.h.items()},
            "days": dict(sorted(self.days.items(), reverse=True)),
        }

    def prometheus(self, active: int, max_vms: int, snapshot: bool) -> str:
        lines: list[str] = []
        for k in self.COUNTERS:
            name = f"qos_{k}_total"
            lines.append(f"# TYPE {name} counter")
            if k in self.LABELLED:
                label = "mode" if k in ("sessions_started", "boots_completed") else "reason"
                lines += [f'{name}{{{label}="{v}"}} {n}' for v, n in sorted(self.c[k].items())]
            else:
                lines.append(f"{name} {sum(self.c[k].values())}")
        for name, v in (("qos_sessions_active", active), ("qos_sessions_max", max_vms),
                        ("qos_snapshot_ready", int(snapshot))):
            lines += [f"# TYPE {name} gauge", f"{name} {v}"]
        for k, hist in self.h.items():
            lines += hist.prometheus(f"qos_{k}")
        return "\n".join(lines) + "\n"

    # ---- persistence (counts and timings only) ----
    def _load(self) -> None:
        try:
            d = json.loads(self.state_file.read_text())
        except (OSError, ValueError):
            return
        self.since = float(d.get("since", self.since))
        for k, v in d.get("counters", {}).items():
            if k in self.c:
                self.c[k] = collections.Counter({str(a): int(b) for a, b in v.items()})
        for k, v in d.get("histograms", {}).items():
            if k in self.h:
                self.h[k].load(v)
        self.days = {str(day): {f: int(row.get(f, 0)) for f in self.DAY_FIELDS}
                     for day, row in d.get("days", {}).items()}

    def save(self) -> None:
        if self.state_file is None or not self.dirty:
            return
        d = {"since": self.since, "counters": {k: dict(v) for k, v in self.c.items()},
             "histograms": {k: v.dump() for k, v in self.h.items()}, "days": self.days}
        tmp = self.state_file.with_name(self.state_file.name + ".tmp")
        try:
            tmp.write_text(json.dumps(d))
            os.replace(tmp, self.state_file)
            self.dirty = False
        except OSError as e:
            log.warning("metrics save failed: %s", e)


class Console:
    def __init__(self, kernel: Path, firecracker: str, run_dir: Path, max_vms: int,
                 per_ip: int, max_seconds: float, mem_mib: int, boot_args: str,
                 snapshot: str = "auto", ready_marker: str = "qsh>", boot_timeout: float = 60,
                 metrics: Metrics | None = None, qpu_broker: str | None = None,
                 calibration_file: Path | None = None, idle_seconds: float = 120,
                 idle_warn_seconds: float = 15, input_frames_per_s: float = 64,
                 input_bytes_per_s: float = 4096, max_output_bytes: int = 2 << 20,
                 internal_token: str = "", saves: SaveStore | None = None):
        self.saves = saves                         # saved machines (#256), or None: no disks at all
        self.calibration_file = calibration_file   # daily QPU calibration series (#275), read-only
        self.idle_seconds = idle_seconds           # #257: no input and no output for this long ends it
        self.idle_warn_seconds = min(idle_warn_seconds, idle_seconds)
        self.input_frames_per_s = input_frames_per_s
        self.input_bytes_per_s = input_bytes_per_s
        self.max_output_bytes = max_output_bytes
        self.internal_token = internal_token       # tags our own traffic out of the funnel; never logged
        self.kernel = kernel
        self.firecracker = firecracker
        self.run_dir = run_dir
        self.max_vms = max_vms
        self.per_ip = per_ip
        self.max_seconds = max_seconds
        self.mem_mib = mem_mib
        self.boot_args = boot_args
        self.snapshot_mode = snapshot
        self.ready_marker = ready_marker.encode()
        self.boot_timeout = boot_timeout
        self.qpu_broker = qpu_broker      # host quantum broker socket (#262), or None
        self.active: dict[str, str] = {}   # session id -> client address
        self.started_at: dict[str, float] = {}   # session id -> monotonic admit time (for retry-after)
        self.salt = secrets.token_bytes(16)  # per process: hashes do not link across restarts
        self.snap: Snapshot | None = None
        self._build_task: asyncio.Task | None = None
        self._kernel_sig: tuple[int, int] | None = None
        self._kernel_sha = ""
        self._failed: tuple[str, float] | None = None   # (kernel sha, monotonic time) of the last failed build
        self.metrics = metrics if metrics is not None else Metrics()

    # ---- bookkeeping -------------------------------------------------------
    def ip_hash(self, ip: str) -> str:
        return hashlib.sha256(self.salt + ip.encode()).hexdigest()[:12]

    def admit(self, ip: str) -> str | None:
        """Return a refusal reason, or None if a new session may start."""
        if len(self.active) >= self.max_vms:
            return "busy"
        if sum(1 for a in self.active.values() if a == ip) >= self.per_ip:
            return "one-per-visitor"
        return None

    def refusal(self, reason: str) -> dict:
        """The refusal control message. A busy console says how many machines there are and
        how long until one is certainly free: the soonest running session's hard cap. Idle
        sessions end sooner, so this is an upper bound, never a promise of a wait."""
        msg = {"type": "refused", "reason": reason}
        if reason == "busy":
            now = time.monotonic()
            left = [self.max_seconds - (now - t) for t in self.started_at.values()]
            msg["message"] = f"all {self.max_vms} machines are busy"
            msg["retry_after_s"] = max(5, int(min(left, default=self.max_seconds)) + 1)
        return msg

    def is_internal(self, request: Request | None) -> bool:
        """Our own traffic: the internal token as an X-QOS-Internal header or ?internal=."""
        if not self.internal_token or request is None:
            return False
        given = request.headers.get("X-QOS-Internal", "") or \
            (parse_qs(urlsplit(request.path).query).get("internal") or [""])[0]
        return bool(given) and hmac.compare_digest(given.encode(), self.internal_token.encode())

    def sweep(self) -> int:
        """Remove session and snapshot directories left by a previous run (crash, kill -9)."""
        n = 0
        self.run_dir.mkdir(parents=True, exist_ok=True)
        for d in self.run_dir.iterdir():
            if d.is_dir() and (d.name.startswith("s-") or d.name.startswith("base-")):
                shutil.rmtree(d, ignore_errors=True)
                n += 1
        return n

    def prepare(self, sid: str) -> Path:
        d = self.run_dir / f"s-{sid}"
        d.mkdir(mode=0o700)
        (d / "fc.log").touch()
        if self.saves is not None:
            self.blank_disk(d)
        return d

    def blank_disk(self, d: Path) -> None:
        """A zero-filled (sparse) disk: a fresh QuantumOS volume, written only on `sync`."""
        with open(d / DISK, "wb") as f:
            f.truncate(self.saves.image_mib << 20)

    def write_cold_config(self, d: Path) -> Path:
        shutil.copyfile(self.kernel, d / "kernel.elf")
        cfg = {
            "boot-source": {"kernel_image_path": str(d / "kernel.elf"), "boot_args": self.boot_args},
            "drives": [self.drive(str(d / DISK))] if self.saves is not None else [],
            "machine-config": {"vcpu_count": 1, "mem_size_mib": self.mem_mib},
        }
        (d / "vm.json").write_text(json.dumps(cfg))
        return d / "vm.json"

    @staticmethod
    def drive(path: str) -> dict:
        return {"drive_id": "data", "path_on_host": path, "is_root_device": False, "is_read_only": False}

    # ---- snapshot ----------------------------------------------------------
    def kernel_sha(self) -> str:
        """sha256 of the kernel image, recomputed only when its size or mtime changes."""
        st = self.kernel.stat()
        sig = (st.st_size, st.st_mtime_ns)
        if sig != self._kernel_sig:
            self._kernel_sha = hashlib.sha256(self.kernel.read_bytes()).hexdigest()
            self._kernel_sig = sig
        return self._kernel_sha

    def ensure_snapshot(self) -> None:
        """Start a snapshot build if there is none for the current kernel (never blocks)."""
        if self.snapshot_mode != "auto":
            return
        try:
            sha = self.kernel_sha()
        except OSError:
            return
        if self.snap is not None and self.snap.kernel_sha == sha:
            return
        if self._build_task is not None and not self._build_task.done():
            return
        if self._failed and self._failed[0] == sha and time.monotonic() - self._failed[1] < SNAPSHOT_RETRY_S:
            return
        self._build_task = asyncio.create_task(self._build_snapshot(sha))

    def take_snapshot(self) -> Snapshot | None:
        """The snapshot a new session should restore from (counted as in use), or None."""
        self.ensure_snapshot()
        s = self.snap
        if s is None or s.kernel_sha != self._kernel_sha:
            return None
        s.users += 1
        return s

    def release_snapshot(self, s: Snapshot) -> None:
        s.users -= 1
        if s.retired and s.users <= 0:
            shutil.rmtree(s.dir, ignore_errors=True)

    def _install_snapshot(self, s: Snapshot) -> None:
        old, self.snap = self.snap, s
        if old is not None:
            old.retired = True
            if old.users <= 0:
                shutil.rmtree(old.dir, ignore_errors=True)

    async def _start_api_vm(self, d: Path) -> tuple[asyncio.subprocess.Process, Path]:
        sock = d / "fc.sock"
        vm = await asyncio.create_subprocess_exec(
            self.firecracker, "--api-sock", str(sock), "--log-path", str(d / "fc.log"), "--level", "Error",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL, cwd=str(d), start_new_session=True)
        try:
            for _ in range(400):
                if sock.exists():
                    return vm, sock
                if vm.returncode is not None:
                    break
                await asyncio.sleep(0.005)
        except BaseException:   # cancelled (shutdown): never leave the VM behind
            await asyncio.shield(self._stop(vm))
            raise
        await self._stop(vm)
        raise FirecrackerError("API socket did not appear")

    async def _build_snapshot(self, sha: str) -> None:
        d = self.run_dir / f"base-{sha[:12]}-{uuid.uuid4().hex[:6]}"
        vm = None
        t0 = time.monotonic()
        try:
            d.mkdir(mode=0o700)
            (d / "fc.log").touch()
            shutil.copyfile(self.kernel, d / "kernel.elf")
            vm, sock = await self._start_api_vm(d)
            await fc_api(sock, "PUT", "/boot-source",
                         {"kernel_image_path": str(d / "kernel.elf"), "boot_args": self.boot_args})
            await fc_api(sock, "PUT", "/machine-config", {"vcpu_count": 1, "mem_size_mib": self.mem_mib})
            if self.saves is not None:
                # A RELATIVE path: Firecracker opens it against its own cwd, so every VM
                # restored from this snapshot opens the disk in its own session directory.
                self.blank_disk(d)
                await fc_api(sock, "PUT", "/drives/data", self.drive(DISK))
            await fc_api(sock, "PUT", "/actions", {"action_type": "InstanceStart"})
            transcript = b""
            deadline = time.monotonic() + self.boot_timeout
            while self.ready_marker not in ANSI.sub(b"", transcript):
                left = deadline - time.monotonic()
                if left <= 0:
                    raise FirecrackerError("no shell prompt before the boot timeout")
                chunk = await asyncio.wait_for(vm.stdout.read(READ_CHUNK), left)
                if not chunk:
                    raise FirecrackerError("guest exited while booting")
                transcript += chunk
            # Let the guest finish what it prints after the prompt: snapshot once it has
            # been quiet for 0.5 s (at most 5 s), then collect anything still in the pipe.
            quiet_end = time.monotonic() + 5
            while time.monotonic() < quiet_end:
                try:
                    chunk = await asyncio.wait_for(vm.stdout.read(READ_CHUNK), 0.5)
                except asyncio.TimeoutError:
                    break
                if not chunk:
                    raise FirecrackerError("guest exited while settling")
                transcript += chunk
            await fc_api(sock, "PATCH", "/vm", {"state": "Paused"})
            with contextlib.suppress(asyncio.TimeoutError):
                while True:
                    chunk = await asyncio.wait_for(vm.stdout.read(READ_CHUNK), 0.05)
                    if not chunk:
                        break
                    transcript += chunk
            await fc_api(sock, "PUT", "/snapshot/create",
                         {"snapshot_type": "Full", "snapshot_path": str(d / "vm.snap"),
                          "mem_file_path": str(d / "vm.mem")}, timeout=60)
            await self._stop(vm)
            vm = None
            for f in ("kernel.elf", "fc.sock", DISK):
                (d / f).unlink(missing_ok=True)
            if self.kernel_sha() != sha:      # the kernel changed during the build
                raise FirecrackerError("kernel changed during the build")
            self._install_snapshot(Snapshot(d, sha, transcript))
            self._failed = None
            log.info("snapshot ready kernel=%s build_s=%.2f transcript_bytes=%d",
                     sha[:12], time.monotonic() - t0, len(transcript))
        except Exception as e:  # noqa: BLE001 - sessions keep cold-booting
            self._failed = (sha, time.monotonic())
            log.warning("snapshot build failed kernel=%s: %s: %s", sha[:12], type(e).__name__, e)
            shutil.rmtree(d, ignore_errors=True)
        except asyncio.CancelledError:   # shutdown mid-build
            await asyncio.shield(self._stop(vm))
            vm = None
            shutil.rmtree(d, ignore_errors=True)
            raise
        finally:
            await self._stop(vm)

    async def _restore(self, d: Path, s: Snapshot) -> asyncio.subprocess.Process:
        vm, sock = await self._start_api_vm(d)
        try:
            await fc_api(sock, "PUT", "/snapshot/load",
                         {"snapshot_path": str(s.vmstate),
                          "mem_backend": {"backend_type": "File", "backend_path": str(s.mem)},
                          "resume_vm": True}, timeout=30)
        except BaseException:
            await self._stop(vm)
            raise
        return vm

    async def _cold_boot(self, d: Path) -> asyncio.subprocess.Process:
        cfg = self.write_cold_config(d)
        return await asyncio.create_subprocess_exec(
            self.firecracker, "--no-api", "--config-file", str(cfg),
            "--log-path", str(d / "fc.log"), "--level", "Error",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL, cwd=str(d), start_new_session=True)

    async def shutdown(self) -> None:
        if self._build_task is not None and not self._build_task.done():
            self._build_task.cancel()
            await asyncio.gather(self._build_task, return_exceptions=True)
        if self.snap is not None:
            self.snap.retired = True
            if self.snap.users <= 0:
                shutil.rmtree(self.snap.dir, ignore_errors=True)

    # ---- one session -------------------------------------------------------
    async def session(self, ws: ServerConnection) -> None:
        ip = ws.request.headers.get("X-Real-IP", "unknown") if ws.request else "unknown"
        internal = self.is_internal(ws.request)   # our own traffic: kept out of the funnel
        resume = ws.request is not None and \
            (parse_qs(urlsplit(ws.request.path).query).get("resume") or [""])[0] == "1"
        reason = "saving_off" if resume and self.saves is None else self.admit(ip)
        if reason:
            if not internal:
                self.metrics.refused(reason)
            await ws.send(json.dumps(self.refusal(reason)))
            await ws.close(1013, reason)
            log.info("refused ip=%s reason=%s active=%d internal=%d", self.ip_hash(ip), reason,
                     len(self.active), internal)
            return
        sid = uuid.uuid4().hex[:12]
        self.active[sid] = ip
        t0 = time.monotonic()
        self.started_at[sid] = t0
        end_reason = "disconnect"
        started = False
        d: Path | None = None
        vm: asyncio.subprocess.Process | None = None
        snap: Snapshot | None = None
        relay: QpuRelay | None = None
        save_id: str | None = None       # the saved machine this session kept or resumed
        new_keep = False
        try:
            d = self.prepare(sid)
            if resume:
                save_id = await self.resume_machine(ws, d, ip)
                if save_id is None:
                    return
            snap = self.take_snapshot() if save_id is None else None   # a resumed disk must boot
            mode = "cold"
            if snap is not None:
                try:
                    vm = await self._restore(d, snap)
                    mode = "restore"
                except Exception as e:  # noqa: BLE001 - fall back to a cold boot
                    log.warning("restore failed sid=%s: %s: %s", sid, type(e).__name__, e)
                    self.release_snapshot(snap)
                    snap = None
            if vm is None:
                vm = await self._cold_boot(d)
            log.info("start sid=%s ip=%s pid=%d mode=%s ready_ms=%d active=%d internal=%d", sid,
                     self.ip_hash(ip), vm.pid, mode, (time.monotonic() - t0) * 1000, len(self.active),
                     internal)
            started = True
            if internal:
                self.metrics.internal_session()
            else:
                self.metrics.started(mode)
                self.metrics.visitor(ip)
                if save_id is not None:
                    self.metrics.resumed()
            # Funnel timings, from the moment the session was admitted: the first guest
            # byte the visitor gets, and the shell prompt reaching them (boot completed).
            seen = {"output": False, "ready": False, "typed": False}
            tail = b""
            last = [time.monotonic()]    # last input or guest output, for the idle timeout
            out_bytes = 0                # guest bytes sent to the visitor, for the output cap

            def sent(data: bytes) -> None:
                nonlocal tail, out_bytes
                out_bytes += len(data)
                last[0] = time.monotonic()
                if internal:
                    return
                if not seen["output"] and data:
                    seen["output"] = True
                    self.metrics.first_output(time.monotonic() - t0)
                if not seen["ready"]:
                    window = tail + data
                    tail = window[-256:]
                    if self.ready_marker in ANSI.sub(b"", window):
                        seen["ready"] = True
                        self.metrics.booted(mode, time.monotonic() - t0)
            hello = {"type": "hello", "session": sid, "max_seconds": self.max_seconds,
                     "restored": mode == "restore"}
            if self.saves is not None:
                hello.update(can_keep=True, resumed=save_id is not None, keep_days=self.saves.days,
                             keep_mib=self.saves.image_mib)
            await ws.send(json.dumps(hello))
            if snap is not None:
                # Every restored copy of the snapshot resumes the same generator state;
                # give this one its own entropy before the visitor can type (ADR-0025).
                vm.stdin.write(reseed_escape(secrets.token_bytes(8)))
                await vm.stdin.drain()
                await ws.send(snap.transcript)   # what the snapshot VM printed while it booted
                sent(snap.transcript)
            if self.qpu_broker:
                async def write_guest(b: bytes) -> None:
                    vm.stdin.write(b)
                    await vm.stdin.drain()
                relay = QpuRelay(self.qpu_broker, sid, self.ip_hash(ip), write_guest)

            async def guest_to_ws():
                while True:
                    data = await vm.stdout.read(READ_CHUNK)
                    if not data:
                        return "guest-exited"
                    if relay is not None:
                        data = relay.from_guest(data)   # qpu requests go to the broker, not the page
                        if not data:
                            continue
                    room = self.max_output_bytes - out_bytes
                    if len(data) >= room:              # the output cap: send up to it, then end
                        if room > 0:
                            await ws.send(data[:room])
                            sent(data[:room])
                        return "output_cap"
                    await ws.send(data)
                    sent(data)

            async def ws_to_guest():
                frames = TokenBucket(self.input_frames_per_s)
                volume = TokenBucket(self.input_bytes_per_s)
                noticed = False
                nonlocal save_id, new_keep
                async for msg in ws:
                    last[0] = time.monotonic()
                    if isinstance(msg, str) and self.saves is not None and msg.startswith("{"):
                        ctl = control_message(msg)
                        if ctl is not None:
                            if ctl.get("type") == "keep":
                                if save_id is not None:
                                    await ws.send(json.dumps({"type": "kept", "already": True}))
                                    continue
                                try:
                                    token, save_id = self.saves.keep(self.ip_hash(ip))
                                except SaveRefused as e:
                                    await ws.send(json.dumps({"type": "keep_refused", "reason": str(e)}))
                                    continue
                                new_keep = True
                                log.info("keep sid=%s save=%s", sid, save_id[:8])
                                await ws.send(json.dumps({"type": "kept", "token": token,
                                                          "days": self.saves.days}))
                            continue    # a control message is never keystrokes
                    data = msg.encode() if isinstance(msg, str) else msg
                    data = data[:MAX_INPUT].replace(b"\r\n", b"\n").replace(b"\r", b"\n")
                    if not (frames.take() and volume.take(len(data))):
                        if not noticed:                # dropped, with one notice; never a disconnect
                            noticed = True
                            await ws.send(INPUT_NOTICE.encode())
                        continue
                    if not seen["typed"] and data and not internal:
                        seen["typed"] = True
                        self.metrics.typed()
                    if relay is not None:
                        data = relay.from_visitor(data)   # no typed qpu requests or answers
                    vm.stdin.write(data)
                    await vm.stdin.drain()
                return "disconnect"

            async def idle_watch():
                warn_at = self.idle_seconds - self.idle_warn_seconds
                warned = False
                while True:
                    idle = time.monotonic() - last[0]
                    if idle >= self.idle_seconds:
                        return "idle"
                    if idle < warn_at:
                        warned = False
                        wait = warn_at - idle
                    else:
                        if not warned and self.idle_warn_seconds > 0:
                            warned = True     # the warning is console output, not guest activity
                            await ws.send(IDLE_WARNING.format(s=round(self.idle_seconds - idle)).encode())
                        wait = self.idle_seconds - idle
                    await asyncio.sleep(min(max(wait, 0.02), 1.0))

            tasks = {asyncio.create_task(guest_to_ws()), asyncio.create_task(ws_to_guest()),
                     asyncio.create_task(idle_watch())}
            done, pending = await asyncio.wait(tasks, timeout=self.max_seconds,
                                               return_when=asyncio.FIRST_COMPLETED)
            for t in pending:
                t.cancel()
            if not done:
                end_reason = "expired"
                try:
                    await ws.send(json.dumps({"type": "expired"}))
                except ConnectionClosed:
                    pass
            else:
                end_reason = end_reason_of(done)
                ctl = {"guest-exited": {"type": "exited"},
                       "idle": {"type": "idle", "seconds": self.idle_seconds},
                       "output_cap": {"type": "output_cap", "bytes": self.max_output_bytes}}.get(end_reason)
                if ctl is not None:
                    try:
                        await ws.send(json.dumps(ctl))
                    except ConnectionClosed:
                        pass
            await asyncio.gather(*pending, return_exceptions=True)
        except ConnectionClosed:
            end_reason = "disconnect"
        except Exception as e:  # noqa: BLE001
            end_reason = f"error:{type(e).__name__}"
            log.exception("session %s failed", sid)
        finally:
            if relay is not None:
                await relay.close()
            await self._stop(vm)
            if save_id is not None and d is not None:
                kept = self.saves.checkin(save_id, d / DISK)
                log.info("save end sid=%s save=%s kept=%d", sid, save_id[:8], kept)
                if kept and new_keep and not internal:
                    self.metrics.kept()
                with contextlib.suppress(Exception):
                    await ws.send(json.dumps({"type": "saved" if kept else "not_saved"}))
            if d is not None:
                shutil.rmtree(d, ignore_errors=True)
            if snap is not None:
                self.release_snapshot(snap)
            self.active.pop(sid, None)
            self.started_at.pop(sid, None)
            try:
                await ws.close()
            except Exception:  # noqa: BLE001
                pass
            if started and not internal:
                self.metrics.ended(end_reason, time.monotonic() - t0)
            log.info("end sid=%s ip=%s reason=%s duration_s=%.1f active=%d",
                     sid, self.ip_hash(ip), end_reason, time.monotonic() - t0, len(self.active))

    async def resume_machine(self, ws: ServerConnection, d: Path, ip: str) -> str | None:
        """The first frame of a /ws?resume=1 session names the machine. Returns its id with
        its disk copied into `d`, or None after sending the refusal."""
        reason = "no_token"
        try:
            ctl = control_message(await asyncio.wait_for(ws.recv(), RESUME_WAIT_S))
            token = ctl.get("token") if ctl and ctl.get("type") == "resume" else None
            if isinstance(token, str) and token:
                save_id = self.saves.checkout(token)
                try:
                    shutil.copyfile(self.saves.image(save_id), d / DISK)
                except OSError:
                    self.saves.open.discard(save_id)
                    raise SaveRefused("unknown_machine")
                return save_id
        except SaveRefused as e:
            reason = str(e)
        except (asyncio.TimeoutError, TypeError):
            pass
        await ws.send(json.dumps({"type": "refused", "reason": reason}))
        log.info("resume refused ip=%s reason=%s", self.ip_hash(ip), reason)
        return None

    @staticmethod
    async def _stop(vm: asyncio.subprocess.Process | None) -> None:
        if vm is None or vm.returncode is not None:
            return

        async def discard(stream: asyncio.StreamReader) -> None:
            # wait() returns only once the pipes close. A guest that was still printing when
            # the bridge stopped reading (output cap, time cap) leaves stdout full and paused,
            # so without this the kill is only reaped by the SIGKILL fallback 5 s later.
            with contextlib.suppress(Exception):
                while await stream.read(65536):
                    pass
        drain = asyncio.ensure_future(discard(vm.stdout)) if vm.stdout is not None else None
        try:
            os.killpg(vm.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError, AttributeError, OSError):
            vm.terminate()
        try:
            await asyncio.wait_for(vm.wait(), 5)
        except asyncio.TimeoutError:
            try:
                os.killpg(vm.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError, AttributeError, OSError):
                vm.kill()
            await vm.wait()
        finally:
            if drain is not None:
                drain.cancel()
                await asyncio.gather(drain, return_exceptions=True)

    # ---- HTTP side ---------------------------------------------------------
    def snapshot_ready(self) -> bool:
        snap = self.snap
        return snap is not None and snap.kernel_sha == self._kernel_sha

    def process_request(self, ws: ServerConnection, request: Request) -> Response | None:
        url = urlsplit(request.path)
        path = url.path.rstrip("/")
        if path.startswith("/qos/"):          # the staging route keeps its /qos prefix
            path = path[len("/qos"):]
        if path == "/health":
            snap = self.snap
            body = json.dumps({"active": len(self.active), "max_vms": self.max_vms,
                               "snapshot": self.snapshot_ready(),
                               "snapshot_kernel": snap.kernel_sha[:12] if snap else None})
            return ws.respond(http.HTTPStatus.OK, body + "\n")
        if path == "/stats":
            saved = self.saves.count() if self.saves is not None else None
            body = json.dumps(self.metrics.stats(len(self.active), self.max_vms, self.snapshot_ready(),
                                                 saved), indent=1)
            return self._respond(ws, body + "\n", "application/json")
        if path == "/metrics":
            return self._respond(ws, self.metrics.prometheus(len(self.active), self.max_vms,
                                                             self.snapshot_ready()),
                                 "text/plain; version=0.0.4")
        if path in ("/calibration.json", "/calibration"):
            doc = calibration_doc(self.calibration_file)
            if doc is None:
                return self._respond(ws, "no calibration series on this host yet\n", "text/plain",
                                     http.HTTPStatus.NOT_FOUND)
            if path == "/calibration":
                return self._respond(ws, calibration_table(doc), "text/plain")
            return self._respond(ws, json.dumps(doc, indent=1) + "\n", "application/json")
        if path == "/beacon":
            event = (parse_qs(url.query).get("e") or [""])[0]
            ok = self.metrics.event(event, request.headers.get("X-Real-IP", "unknown"),
                                    internal=self.is_internal(request))
            return self._respond(ws, "ok\n" if ok else "unknown event\n", "text/plain",
                                 http.HTTPStatus.OK if ok else http.HTTPStatus.BAD_REQUEST)
        return None

    @staticmethod
    def _respond(ws: ServerConnection, body: str, ctype: str,
                 status: http.HTTPStatus = http.HTTPStatus.OK) -> Response:
        r = ws.respond(status, body)
        del r.headers["Content-Type"]
        r.headers["Content-Type"] = ctype if "charset" in ctype else ctype + "; charset=utf-8"
        r.headers["Cache-Control"] = "no-store"
        return r


def control_message(msg) -> dict | None:
    """A JSON control frame from the page ({"type": ...}), or None if `msg` is keystrokes."""
    if not isinstance(msg, str) or not msg.startswith("{") or len(msg) > MAX_INPUT:
        return None
    try:
        d = json.loads(msg)
    except ValueError:
        return None
    return d if isinstance(d, dict) and d.get("type") in ("keep", "resume") else None


def calibration_doc(path: Path | None, today: datetime.date | None = None) -> dict | None:
    """The calibration series file (written by tools/qpu-broker/qpu_calibrate.py), plus the
    days since its first row that have no row at all: the timer did not run, which would
    otherwise be a silent gap. None when there is no file."""
    if path is None:
        return None
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as e:
        return {"error": f"calibration file unreadable ({type(e).__name__})", "rows": [], "missing_days": []}
    rows = doc.get("rows") or []
    have = {r.get("date") for r in rows}
    missing = []
    if rows:
        try:
            d = datetime.date.fromisoformat(min(have))
        except (TypeError, ValueError):
            d = None
        today = today or datetime.datetime.now(datetime.timezone.utc).date()
        while d is not None and d < today:
            if d.isoformat() not in have:
                missing.append(d.isoformat())
            d += datetime.timedelta(days=1)
    doc["missing_days"] = missing
    return doc


def calibration_table(doc: dict) -> str:
    """A short readable table of the calibration series."""
    out = ["QuantumOS daily QPU calibration (#275)",
           "Bell circuit (H q0; CX q0,q1; measure), the same shots every day, run at 00:05 UTC",
           "through the console's quantum broker; one job a day is reserved for it.",
           "frac = (00 + 11) / shots: 1.00 is ideal, about 0.5 is noise. A device change starts a",
           "new series; compare rows within one series. Idea: mira_muse (1F916, c119164).",
           "JSON: /calibration.json", ""]
    if doc.get("error"):
        out.append("ERROR: " + doc["error"])
    out.append(f"{'date':<10}  {'state':<8} {'ser':>3}  {'device':<36} {'00':>3} {'01':>3} {'10':>3} {'11':>3}"
               f"  {'frac':>5}  {'credits':>7}  job / reason")
    rows = [(r.get("date") or "", r) for r in doc.get("rows") or []]
    rows += [(d, None) for d in doc.get("missing_days") or []]
    for d, r in sorted(rows, key=lambda x: x[0]):
        if r is None:
            out.append(f"{d:<10}  {'MISSING':<8} {'':>3}  no row: the calibration did not run this day")
            continue
        c = r.get("counts") or {}
        cell = (lambda k: f"{c.get(k, 0):>3}") if c else (lambda k: f"{'-':>3}")
        frac = r.get("fraction_00_11")
        cr = r.get("credits_billed")
        tail = r.get("provider_job") or ""
        if r.get("state") != "done" and r.get("reason"):
            tail = (tail + " " if tail else "") + "(" + str(r["reason"])[:160] + ")"
        if r.get("device_changed"):
            tail = "NEW DEVICE " + tail
        out.append(f"{d:<10}  {str(r.get('state')):<8} {str(r.get('series') or '-'):>3}  "
                   f"{str(r.get('device') or '-'):<36} {cell('00')} {cell('01')} {cell('10')} {cell('11')}"
                   f"  {(f'{frac:.2f}' if frac is not None else '-'):>5}  "
                   f"{(f'{cr:.4f}' if cr is not None else '-'):>7}  {tail}")
    return "\n".join(out) + "\n"


def listen_problem(a: argparse.Namespace) -> str | None:
    """Why this listen address is unsafe, or None. --per-ip trusts X-Real-IP, which only a
    fronting proxy can be trusted to set: a public TCP listener without one would let any
    client claim any address. A Unix socket or loopback is reachable only through the proxy."""
    if not a.listen_tcp or a.trust_forwarded_for:
        return None
    host = a.listen_tcp.rsplit(":", 1)[0]
    if host in LOOPBACK:
        return None
    return (f"refusing to listen on {a.listen_tcp}: --per-ip trusts X-Real-IP, so a non-loopback "
            "listener must sit behind a proxy that sets it; pass --trust-forwarded-for if it does")


def read_token(path: str | None) -> str:
    """The internal-traffic token (one line). Empty when unset, missing or unreadable, which
    only means nothing is tagged internal; the console itself runs as before."""
    if not path:
        return ""
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except OSError as e:
        log.warning("internal token unreadable (%s): no traffic will be tagged internal", type(e).__name__)
        return ""


async def amain(a: argparse.Namespace, stop: asyncio.Future | None = None) -> int:
    problem = listen_problem(a)
    if problem:
        log.error("%s", problem)
        return 2
    metrics = Metrics(Path(a.state_file) if a.state_file else None)
    con = Console(Path(a.kernel), a.firecracker, Path(a.run_dir), a.max_vms, a.per_ip,
                  a.max_seconds, a.mem_mib, a.boot_args, a.snapshot, a.ready_marker,
                  metrics=metrics, qpu_broker=a.qpu_broker,
                  calibration_file=Path(a.calibration_file) if a.calibration_file else None,
                  idle_seconds=a.idle_seconds, idle_warn_seconds=a.idle_warn_seconds,
                  input_frames_per_s=a.input_frames_per_s, input_bytes_per_s=a.input_bytes_per_s,
                  max_output_bytes=a.max_output_bytes, internal_token=read_token(a.internal_token_file),
                  saves=SaveStore(Path(a.save_dir), a.save_mib, a.save_days, a.save_total_mib,
                                  a.saves_per_ip_day) if a.save_dir else None)
    swept = con.sweep()
    if swept:
        log.info("swept %d stale session dirs", swept)
    if con.saves is not None:
        log.info("saved machines: %d kept, %d expired", con.saves.count(), con.saves.sweep())
    con.ensure_snapshot()
    if stop is None:
        stop = asyncio.get_running_loop().create_future()
    for s in (signal.SIGTERM, signal.SIGINT):
        try:
            asyncio.get_running_loop().add_signal_handler(s, lambda: stop.done() or stop.set_result(None))
        except (NotImplementedError, RuntimeError):  # Windows, or not the main thread
            pass
    kw = dict(process_request=con.process_request, max_size=MAX_INPUT * 4, ping_interval=20, ping_timeout=20)
    if a.listen_unix:
        sock = Path(a.listen_unix)
        sock.unlink(missing_ok=True)
        server = await unix_serve(con.session, str(sock), **kw)
        os.chmod(sock, 0o660)
        log.info("listening unix %s", sock)
    else:
        host, port = a.listen_tcp.rsplit(":", 1)
        server = await serve(con.session, host, int(port), **kw)
        log.info("listening tcp %s", a.listen_tcp)
    async def save_loop():
        while True:
            await asyncio.sleep(SAVE_EVERY_S)
            metrics.save()

    async def sweep_loop():
        while True:
            await asyncio.sleep(SAVE_SWEEP_S)
            n = con.saves.sweep()
            if n:
                log.info("saved machines: %d expired", n)

    saver = asyncio.create_task(save_loop())
    sweeper = asyncio.create_task(sweep_loop()) if con.saves is not None else None
    try:
        async with server:
            await stop
    finally:
        saver.cancel()
        if sweeper is not None:
            sweeper.cancel()
        await asyncio.gather(saver, *([sweeper] if sweeper else []), return_exceptions=True)
        metrics.save()
        await con.shutdown()
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--firecracker", default=os.environ.get("FIRECRACKER_BIN", "/usr/local/bin/firecracker"))
    ap.add_argument("--run-dir", required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--listen-unix")
    g.add_argument("--listen-tcp", help="host:port (tests only)")
    ap.add_argument("--max-vms", type=int, default=4)
    ap.add_argument("--per-ip", type=int, default=1)
    ap.add_argument("--max-seconds", type=float, default=600, help="hard cap per session")
    ap.add_argument("--idle-seconds", type=float, default=120,
                    help="end a session after this long with no input and no output")
    ap.add_argument("--idle-warn-seconds", type=float, default=15,
                    help="warn on the terminal this long before the idle timeout (0: no warning)")
    ap.add_argument("--input-frames-per-s", type=float, default=64,
                    help="input frames per second per session (1 s burst); excess is dropped")
    ap.add_argument("--input-bytes-per-s", type=float, default=4096,
                    help="input bytes per second per session (1 s burst); excess is dropped")
    ap.add_argument("--max-output-bytes", type=int, default=2 << 20,
                    help="guest output per session; reaching it ends the session (reason output_cap)")
    ap.add_argument("--trust-forwarded-for", action="store_true",
                    help="allow a non-loopback --listen-tcp: a proxy in front really sets X-Real-IP")
    ap.add_argument("--internal-token-file", default=None,
                    help="file holding the token that tags our own traffic out of the visitor funnel")
    ap.add_argument("--mem-mib", type=int, default=128)
    ap.add_argument("--boot-args", default="quiet")
    ap.add_argument("--snapshot", choices=("auto", "off"), default="auto",
                    help="auto: restore visitors from a booted snapshot; off: cold-boot every session")
    ap.add_argument("--state-file",
                    help="JSON file the funnel metrics (counts and timings only) persist to")
    ap.add_argument("--ready-marker", default="qsh>",
                    help="guest output (ANSI colour stripped) that means the shell is ready to snapshot")
    ap.add_argument("--qpu-broker", default=None,
                    help="Unix socket of the host quantum broker (tools/qpu-broker); unset: no qpu relay")
    ap.add_argument("--save-dir", default=None,
                    help="keep visitors' machines here (#256); unset: VMs have no disk and nothing is kept")
    ap.add_argument("--save-mib", type=int, default=4, help="size of each VM's disk")
    ap.add_argument("--save-days", type=float, default=7,
                    help="delete a saved machine after this many days without use")
    ap.add_argument("--save-total-mib", type=int, default=2048,
                    help="most disk the saved machines may use; new keeps are refused beyond it")
    ap.add_argument("--saves-per-ip-day", type=int, default=3,
                    help="machines one visitor address may keep per UTC day")
    ap.add_argument("--calibration-file", default=None,
                    help="daily QPU calibration series JSON (tools/qpu-broker/qpu_calibrate.py), "
                         "served read-only at /calibration.json and /calibration")
    return ap


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    return asyncio.run(amain(a))


if __name__ == "__main__":
    sys.exit(main())
