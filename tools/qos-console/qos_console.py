#!/usr/bin/env python3
"""QuantumOS public console: one throwaway Firecracker microVM per visitor.

Each WebSocket session gets a fresh microVM booted from the same kernel image
(docs/FIRECRACKER.md: PVH entry, serial console on Firecracker's stdio). The VM
has no drives and no network interface; its serial console is the only I/O, and
it is bridged to the WebSocket byte for byte. The VM and its directory are
deleted when the visitor disconnects, when the hard time cap runs out, or when
the guest exits.

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
import hashlib
import http
import json
import logging
import os
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


class Console:
    def __init__(self, kernel: Path, firecracker: str, run_dir: Path, max_vms: int,
                 per_ip: int, max_seconds: float, mem_mib: int, boot_args: str):
        self.kernel = kernel
        self.firecracker = firecracker
        self.run_dir = run_dir
        self.max_vms = max_vms
        self.per_ip = per_ip
        self.max_seconds = max_seconds
        self.mem_mib = mem_mib
        self.boot_args = boot_args
        self.active: dict[str, str] = {}   # session id -> client address
        self.salt = secrets.token_bytes(16)  # per process: hashes do not link across restarts

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
        """Remove session directories left by a previous run (crash, kill -9)."""
        n = 0
        self.run_dir.mkdir(parents=True, exist_ok=True)
        for d in self.run_dir.iterdir():
            if d.is_dir() and d.name.startswith("s-"):
                shutil.rmtree(d, ignore_errors=True)
                n += 1
        return n

    def prepare(self, sid: str) -> Path:
        d = self.run_dir / f"s-{sid}"
        d.mkdir(mode=0o700)
        shutil.copyfile(self.kernel, d / "kernel.elf")
        cfg = {
            "boot-source": {"kernel_image_path": str(d / "kernel.elf"), "boot_args": self.boot_args},
            "drives": [],
            "machine-config": {"vcpu_count": 1, "mem_size_mib": self.mem_mib},
            "logger": {"level": "Error", "log_path": str(d / "fc.log")},
        }
        (d / "fc.log").touch()
        (d / "vm.json").write_text(json.dumps(cfg))
        return d

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
        try:
            d = self.prepare(sid)
            vm = await asyncio.create_subprocess_exec(
                self.firecracker, "--no-api", "--config-file", str(d / "vm.json"),
                stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL, cwd=str(d), start_new_session=True)
            log.info("start sid=%s ip=%s pid=%d active=%d", sid, self.ip_hash(ip), vm.pid, len(self.active))
            await ws.send(json.dumps({"type": "hello", "session": sid, "max_seconds": self.max_seconds}))

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
            body = json.dumps({"active": len(self.active), "max_vms": self.max_vms}).encode()
            return ws.respond(http.HTTPStatus.OK, body.decode() + "\n")
        return None


async def amain(a: argparse.Namespace, stop: asyncio.Future | None = None) -> int:
    con = Console(Path(a.kernel), a.firecracker, Path(a.run_dir), a.max_vms, a.per_ip,
                  a.max_seconds, a.mem_mib, a.boot_args)
    swept = con.sweep()
    if swept:
        log.info("swept %d stale session dirs", swept)
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
    async with server:
        await stop
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
    return ap


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    return asyncio.run(amain(a))


if __name__ == "__main__":
    sys.exit(main())
