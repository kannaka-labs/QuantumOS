"""Tests for the QuantumOS public console launcher (Linux only: uses process groups).

Run: python3 -m pytest -q tools/qos-console
They drive the real launcher over a real WebSocket, with fake_firecracker.py standing
in for the hypervisor, and check the guarantees the public page depends on:
the VM starts and talks, the caps refuse, the time cap ends sessions, the VM process
and its directory are gone afterwards (normal end, guest crash, guest hang), and the
log carries no keystrokes.
"""
from __future__ import annotations

import asyncio
import json
import os
import socket
import stat
import sys
import time
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="process groups are POSIX-only")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qos_console  # noqa: E402

websockets = pytest.importorskip("websockets")
from websockets.asyncio.client import connect  # noqa: E402


CRLF = bytes([13, 10])
HEALTH_REQUEST = CRLF.join([b"GET /health HTTP/1.1", b"Host: t", b"Connection: close", b"", b""])


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


@pytest.fixture
def env(tmp_path):
    fake = tmp_path / "firecracker"
    fake.write_text(f"#!{sys.executable}\n" + (HERE / "fake_firecracker.py").read_text())
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    kernel = tmp_path / "kernel.elf"
    kernel.write_bytes(b"\x7fELF fake kernel")
    run = tmp_path / "run"
    return {"fake": str(fake), "kernel": str(kernel), "run": run}


class Server:
    def __init__(self, env, **kw):
        self.port = free_port()
        self.args = ["--kernel", env["kernel"], "--firecracker", env["fake"], "--run-dir", str(env["run"]),
                     "--listen-tcp", f"127.0.0.1:{self.port}"]
        for k, v in kw.items():
            self.args += [f"--{k.replace('_', '-')}", str(v)]
        self.run = env["run"]

    async def __aenter__(self):
        a = qos_console.build_parser().parse_args(self.args)  # the launcher's own parser and defaults
        self.stop = asyncio.get_running_loop().create_future()
        self.task = asyncio.create_task(qos_console.amain(a, self.stop))
        for _ in range(100):  # wait for a real HTTP answer from /health, not a bare TCP connect
            try:
                status, _ = await self._get_health()
                if b" 200 " in status:
                    return self
            except OSError:
                pass
            await asyncio.sleep(0.05)
        raise RuntimeError("server did not start")

    async def _get_health(self) -> tuple[bytes, bytes]:
        r, w = await asyncio.open_connection("127.0.0.1", self.port)
        w.write(HEALTH_REQUEST)
        await w.drain()
        raw = await asyncio.wait_for(r.read(), 5)
        w.close()
        await w.wait_closed()
        head, _, body = raw.partition(CRLF + CRLF)
        return head.split(CRLF, 1)[0], body

    async def health(self) -> dict:
        _, body = await self._get_health()
        return json.loads(body)

    async def __aexit__(self, *exc):
        if not self.stop.done():
            self.stop.set_result(None)
        await asyncio.wait_for(self.task, 10)

    def url(self):
        return f"ws://127.0.0.1:{self.port}/ws"


async def read_until(ws, needle: bytes, timeout=5.0) -> bytes:
    buf, t0 = b"", time.monotonic()
    while needle not in buf:
        left = timeout - (time.monotonic() - t0)
        if left <= 0:
            raise AssertionError(f"did not see {needle!r} in {buf!r}")
        msg = await asyncio.wait_for(ws.recv(), left)
        if isinstance(msg, bytes):
            buf += msg
    return buf


async def control(ws, timeout=5.0) -> dict:
    while True:
        msg = await asyncio.wait_for(ws.recv(), timeout)
        if isinstance(msg, str):
            return json.loads(msg)


def fake_pids(run: Path) -> set[int]:
    """PIDs of hypervisors whose config lives under THIS test's run dir. Matching on the
    run dir (not on the binary name) keeps a host's real, long-running Firecracker VMs
    out of the count."""
    out = set()
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        try:
            cmd = (p / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if b"--config-file" in cmd and any(str(run).encode() in c for c in cmd):
            out.add(int(p.name))
    return out


def sessions(run: Path) -> list[str]:
    return sorted(d.name for d in run.iterdir() if d.name.startswith("s-")) if run.exists() else []


def h(ip):
    return {"X-Real-IP": ip}


@pytest.mark.asyncio
async def test_boot_talk_and_cleanup(env):
    async with Server(env) as s:
        async with connect(s.url(), additional_headers=h("10.0.0.1")) as ws:
            hello = await control(ws)
            assert hello["type"] == "hello" and hello["max_seconds"] == 600
            await read_until(ws, b"qsh> ")
            assert len(sessions(env["run"])) == 1
            await ws.send("echo hello quantum\r")          # Enter from xterm is \r
            out = await read_until(ws, b"hello quantum")
            assert b"hello quantum" in out
        await asyncio.sleep(0.5)
        assert sessions(env["run"]) == [], "session dir must be deleted on disconnect"
        assert not fake_pids(env["run"]), "VM process must be gone on disconnect"
        assert (await s.health())["active"] == 0


@pytest.mark.asyncio
async def test_vm_config_has_no_drives_or_network(env):
    # fake_firecracker asserts this and exits non-zero otherwise; a clean banner proves it
    async with Server(env) as s:
        async with connect(s.url(), additional_headers=h("10.0.0.2")) as ws:
            await control(ws)
            await read_until(ws, b"Welcome to QuantumOS")
            cfg = json.loads((env["run"] / sessions(env["run"])[0] / "vm.json").read_text())
            assert cfg["drives"] == [] and "network-interfaces" not in cfg
            assert cfg["machine-config"]["mem_size_mib"] == 128
            assert cfg["boot-source"]["kernel_image_path"].startswith(str(env["run"]))


@pytest.mark.asyncio
async def test_per_ip_cap(env):
    async with Server(env, per_ip=1) as s:
        async with connect(s.url(), additional_headers=h("10.0.0.3")) as a:
            await control(a)
            async with connect(s.url(), additional_headers=h("10.0.0.3")) as b:
                msg = await control(b)
                assert msg == {"type": "refused", "reason": "one-per-visitor"}
            async with connect(s.url(), additional_headers=h("10.0.0.4")) as c:
                assert (await control(c))["type"] == "hello"


@pytest.mark.asyncio
async def test_total_cap(env):
    async with Server(env, max_vms=2, per_ip=5) as s:
        async with connect(s.url(), additional_headers=h("10.1.0.1")) as a, \
                connect(s.url(), additional_headers=h("10.1.0.2")) as b:
            await control(a)
            await control(b)
            async with connect(s.url(), additional_headers=h("10.1.0.3")) as c:
                assert await control(c) == {"type": "refused", "reason": "busy"}


@pytest.mark.asyncio
async def test_time_cap_ends_session(env):
    async with Server(env, max_seconds=1.5) as s:
        async with connect(s.url(), additional_headers=h("10.2.0.1")) as ws:
            await control(ws)
            await read_until(ws, b"qsh> ")
            await ws.send("hang\n")
            msg = await control(ws, timeout=6)
            assert msg["type"] == "expired"
        await asyncio.sleep(0.5)
        assert sessions(env["run"]) == [] and not fake_pids(env["run"])


@pytest.mark.asyncio
async def test_guest_crash_cleans_up(env):
    async with Server(env) as s:
        async with connect(s.url(), additional_headers=h("10.3.0.1")) as ws:
            await control(ws)
            await read_until(ws, b"qsh> ")
            await ws.send("crash\n")
            assert (await control(ws))["type"] == "exited"
        await asyncio.sleep(0.5)
        assert sessions(env["run"]) == [] and not fake_pids(env["run"])


@pytest.mark.asyncio
async def test_input_frame_is_capped(env):
    async with Server(env) as s:
        async with connect(s.url(), additional_headers=h("10.4.0.1")) as ws:
            await control(ws)
            await read_until(ws, b"qsh> ")
            await ws.send("echo " + "A" * 2995)        # 3000 bytes, no newline yet
            await ws.send("\n")
            out = await read_until(ws, b"A\n")
            run = max((len(x) for x in out.split(b"\n") if set(x) == {ord("A")}), default=0)
            assert run == qos_console.MAX_INPUT - len("echo "), run


@pytest.mark.asyncio
async def test_both_directions_ending_together_leaves_no_unretrieved_exception():
    """Deterministic version of a race seen live: the visitor closes while the guest is
    printing, so BOTH bridge tasks finish, one with ConnectionClosedOK. Every outcome must
    be retrieved; an unretrieved one is reported when the task is garbage-collected."""
    import gc
    from websockets.exceptions import ConnectionClosedOK
    from websockets.frames import Close

    loop = asyncio.get_running_loop()
    reported = []
    old = loop.get_exception_handler()
    loop.set_exception_handler(lambda l, ctx: reported.append(ctx.get("message", "")))
    try:
        async def returns(v):
            return v

        async def raises_closed():
            raise ConnectionClosedOK(Close(1000, ""), Close(1000, ""), True)

        done = {asyncio.ensure_future(returns("disconnect")), asyncio.ensure_future(raises_closed())}
        await asyncio.wait(done)
        assert qos_console.end_reason_of(done) == "disconnect"
        done = None
        gc.collect()
        await asyncio.sleep(0)
    finally:
        loop.set_exception_handler(old)
    assert not [m for m in reported if "never retrieved" in m], reported


@pytest.mark.asyncio
async def test_guest_exit_wins_over_disconnect():
    async def returns(v):
        return v
    done = {asyncio.ensure_future(returns("disconnect")), asyncio.ensure_future(returns("guest-exited"))}
    await asyncio.wait(done)
    assert qos_console.end_reason_of(done) == "guest-exited"


@pytest.mark.asyncio
async def test_stale_dirs_swept_on_start(env):
    (env["run"] / "s-deadbeef0000").mkdir(parents=True)
    (env["run"] / "keep-me").mkdir()
    async with Server(env):
        assert sessions(env["run"]) == []
        assert (env["run"] / "keep-me").exists()


@pytest.mark.asyncio
async def test_log_has_no_keystrokes_or_addresses(env, caplog):
    caplog.set_level("INFO", logger="qos-console")
    secret = "my-secret-typed-words"
    async with Server(env) as s:
        async with connect(s.url(), additional_headers=h("203.0.113.77")) as ws:
            await control(ws)
            await read_until(ws, b"qsh> ")
            await ws.send(f"echo {secret}\n")
            await read_until(ws, secret.encode())
        await asyncio.sleep(0.3)
    text = caplog.text
    assert "start sid=" in text and "end sid=" in text and "duration_s=" in text
    assert secret not in text, "typed input must never reach the log"
    assert "203.0.113.77" not in text, "raw client addresses must never reach the log"
