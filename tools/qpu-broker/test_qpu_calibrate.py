"""Tests for the daily calibration run (#275). The broker is either a scripted fake answer
list or the real broker over its Unix socket with a fake qBraid backend, so nothing is
billed and no credentials are needed.

Run: python3 -m pytest -q tools/qpu-broker
"""
from __future__ import annotations

import asyncio
import json
import sys
import threading
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qpu_broker as qb  # noqa: E402
import qpu_calibrate as cal  # noqa: E402
from test_qpu_broker import DEVICES, FakeBackend  # noqa: E402

RIG = "rigetti:rigetti:qpu:cepheus-1-108q"


def args(tmp_path, *extra):
    return cal.build_parser().parse_args(["--socket", str(tmp_path / "b.sock"),
                                          "--series", str(tmp_path / "c.jsonl"),
                                          "--json", str(tmp_path / "c.json"),
                                          "--poll-seconds", "0", *extra])


def job(state, device=RIG, counts=None, cost=None, jid="q0123456789"):
    return {"ok": True, "job": jid, "kind": "qpu", "device": device, "provider_job": "pj-" + jid,
            "state": state, "shots": 20, "n_qubits": 2, "est_cost": 4.992, "cost": cost,
            "cost_source": "billed" if cost is not None else None, "counts": counts,
            "submitted": "2026-10-25T00:05:01Z", "finished": None, "detail": None}


def script(monkeypatch, answers):
    """Replace the socket call with a list of answers (or exceptions), recording requests."""
    seen = []

    def fake(sock, req, timeout=300.0):
        seen.append(req)
        a = answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return a
    monkeypatch.setattr(cal, "ask", fake)
    return seen


def rows(tmp_path):
    return [json.loads(x) for x in (tmp_path / "c.jsonl").read_text().splitlines()]


def test_a_done_day_appends_one_row_with_counts_cost_and_fraction(tmp_path, monkeypatch):
    seen = script(monkeypatch, [job("queued"), job("running"),
                                job("done", counts={"00": 9, "11": 10, "01": 1}, cost=4.1724)])
    assert cal.run(args(tmp_path), sleep=lambda s: None) == 0
    assert seen[0] == {"op": "calibrate", "sim": False} and seen[1]["op"] == "job"
    (r,) = rows(tmp_path)
    assert r["state"] == "done" and r["device"] == RIG and r["series"] == 1 and not r["device_changed"]
    assert r["counts"] == {"00": 9, "11": 10, "01": 1} and r["shots"] == 20
    assert r["fraction_00_11"] == 0.95 and r["credits_billed"] == 4.1724
    assert r["provider_job"] == "pj-q0123456789" and r["broker_job"] == "q0123456789"
    doc = json.loads((tmp_path / "c.json").read_text())
    assert doc["rows"] == [r] and "mira_muse" in doc["idea"]
    # the same day again: nothing is asked, nothing appended
    script(monkeypatch, [])
    assert cal.run(args(tmp_path)) == 0
    assert len(rows(tmp_path)) == 1


def test_a_refused_day_is_a_row_not_a_gap(tmp_path, monkeypatch):
    script(monkeypatch, [{"ok": False, "error": "the cheapest suitable QPU is estimated at 8.64 credits "
                                                "for 100 shots, over the 5-credit per-job cap"}])
    assert cal.run(args(tmp_path)) == 1
    (r,) = rows(tmp_path)
    assert r["state"] == "refused" and "per-job cap" in r["reason"] and r["counts"] is None
    assert r["credits_billed"] is None


def test_an_unreachable_broker_is_an_error_row(tmp_path, monkeypatch):
    script(monkeypatch, [FileNotFoundError("no socket")])
    assert cal.run(args(tmp_path)) == 1
    (r,) = rows(tmp_path)
    assert r["state"] == "error" and "broker unreachable" in r["reason"]


def test_a_failed_job_and_a_timeout_are_rows(tmp_path, monkeypatch):
    script(monkeypatch, [job("queued"), dict(job("failed"), detail="FAILED")])
    assert cal.run(args(tmp_path), sleep=lambda s: None) == 1
    assert rows(tmp_path)[0]["state"] == "failed" and rows(tmp_path)[0]["reason"] == "FAILED"

    t = [0.0]

    def clock():
        t[0] += 3600
        return 86400 * 20400 + t[0]       # a later day, so the first row does not count
    script(monkeypatch, [job("queued")] + [job("queued")] * 30)
    assert cal.run(args(tmp_path, "--deadline-hours", "3"), sleep=lambda s: None, clock=clock) == 1
    r = rows(tmp_path)[-1]
    assert r["state"] == "timeout" and r["provider_job"] and "qpu job q0123456789" in r["reason"]


def test_a_poll_error_keeps_waiting(tmp_path, monkeypatch):
    script(monkeypatch, [job("queued"), ConnectionRefusedError("broker restarting"),
                         job("done", counts={"00": 10, "11": 10}, cost=4.2)])
    assert cal.run(args(tmp_path), sleep=lambda s: None) == 0
    assert rows(tmp_path)[0]["fraction_00_11"] == 1.0


def test_a_device_change_starts_a_new_series(tmp_path):
    prev = []
    r1 = cal.make_row("2026-10-25", "done", job("done", counts={"00": 10, "11": 10}), prev)
    prev.append(r1)
    r2 = cal.make_row("2026-10-26", "refused", None, prev, "budget")
    prev.append(r2)
    r3 = cal.make_row("2026-10-27", "done", job("done", device="aws:other:qpu:x", counts={"00": 20}), prev)
    prev.append(r3)
    r4 = cal.make_row("2026-10-28", "done", job("done", device="aws:other:qpu:x", counts={"00": 20}), prev)
    assert (r1["series"], r1["device_changed"]) == (1, False)
    assert (r2["series"], r2["device"]) == (1, None)
    assert (r3["series"], r3["device_changed"]) == (2, True)
    assert (r4["series"], r4["device_changed"]) == (2, False)


def test_the_series_file_is_only_appended_to(tmp_path, monkeypatch):
    series = tmp_path / "c.jsonl"
    series.write_text('{"date": "2000-01-01", "state": "done", "device": "d", "series": 1}\n')
    before = series.read_bytes()
    script(monkeypatch, [{"ok": False, "error": "no"}])
    cal.run(args(tmp_path))
    assert series.read_bytes().startswith(before) and len(rows(tmp_path)) == 2


def test_end_to_end_over_the_broker_socket_on_the_simulator(tmp_path):
    if sys.platform == "win32":
        pytest.skip("Unix sockets")
    be = FakeBackend(DEVICES)
    orig_submit = be.submit

    def submit(dev, qasm, shots):          # the fake simulator finishes at once
        pj = orig_submit(dev, qasm, shots)
        be.polls[pj] = {"state": "done", "counts": {"00": 11, "11": 9}, "cost": 0.0}
        return pj
    be.submit = submit
    ready = threading.Event()
    box = {}

    def serve():
        async def go():
            box["stop"] = asyncio.get_running_loop().create_future()
            box["loop"] = asyncio.get_running_loop()
            a = qb.build_parser().parse_args(["--socket", str(tmp_path / "b.sock"), "--db", str(tmp_path / "d.db")])
            t = asyncio.create_task(qb.amain(a, backend=be, stop=box["stop"]))
            while not (tmp_path / "b.sock").exists():
                await asyncio.sleep(0.01)
            ready.set()
            await t
        asyncio.run(go())
    th = threading.Thread(target=serve)
    th.start()
    try:
        assert ready.wait(5)
        assert cal.run(args(tmp_path, "--sim")) == 0
        assert cal.run(args(tmp_path, "--sim")) == 0       # sim runs are not limited to one a day
    finally:
        box["loop"].call_soon_threadsafe(lambda: box["stop"].done() or box["stop"].set_result(None))
        th.join(5)
    rs = rows(tmp_path)
    assert len(rs) == 2 and rs[0]["device"] == "qbraid:qbraid:sim:qir-sv" and rs[0]["shots"] == 20
    assert rs[0]["counts"] == {"00": 11, "11": 9} and rs[0]["fraction_00_11"] == 1.0
    assert be.submitted == [("qbraid:qbraid:sim:qir-sv", 20)] * 2
