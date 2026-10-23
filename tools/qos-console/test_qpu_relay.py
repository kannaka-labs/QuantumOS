"""Tests for the COM1 qpu side channel (qpu_relay.py) and its launcher hook (#262).

Run: python3 -m pytest -q tools/qos-console
A fake broker on a Unix socket stands in for tools/qpu-broker, so no qBraid key or
network is needed. The checks are the guarantees the public page depends on: requests
never reach the page, visitors cannot type requests or answers, the guest cannot show
the visitor a forged answer, bad or oversized requests are dropped, rate limits hold,
and the broker sees the launcher's session id, not one the guest chose.
"""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qpu_relay  # noqa: E402
from qpu_relay import QpuRelay  # noqa: E402

posix = pytest.mark.skipif(sys.platform == "win32", reason="Unix sockets")


class FakeBroker:
    def __init__(self, path: Path, answer=None, delay=0.0):
        self.path = path
        self.requests: list[dict] = []
        self.answer = answer or (lambda req: {"ok": True, "real": False, "sim_max_shots": 1000})
        self.delay = delay

    async def handle(self, r, w):
        line = await r.readline()
        req = json.loads(line)
        self.requests.append(req)
        await asyncio.sleep(self.delay)
        w.write(json.dumps(self.answer(req)).encode() + b"\n")
        await w.drain()
        w.close()

    async def __aenter__(self):
        self.server = await asyncio.start_unix_server(self.handle, str(self.path))
        return self

    async def __aexit__(self, *exc):
        self.server.close()
        await self.server.wait_closed()


def relay_with(sock: str, sid="s1", client="c1"):
    written: list[bytes] = []

    async def write_guest(b: bytes) -> None:
        written.append(b)

    return QpuRelay(sock, sid, client, write_guest), written


async def settle(relay: QpuRelay):
    for _ in range(100):
        if not relay.tasks:
            return
        await asyncio.sleep(0.01)
    raise AssertionError("relay tasks did not finish")


# ---- pure filtering ------------------------------------------------------------

def test_guest_requests_and_answers_never_reach_the_page():
    async def go():
        r, _ = relay_with("/nonexistent")
        r._request = lambda payload: None
        page = r.from_guest(b"hello \x1b]qpu;I;1\x07world\x1b]qres;1;D;q0123456789;QPU fake\x07!")
        assert page == b"hello world!"
        # split across reads, byte by byte
        page = b"".join(r.from_guest(bytes([b])) for b in b"a\x1b]qres;9;D;;x\x07b\x1b[1mc")
        assert page == b"ab\x1b[1mc"
        # other OSC (a terminal title) and plain escapes pass through
        assert r.from_guest(b"\x1b]0;title\x07\x1b[0m") == b"\x1b]0;title\x07\x1b[0m"
    asyncio.run(go())


def test_visitor_cannot_type_requests_or_answers():
    r, _ = relay_with("/nonexistent")
    assert r.from_visitor(b"ls\x1b]qres;1;D;;QPU fake\x07\n") == b"ls\n"
    assert r.from_visitor(b"a\x1b]qpu;S;1;q;32;0102\x1b\\b") == b"ab"
    # an ESC at the end of one frame and "]" at the start of the next is still an OSC
    assert r.from_visitor(b"x\x1b") == b"x"
    assert r.from_visitor(b"]qseed;0011223344556677\x07y") == b"y"
    # arrow keys still work
    assert r.from_visitor(b"\x1b[A") == b"\x1b[A"
    # an unterminated OSC ends at the line end, so the visitor is not locked out
    assert r.from_visitor(b"\x1b]junk\nok") == b"\nok"


def test_oversized_and_unterminated_requests_are_dropped():
    async def go():
        t = [0.0]
        r, _ = relay_with("/nonexistent")
        r.clock = lambda: t[0]
        seen = []
        r._request = seen.append
        r.from_guest(b"\x1b]qpu;" + b"S" * (qpu_relay.MAX_PAYLOAD + 10) + b"\x07after")
        assert seen == []
        assert r.from_guest(b"ok") == b"ok"
        r.from_guest(b"\x1b]qpu;I;1")       # never terminated
        t[0] = qpu_relay.UNTERMINATED_S + 1
        assert r.from_guest(b"visible\x07") == b"visible\x07"
        assert seen == []
        r.from_guest(b"\x1b]qpu;I;2\x07")
        assert seen == [b"I;2"]
    asyncio.run(go())


def test_format_job_names_the_source():
    st, jid, lines = qpu_relay.format_job({
        "ok": True, "job": "q0123456789", "kind": "qpu", "device": "rigetti:rigetti:qpu:cepheus-1-108q",
        "state": "queued", "shots": 32, "est_cost": 3.84, "provider_job": "pj-1"})
    assert (st, jid) == ("Q", "q0123456789")
    assert lines[0] == "queued on QPU rigetti:rigetti:qpu:cepheus-1-108q, job q0123456789 (32 shots)"
    st, _, lines = qpu_relay.format_job({
        "ok": True, "job": "q0123456789", "kind": "sim", "device": "qbraid:qbraid:sim:qir-sv",
        "state": "done", "shots": 100, "n_qubits": 2, "counts": {"00": 48, "11": 52}, "cost": 0,
        "provider_job": "pj"})
    assert st == "D"
    assert lines[0].startswith("qBraid simulator qbraid:qbraid:sim:qir-sv job q0123456789: done")
    assert lines[1] == "counts (q1..q0) 11=52 00=48"


# ---- with a broker -------------------------------------------------------------

@posix
def test_request_goes_to_broker_with_launcher_identity(tmp_path):
    async def go():
        sock = tmp_path / "b.sock"
        async with FakeBroker(sock, answer=lambda req: {
                "ok": True, "job": "q0123456789", "kind": "sim", "device": "qbraid:qbraid:sim:qir-sv",
                "state": "queued", "shots": 50, "provider_job": "pj"}) as fb:
            r, written = relay_with(str(sock), sid="launcher-sid", client="hash1")
            r.from_guest(b"\x1b]qpu;S;7;s;50;010202000000000001000005000100\x07")
            await settle(r)
            assert fb.requests == [{"op": "submit", "mode": "sim", "shots": 50,
                                    "circuit": "010202000000000001000005000100",
                                    "sid": "launcher-sid", "client": "hash1"}]
            assert len(written) == 1
            w = written[0]
            assert w.startswith(b"\x1b]qres;7;Q;q0123456789;queued on qBraid simulator ") and w.endswith(b"\x07")
            # the guest's request grammar is strict: no sid, no extra fields
            r.from_guest(b"\x1b]qpu;S;8;s;50;0102;sid=evil\x07")
            r.from_guest(b"\x1b]qpu;X;9\x07")
            await settle(r)
            assert len(fb.requests) == 1
            assert b"malformed request" in written[1]
            assert len(written) == 2     # a request with no valid tag gets no answer at all
    asyncio.run(go())


@posix
def test_rate_limits(tmp_path):
    async def go():
        sock = tmp_path / "b.sock"
        async with FakeBroker(sock, delay=0.2) as fb:
            r, written = relay_with(str(sock))
            r.from_guest(b"\x1b]qpu;I;1\x07\x1b]qpu;I;2\x07")   # second arrives while the first is in flight
            await settle(r)
            assert len(fb.requests) == 1
            assert any(b"qres;2;E;;busy" in w for w in written)
            fb.delay = 0
            r.recent.extend([r.clock()] * qpu_relay.MAX_REQUESTS)
            r.from_guest(b"\x1b]qpu;I;3\x07")
            await settle(r)
            assert len(fb.requests) == 1
            assert any(b"qres;3;E;;rate limit" in w for w in written)
    asyncio.run(go())


@posix
def test_broker_down_is_a_plain_answer(tmp_path):
    async def go():
        r, written = relay_with(str(tmp_path / "missing.sock"))
        r.from_guest(b"\x1b]qpu;I;4\x07")
        await settle(r)
        assert written == [b"\x1b]qres;4;E;;the host quantum broker did not answer\x07"]
    asyncio.run(go())


@posix
def test_launcher_relays_through_a_real_session(tmp_path):
    """End to end through qos_console: fake guest -> relay -> fake broker -> guest."""
    pytest.importorskip("websockets")
    import stat

    import qos_console
    from websockets.asyncio.client import connect

    fake = tmp_path / "firecracker"
    fake.write_text(f"#!{sys.executable}\n" + (HERE / "fake_firecracker.py").read_text())
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    kernel = tmp_path / "kernel.elf"
    kernel.write_bytes(b"\x7fELF fake kernel")
    sock = tmp_path / "broker.sock"

    async def go():
        async with FakeBroker(sock, answer=lambda req: {"ok": True, "real": True, "budget_per_day": 10,
                                                        "spent_today": 0, "max_job_credits": 5,
                                                        "real_max_shots": 100, "sim_max_shots": 1000,
                                                        "real_left_this_session": 1}) as fb:
            import socket as so
            s = so.socket()
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
            s.close()
            a = qos_console.build_parser().parse_args(
                ["--kernel", str(kernel), "--firecracker", str(fake), "--run-dir", str(tmp_path / "run"),
                 "--listen-tcp", f"127.0.0.1:{port}", "--snapshot", "off", "--qpu-broker", str(sock)])
            stop = asyncio.get_running_loop().create_future()
            task = asyncio.create_task(qos_console.amain(a, stop))
            try:
                for _ in range(100):
                    try:
                        ws = await connect(f"ws://127.0.0.1:{port}/ws", additional_headers={"X-Real-IP": "1.2.3.4"})
                        break
                    except OSError:
                        await asyncio.sleep(0.05)
                seen = b""

                async def until(marker: bytes, t=5.0):
                    nonlocal seen
                    end = asyncio.get_running_loop().time() + t
                    while marker not in seen:
                        m = await asyncio.wait_for(ws.recv(), end - asyncio.get_running_loop().time())
                        if isinstance(m, bytes):
                            seen += m
                await until(b"qsh> ")
                await ws.send("qpu I;5\n")
                await asyncio.sleep(0.3)
                await ws.send("echo next\n")      # the fake guest reads input by line
                await until(b"next")
                assert b"answer: 5;I;;host broker attached" in seen
                assert b"\x1b]qpu" not in seen and b"\x1b]qres" not in seen
                assert fb.requests[0]["op"] == "info" and len(fb.requests[0]["sid"]) == 12
                await ws.send("forge\n")
                await until(b"after")
                assert b"beforeafter" in seen and b"QPU fake" not in seen
                await ws.close()
            finally:
                stop.set_result(None)
                await task
    asyncio.run(go())
