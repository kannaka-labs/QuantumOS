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
import re
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

    async def _get_health(self, request: bytes = HEALTH_REQUEST) -> tuple[bytes, bytes]:
        r, w = await asyncio.open_connection("127.0.0.1", self.port)
        w.write(request)
        await w.drain()
        raw = await asyncio.wait_for(r.read(), 5)
        w.close()
        await w.wait_closed()
        head, _, body = raw.partition(CRLF + CRLF)
        return head.split(CRLF, 1)[0], body

    async def get(self, path: str, headers: dict | None = None) -> tuple[bytes, bytes]:
        lines = [f"GET {path} HTTP/1.1".encode(), b"Host: t", b"Connection: close"]
        lines += [f"{k}: {v}".encode() for k, v in (headers or {}).items()]
        return await self._get_health(CRLF.join(lines + [b"", b""]))

    async def health(self) -> dict:
        _, body = await self._get_health()
        return json.loads(body)

    async def wait_snapshot(self, kernel: str | None = None, timeout=10.0) -> dict:
        """Wait until /health reports a usable snapshot (of `kernel`, a 12-hex sha, if given)."""
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            hl = await self.health()
            if hl["snapshot"] and (kernel is None or hl["snapshot_kernel"] == kernel):
                return hl
            await asyncio.sleep(0.05)
        raise AssertionError(f"no snapshot after {timeout} s: {hl}")

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


def fake_pids(run: Path, builders: bool = False) -> set[int]:
    """PIDs of hypervisors whose config or API socket lives under THIS test's run dir.
    Matching on the run dir (not on the binary name) keeps a host's real, long-running
    Firecracker VMs out of the count. The snapshot builder (under run/base-*) may still be
    booting while a first session runs, so it only counts with builders=True."""
    out = set()
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        try:
            cmd = (p / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if (b"--config-file" in cmd or b"--api-sock" in cmd) and any(str(run).encode() in c for c in cmd):
            if builders or not any(f"{run}/base-".encode() in c for c in cmd):
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
    async with Server(env, snapshot="off") as s:
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


# ---- snapshot restore ------------------------------------------------------------

def ksha(path) -> str:
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()[:12]


def bases(run: Path) -> list[Path]:
    return sorted(d for d in run.iterdir() if d.name.startswith("base-")) if run.exists() else []


@pytest.mark.asyncio
async def test_session_restores_from_snapshot_and_replays_boot_transcript(env):
    async with Server(env) as s:
        await s.wait_snapshot(ksha(env["kernel"]))
        async with connect(s.url(), additional_headers=h("10.5.0.1")) as ws:
            hello = await control(ws)
            assert hello["type"] == "hello" and hello["restored"] is True
            out = await read_until(ws, b"qsh> ")      # replayed transcript: banner + prompt
            assert f"kernel={ksha(env['kernel'])}".encode() in out
            await ws.send("whoami\r")
            assert f"kernel={ksha(env['kernel'])}".encode() in await read_until(ws, b"kernel=")
            sdir = env["run"] / sessions(env["run"])[0]
            assert not (sdir / "kernel.elf").exists(), "a restored session needs no kernel copy"
        await asyncio.sleep(0.5)
        assert sessions(env["run"]) == [] and not fake_pids(env["run"])
        assert len(bases(env["run"])) == 1, "the snapshot outlives the session"


@pytest.mark.asyncio
async def test_each_restored_session_is_reseeded_before_the_visitor_types(env):
    async with Server(env, per_ip=2) as s:
        await s.wait_snapshot()
        seeds = []
        for ip in ("10.8.0.1", "10.8.0.2"):
            async with connect(s.url(), additional_headers=h(ip)) as ws:
                assert (await control(ws))["restored"]
                await read_until(ws, b"qsh> ")
                await ws.send("qseed\r")
                out = await read_until(ws, b"qsh> ")
                m = re.search(rb"qseed: ([0-9a-f,]*) late=(\w+)", out)
                assert m, out
                got = m.group(1).decode().split(",")
                assert len(got) == 1 and len(got[0]) == 16, got       # exactly one reseed
                assert m.group(2) == b"False", "the visitor's input reached the guest first"
                seeds.append(got[0])
        assert seeds[0] != seeds[1], "two visitors got the same session seed"


def test_reseed_escape_format():
    from qos_console import reseed_escape
    assert reseed_escape(bytes.fromhex("0123456789abcdef")) == b"\x1b]qseed;0123456789abcdef\x07"
    with pytest.raises(AssertionError):
        reseed_escape(bytes(8))


@pytest.mark.asyncio
async def test_restored_sessions_are_isolated_and_never_write_the_snapshot(env):
    async with Server(env, per_ip=2) as s:
        await s.wait_snapshot()
        mem = bases(env["run"])[0] / "vm.mem"
        before = mem.read_bytes()
        async with connect(s.url(), additional_headers=h("10.6.0.1")) as a, \
                connect(s.url(), additional_headers=h("10.6.0.2")) as b:
            assert (await control(a))["restored"] and (await control(b))["restored"]
            await read_until(a, b"qsh> ")
            await read_until(b, b"qsh> ")
            await a.send("imprint visitor-a-secret\n")
            await read_until(a, b"qsh> ")
            await b.send("recall\n")
            out = await read_until(b, b"recall:")
            assert b"visitor-a-secret" not in out, "one visitor's memory leaked into another's VM"
            await a.send("recall\n")
            assert b"visitor-a-secret" in await read_until(a, b"visitor-a-secret")
        await asyncio.sleep(0.5)
        assert mem.read_bytes() == before, "a session wrote to the shared snapshot memory"
        assert sessions(env["run"]) == [] and not fake_pids(env["run"])


@pytest.mark.asyncio
async def test_kernel_change_rebuilds_snapshot_and_drops_the_old_one(env):
    async with Server(env) as s:
        old = ksha(env["kernel"])
        await s.wait_snapshot(old)
        Path(env["kernel"]).write_bytes(b"\x7fELF a newer fake kernel")
        new = ksha(env["kernel"])
        # The next session sees the new kernel either way: cold boot while the new
        # snapshot builds, or restored from it once it exists. Never the old snapshot.
        async with connect(s.url(), additional_headers=h("10.7.0.1")) as ws:
            await control(ws)
            await read_until(ws, b"qsh> ")
            await ws.send("whoami\n")
            assert f"kernel={new}".encode() in await read_until(ws, b"kernel=")
        await s.wait_snapshot(new)
        await asyncio.sleep(0.3)
        assert [d.name.split("-")[1] for d in bases(env["run"])] == [new], "old snapshot not deleted"
        async with connect(s.url(), additional_headers=h("10.7.0.2")) as ws:
            assert (await control(ws))["restored"] is True
            await ws.send("whoami\n")
            assert f"kernel={new}".encode() in await read_until(ws, f"kernel={new}".encode())


@pytest.mark.asyncio
async def test_failed_restore_falls_back_to_cold_boot(env):
    async with Server(env) as s:
        await s.wait_snapshot()
        (bases(env["run"])[0] / "vm.mem").unlink()      # the load now fails inside the VMM
        async with connect(s.url(), additional_headers=h("10.8.0.1")) as ws:
            assert (await control(ws))["restored"] is False
            await read_until(ws, b"Welcome to QuantumOS")
            await ws.send("echo still-works\n")
            await read_until(ws, b"still-works")
        await asyncio.sleep(0.5)
        assert sessions(env["run"]) == [] and not fake_pids(env["run"])


@pytest.mark.asyncio
async def test_snapshot_off_cold_boots(env):
    async with Server(env, snapshot="off") as s:
        async with connect(s.url(), additional_headers=h("10.9.0.1")) as ws:
            assert (await control(ws))["restored"] is False
            await read_until(ws, b"qsh> ")
        assert (await s.health())["snapshot"] is False and bases(env["run"]) == []


@pytest.mark.asyncio
async def test_snapshot_removed_on_shutdown(env):
    async with Server(env) as s:
        await s.wait_snapshot()
        assert len(bases(env["run"])) == 1
        assert not fake_pids(env["run"], builders=True), "the builder VM must exit once it has snapshotted"
    assert bases(env["run"]) == [] and not fake_pids(env["run"], builders=True)


@pytest.mark.asyncio
async def test_shutdown_mid_build_leaves_no_vm_or_dir(env):
    async with Server(env):
        pass                                             # stop before the snapshot is ready
    await asyncio.sleep(0.2)
    assert bases(env["run"]) == [] and not fake_pids(env["run"], builders=True)


# ---- metrics (#258) ----------------------------------------------------------------

HEXID = r"\b(?=[0-9a-f]*[a-f])[0-9a-f]{12,}\b"   # a hash or session id, not a plain number


def prom_value(text: str, series: str) -> float:
    for line in text.splitlines():
        if line.startswith(series + " "):
            return float(line.split()[-1])
    raise AssertionError(f"{series} not in /metrics")


@pytest.mark.asyncio
async def test_metrics_count_the_funnel_without_personal_data(env, tmp_path):
    state = tmp_path / "metrics.json"
    async with Server(env, per_ip=1, state_file=state) as s:
        await s.wait_snapshot()
        for e in ("load", "load", "boot"):
            status, _ = await s.get(f"/beacon?e={e}", h("198.51.100.7"))
            assert b" 200 " in status
        status, _ = await s.get("/beacon?e=anything-else", h("198.51.100.7"))
        assert b" 400 " in status, "only the two page events count"
        async with connect(s.url(), additional_headers=h("198.51.100.7")) as ws:
            assert (await control(ws))["restored"]
            await read_until(ws, b"qsh> ")
            async with connect(s.url(), additional_headers=h("198.51.100.7")) as again:
                assert (await control(again))["type"] == "refused"
            await ws.send("echo typed-secret-words\r")
            await read_until(ws, b"typed-secret-words")
        await asyncio.sleep(0.5)

        _, body = await s.get("/stats")
        st = json.loads(body)
        tot = st["totals"]
        assert tot["page_loads"] == 2 and tot["boot_clicks"] == 1
        assert tot["sessions_started"] == {"restore": 1}
        assert tot["sessions_refused"] == {"one-per-visitor": 1}
        assert tot["boots_completed"] == {"restore": 1}
        assert tot["sessions_with_input"] == 1
        assert tot["sessions_ended"] == {"disconnect": 1}
        for k in ("first_output", "ready", "session"):
            assert st["timings_s"][k]["n"] == 1, k
        assert st["timings_s"]["first_output"]["p50"] <= st["timings_s"]["ready"]["p50"]
        (today,) = st["days"].values()
        assert today["unique_visitors"] == 1, "one address, one visitor"
        assert today["page_loads"] == 2 and today["sessions_started"] == 1

        status, prom = await s.get("/metrics")
        assert b" 200 " in status
        prom = prom.decode()
        assert prom_value(prom, 'qos_sessions_started_total{mode="restore"}') == 1
        assert prom_value(prom, 'qos_sessions_ended_total{reason="disconnect"}') == 1
        assert prom_value(prom, 'qos_sessions_refused_total{reason="one-per-visitor"}') == 1
        assert prom_value(prom, "qos_page_loads_total") == 2
        assert prom_value(prom, "qos_snapshot_ready") == 1
        assert prom_value(prom, "qos_ready_seconds_count") == 1
        assert prom_value(prom, 'qos_session_seconds_bucket{le="+Inf"}') == 1

        for text in (body.decode(), prom):
            assert "198.51.100.7" not in text and "typed-secret" not in text
            assert not re.search(HEXID, text), "no hashes or session ids in metrics"
    saved = state.read_text()
    assert '"page_loads"' in saved
    assert "198.51.100.7" not in saved and "typed-secret" not in saved
    assert not re.search(HEXID, saved), "no visitor hashes are persisted"


@pytest.mark.asyncio
async def test_metrics_survive_a_restart(env, tmp_path):
    state = tmp_path / "metrics.json"
    async with Server(env, state_file=state) as s:
        await s.get("/beacon?e=load", h("192.0.2.1"))
    async with Server(env, state_file=state) as s:
        await s.get("/beacon?e=load", h("192.0.2.1"))
        st = json.loads((await s.get("/qos/stats"))[1])     # the staging prefix answers too
        assert st["totals"]["page_loads"] == 2


def test_unique_visitors_reset_with_the_daily_salt():
    now = [1_800_000_000.0]
    m = qos_console.Metrics(clock=lambda: now[0])
    for ip in ("192.0.2.1", "192.0.2.1", "192.0.2.2"):
        m.event("load", ip)
    salt = m._salt
    now[0] += 86400
    m.event("load", "192.0.2.1")
    assert m._salt != salt, "the salt must change with the UTC day"
    days = sorted(m.days.items())
    assert [d["unique_visitors"] for _, d in days] == [2, 1]
    assert [d["page_loads"] for _, d in days] == [3, 1]
    assert len(m._seen) == 1, "yesterday's hashes are dropped"


def test_histogram_buckets_and_percentiles():
    hist = qos_console.Histogram()
    for v in (0.02, 0.3, 0.3, 700):
        hist.observe(v)
    lines = hist.prometheus("x")
    assert 'x_bucket{le="0.025"} 1' in lines and 'x_bucket{le="0.5"} 3' in lines
    assert 'x_bucket{le="600"} 3' in lines and 'x_bucket{le="+Inf"} 4' in lines
    assert hist.summary()["p50"] == 0.3 and hist.summary()["max"] == 700


# ---- daily QPU calibration series (#275), served read-only ------------------------

CALIB = {"what": "test", "rows": [
    {"date": "2026-10-25", "state": "done", "device": "rigetti:rigetti:qpu:cepheus-1-108q", "series": 1,
     "device_changed": False, "counts": {"00": 9, "11": 11}, "fraction_00_11": 1.0, "credits_billed": 4.1724,
     "provider_job": "pj-1", "reason": None},
    {"date": "2026-10-27", "state": "refused", "device": None, "series": 1, "device_changed": False,
     "counts": None, "fraction_00_11": None, "credits_billed": None, "provider_job": None,
     "reason": "over the 5-credit per-job cap"},
]}


def test_calibration_doc_marks_days_with_no_row():
    import datetime as dt
    p = Path(__file__).parent / "_calib_test.json"
    try:
        p.write_text(json.dumps(CALIB))
        doc = qos_console.calibration_doc(p, today=dt.date(2026, 10, 29))
        assert doc["missing_days"] == ["2026-10-26", "2026-10-28"]     # today is not missing yet
        t = qos_console.calibration_table(doc)
        assert "2026-10-26  MISSING" in t and "2026-10-28  MISSING" in t
        assert "1.00" in t and "4.1724" in t and "per-job cap" in t and "mira_muse" in t
        assert t.index("2026-10-25") < t.index("2026-10-26") < t.index("2026-10-27")
    finally:
        p.unlink(missing_ok=True)
    assert qos_console.calibration_doc(None) is None
    assert qos_console.calibration_doc(p) is None


@pytest.mark.asyncio
async def test_calibration_routes_serve_the_series_read_only(env, tmp_path):
    f = tmp_path / "calibration.json"
    async with Server(env, calibration_file=f) as s:
        status, _ = await s.get("/calibration.json")
        assert b" 404 " in status, "no file yet"
        f.write_text(json.dumps(CALIB))
        status, body = await s.get("/calibration.json")
        assert b" 200 " in status
        doc = json.loads(body)
        assert doc["rows"] == CALIB["rows"] and "missing_days" in doc
        status, body = await s.get("/calibration")
        assert b" 200 " in status and b"cepheus-1-108q" in body and b"frac" in body
    assert json.loads(f.read_text()) == CALIB, "serving never writes the file"
