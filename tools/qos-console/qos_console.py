#!/usr/bin/env python3
"""QuantumOS public console: one throwaway Firecracker microVM per visitor.

Each WebSocket session gets its own microVM (docs/FIRECRACKER.md: PVH entry, serial
console on Firecracker's stdio). The VM has no drives and no network interface; its
serial console is the only I/O, and it is bridged to the WebSocket byte for byte. The
VM and its directory are deleted when the visitor disconnects, when the hard time cap
runs out, or when the guest exits.

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
  server -> client  text frames:   JSON control ({"type": "hello"|"expired"|"refused"|"exited", ...})
  client -> server  binary or text frames: keystrokes for COM1 (max 1 KiB per frame)

Caps: --max-vms concurrent VMs in total, --per-ip per client address (taken
from X-Real-IP, which only the fronting nginx sets; the launcher is reachable
only through a Unix socket that nginx reaches over an SSH tunnel), and
--max-seconds per session.

The log records session start, end, duration, reason and a salted hash of the
client address. It never records anything the visitor types or the guest prints.

  qos_console.py --kernel /opt/qos-console/kernel.elf --run-dir /run/qos-console \
                 --listen-unix /run/qos-console/ws.sock
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import hashlib
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

from websockets.asyncio.server import ServerConnection, serve, unix_serve
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

log = logging.getLogger("qos-console")

MAX_INPUT = 1024          # bytes per client frame
READ_CHUNK = 4096         # bytes per guest read
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
SNAPSHOT_RETRY_S = 300    # after a failed snapshot build, cold-boot this long before retrying


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


class Console:
    def __init__(self, kernel: Path, firecracker: str, run_dir: Path, max_vms: int,
                 per_ip: int, max_seconds: float, mem_mib: int, boot_args: str,
                 snapshot: str = "auto", ready_marker: str = "qsh>", boot_timeout: float = 60):
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
        self.active: dict[str, str] = {}   # session id -> client address
        self.salt = secrets.token_bytes(16)  # per process: hashes do not link across restarts
        self.snap: Snapshot | None = None
        self._build_task: asyncio.Task | None = None
        self._kernel_sig: tuple[int, int] | None = None
        self._kernel_sha = ""
        self._failed: tuple[str, float] | None = None   # (kernel sha, monotonic time) of the last failed build

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
        return d

    def write_cold_config(self, d: Path) -> Path:
        shutil.copyfile(self.kernel, d / "kernel.elf")
        cfg = {
            "boot-source": {"kernel_image_path": str(d / "kernel.elf"), "boot_args": self.boot_args},
            "drives": [],
            "machine-config": {"vcpu_count": 1, "mem_size_mib": self.mem_mib},
        }
        (d / "vm.json").write_text(json.dumps(cfg))
        return d / "vm.json"

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
            for f in ("kernel.elf", "fc.sock"):
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
        reason = self.admit(ip)
        if reason:
            await ws.send(json.dumps({"type": "refused", "reason": reason}))
            await ws.close(1013, reason)
            log.info("refused ip=%s reason=%s active=%d", self.ip_hash(ip), reason, len(self.active))
            return
        sid = uuid.uuid4().hex[:12]
        self.active[sid] = ip
        t0 = time.monotonic()
        end_reason = "disconnect"
        d: Path | None = None
        vm: asyncio.subprocess.Process | None = None
        snap: Snapshot | None = None
        try:
            d = self.prepare(sid)
            snap = self.take_snapshot()
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
            log.info("start sid=%s ip=%s pid=%d mode=%s ready_ms=%d active=%d", sid, self.ip_hash(ip),
                     vm.pid, mode, (time.monotonic() - t0) * 1000, len(self.active))
            await ws.send(json.dumps({"type": "hello", "session": sid, "max_seconds": self.max_seconds,
                                      "restored": mode == "restore"}))
            if snap is not None:
                # Every restored copy of the snapshot resumes the same generator state;
                # give this one its own entropy before the visitor can type (ADR-0025).
                vm.stdin.write(reseed_escape(secrets.token_bytes(8)))
                await vm.stdin.drain()
                await ws.send(snap.transcript)   # what the snapshot VM printed while it booted

            async def guest_to_ws():
                while True:
                    data = await vm.stdout.read(READ_CHUNK)
                    if not data:
                        return "guest-exited"
                    await ws.send(data)

            async def ws_to_guest():
                async for msg in ws:
                    data = msg.encode() if isinstance(msg, str) else msg
                    data = data[:MAX_INPUT].replace(b"\r\n", b"\n").replace(b"\r", b"\n")
                    vm.stdin.write(data)
                    await vm.stdin.drain()
                return "disconnect"

            tasks = {asyncio.create_task(guest_to_ws()), asyncio.create_task(ws_to_guest())}
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
                if end_reason == "guest-exited":
                    try:
                        await ws.send(json.dumps({"type": "exited"}))
                    except ConnectionClosed:
                        pass
            await asyncio.gather(*pending, return_exceptions=True)
        except ConnectionClosed:
            end_reason = "disconnect"
        except Exception as e:  # noqa: BLE001
            end_reason = f"error:{type(e).__name__}"
            log.exception("session %s failed", sid)
        finally:
            await self._stop(vm)
            if d is not None:
                shutil.rmtree(d, ignore_errors=True)
            if snap is not None:
                self.release_snapshot(snap)
            self.active.pop(sid, None)
            try:
                await ws.close()
            except Exception:  # noqa: BLE001
                pass
            log.info("end sid=%s ip=%s reason=%s duration_s=%.1f active=%d",
                     sid, self.ip_hash(ip), end_reason, time.monotonic() - t0, len(self.active))

    @staticmethod
    async def _stop(vm: asyncio.subprocess.Process | None) -> None:
        if vm is None or vm.returncode is not None:
            return
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

    # ---- HTTP side ---------------------------------------------------------
    def process_request(self, ws: ServerConnection, request: Request) -> Response | None:
        if request.path.split("?")[0].rstrip("/") in ("/health", "/qos/health"):
            snap = self.snap
            body = json.dumps({"active": len(self.active), "max_vms": self.max_vms,
                               "snapshot": snap is not None and snap.kernel_sha == self._kernel_sha,
                               "snapshot_kernel": snap.kernel_sha[:12] if snap else None}).encode()
            return ws.respond(http.HTTPStatus.OK, body.decode() + "\n")
        return None


async def amain(a: argparse.Namespace, stop: asyncio.Future | None = None) -> int:
    con = Console(Path(a.kernel), a.firecracker, Path(a.run_dir), a.max_vms, a.per_ip,
                  a.max_seconds, a.mem_mib, a.boot_args, a.snapshot, a.ready_marker)
    swept = con.sweep()
    if swept:
        log.info("swept %d stale session dirs", swept)
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
    try:
        async with server:
            await stop
    finally:
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
    ap.add_argument("--max-seconds", type=float, default=600)
    ap.add_argument("--mem-mib", type=int, default=128)
    ap.add_argument("--boot-args", default="quiet")
    ap.add_argument("--snapshot", choices=("auto", "off"), default="auto",
                    help="auto: restore visitors from a booted snapshot; off: cold-boot every session")
    ap.add_argument("--ready-marker", default="qsh>",
                    help="guest output (ANSI colour stripped) that means the shell is ready to snapshot")
    return ap


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    return asyncio.run(amain(a))


if __name__ == "__main__":
    sys.exit(main())
