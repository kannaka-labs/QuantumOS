"""Tests for the host quantum broker (#262). The qBraid SDK is replaced by a fake backend,
so these run in CI with no credentials and spend nothing.

Run: python3 -m pytest -q tools/qpu-broker
"""
from __future__ import annotations

import asyncio
import cmath
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qpu_broker as qb  # noqa: E402
from qpu_broker import Broker, Device, Policy, Refused, Store  # noqa: E402

qc = qb.qc


# ---- a tiny exact state-vector check of the QASM translation ---------------------

def simulate_qasm2(text: str) -> list[float]:
    """Probabilities (index = sum c[i] << i) of the qelib1 subset to_qasm2 emits."""
    n = int(text.split("qreg q[")[1].split("]")[0])
    st = [0j] * (1 << n)
    st[0] = 1
    r2 = 1 / 2 ** 0.5

    def one(q, m):
        for i in range(1 << n):
            if not (i >> q) & 1:
                j = i | (1 << q)
                a, b = st[i], st[j]
                st[i], st[j] = m[0][0] * a + m[0][1] * b, m[1][0] * a + m[1][1] * b

    def phase_if(pred, ph):
        for i in range(1 << n):
            if pred(i):
                st[i] *= ph

    for line in text.splitlines():
        g, _, args = line.partition(" ")
        qs = [int(x.split("[")[1].rstrip("];")) for x in args.split(",") if "[" in x]
        if g == "h":
            one(qs[0], [[r2, r2], [r2, -r2]])
        elif g == "x":
            one(qs[0], [[0, 1], [1, 0]])
        elif g == "z":
            phase_if(lambda i: (i >> qs[0]) & 1, -1)
        elif g == "s":
            phase_if(lambda i: (i >> qs[0]) & 1, 1j)
        elif g == "cz":
            phase_if(lambda i: (i >> qs[0]) & 1 and (i >> qs[1]) & 1, -1)
        elif g in ("cx", "ccx"):
            *ctl, t = qs
            for i in range(1 << n):
                if all((i >> c) & 1 for c in ctl) and not (i >> t) & 1:
                    j = i | (1 << t)
                    st[i], st[j] = st[j], st[i]
    return [abs(a) ** 2 for a in st]


def test_qasm_translation_matches_the_exact_engine():
    p = simulate_qasm2(qb.to_qasm2(qc.bell())[1])
    assert p == pytest.approx([0.5, 0, 0, 0.5])
    p = simulate_qasm2(qb.to_qasm2(qc.ghz(3))[1])
    assert p[0] == pytest.approx(0.5) and p[7] == pytest.approx(0.5)
    p = simulate_qasm2(qb.to_qasm2(qc.grover(3, 5, 2))[1])
    assert p[5] == pytest.approx(121 / 128)          # the exact engine's grover3 answer
    p = simulate_qasm2(qb.to_qasm2(qc.grover(2, 2, 1))[1])
    assert p[2] == pytest.approx(1.0)
    # asymmetric: X on qubit 0 of 2 -> index 1 (qubit 0 = least significant bit)
    p = simulate_qasm2(qb.to_qasm2(qc.build(2, [(qc.QC_OP_X, 0, 0)]))[1])
    assert p[1] == pytest.approx(1.0)
    assert cmath.isclose(sum(p), 1)


def test_translation_refuses_what_it_cannot_do_exactly():
    with pytest.raises(Refused):
        qb.to_qasm2(qc.grover(4, 3, 1))
    with pytest.raises(Refused):
        qb.to_qasm2(qc.build(2, [(qc.QC_OP_CNOT, 1, 1)]))
    with pytest.raises(Refused):
        qb.to_qasm2(qc.build(2, [(qc.QC_OP_H, 5, 0)]))
    with pytest.raises(Refused):
        qb.to_qasm2(qc.build(2, [(99, 0, 0)]))


# ---- the broker with a fake qBraid ----------------------------------------------

class FakeBackend:
    def __init__(self, devices, status=None):
        self.devs = devices
        self.st = status or {}
        self.submitted: list[tuple[str, int]] = []
        self.polls: dict[str, dict] = {}
        self.status_calls: list[str] = []

    def devices(self):
        return [Device(d.id, d.simulator, d.qubits, d.per_task, d.per_shot, d.per_minute, qasm=d.qasm)
                for d in self.devs]

    def status(self, dev):
        self.status_calls.append(dev.id)
        return self.st.get(dev.id, "ONLINE")

    def submit(self, dev, qasm, shots):
        self.submitted.append((dev.id, shots))
        pj = f"pj-{len(self.submitted)}"
        self.polls[pj] = {"state": "queued"}
        return pj

    def poll(self, pj):
        return self.polls[pj]


DEVICES = [
    Device("qbraid:qbraid:sim:qir-sv", True, 30, 0, 0, 0),
    Device("aws:aws:sim:sv1", True, 34, 0, 0, 7.5),                       # not free
    Device("aws:iqm:qpu:garnet", False, 20, 30, 0.145, 0),
    Device("rigetti:rigetti:qpu:cepheus-1-108q", False, 107, 0, 0, 12000),
    Device("openquantum:iqm:qpu:garnet", False, 20, 0, 0, 0),            # billed elsewhere
    Device("azure:ionq:qpu:forte-1", False, 36, None, None, None),       # no price
    Device("azure:pasqal:qpu:fresnel", False, 100, 0, 0, 500, qasm=False),  # analog: no OpenQASM
]


def make(tmp_path, devices=DEVICES, status=None, **pol):
    be = FakeBackend(devices, status)
    return Broker(be, Store(str(tmp_path / "b.db")), Policy(**pol)), be


def sub(mode="sim", circuit=None, shots=0, sid="s1", client="c1"):
    return {"op": "submit", "mode": mode, "shots": shots, "sid": sid, "client": client,
            "circuit": (circuit or qc.bell()).hex()}


def run(coro):
    return asyncio.run(coro)


def test_simulator_is_the_default_and_free(tmp_path):
    b, be = make(tmp_path)
    a = run(b.handle(sub()))
    assert a["ok"] and a["kind"] == "sim" and a["device"] == "qbraid:qbraid:sim:qir-sv"
    assert a["state"] == "queued" and a["shots"] == 100 and a["est_cost"] == 0
    assert a["provider_job"] == "pj-1" and a["job"].startswith("q")
    be.polls["pj-1"] = {"state": "done", "counts": {"00": 51, "11": 49}, "cost": 0.0}
    j = run(b.handle({"op": "job", "id": a["job"]}))
    assert j["state"] == "done" and j["counts"] == {"00": 51, "11": 49}
    assert j["cost"] == 0 and j["cost_source"] == "billed" and j["finished"]


def test_real_qpu_off_by_default(tmp_path):
    b, be = make(tmp_path)
    a = run(b.handle(sub("qpu")))
    assert not a["ok"] and "switched off" in a["error"]
    assert be.submitted == []


def test_real_picks_cheapest_online_priced_qpu(tmp_path):
    b, be = make(tmp_path, real=True)
    a = run(b.handle(sub("qpu")))       # default shots
    assert a["ok"], a
    # Rigetti bills 12000 credits/min = 0.2 credits/ms; (20 x 0.4 + 5) ms x 1.5 = 19.5 ms
    # -> 3.9 credits, under the 5-credit cap; Garnet's 30-credit task fee is not.
    assert a["device"] == "rigetti:rigetti:qpu:cepheus-1-108q"
    assert a["est_cost"] == pytest.approx(3.9)
    assert be.submitted == [("rigetti:rigetti:qpu:cepheus-1-108q", 20)]
    assert "openquantum:iqm:qpu:garnet" not in be.status_calls       # zero price = unbounded, skipped
    assert "azure:ionq:qpu:forte-1" not in be.status_calls
    assert "azure:pasqal:qpu:fresnel" not in be.status_calls          # cheaper, but analog


def test_real_refused_when_estimate_over_cap(tmp_path):
    b, be = make(tmp_path, real=True)
    a = run(b.handle(sub("qpu", shots=100)))
    assert not a["ok"] and "estimated at 13.50 credits" in a["error"] and "per-job cap" in a["error"]
    assert be.submitted == []


def test_offline_device_is_skipped(tmp_path):
    b, be = make(tmp_path, status={"rigetti:rigetti:qpu:cepheus-1-108q": "OFFLINE"}, real=True)
    a = run(b.handle(sub("qpu", shots=10)))
    assert not a["ok"]          # Garnet's 30-credit task fee is over the cap, so nothing fits
    assert be.submitted == []


def test_one_real_job_per_session_and_client_and_daily_budget(tmp_path):
    b, be = make(tmp_path, real=True, budget_per_day=6.0, real_per_client_per_day=5)
    a = run(b.handle(sub("qpu", shots=10, sid="s1", client="c1")))
    assert a["ok"]
    est1 = a["est_cost"]
    a = run(b.handle(sub("qpu", shots=10, sid="s1", client="c2")))
    assert not a["ok"] and "per session" in a["error"]
    a = run(b.handle(sub("qpu", shots=10, sid="s2", client="c2")))
    assert a["ok"]                       # 2 x 2.7 = 5.4 of 6
    a = run(b.handle(sub("qpu", shots=10, sid="s3", client="c3")))
    assert not a["ok"] and "budget" in a["error"]
    assert len(be.submitted) == 2
    # the billed cost replaces the estimate in the day's spend
    be.polls["pj-1"] = {"state": "done", "counts": {"00": 5, "11": 5}, "cost": 0.5}
    run(b.handle({"op": "job", "id": run(_first_id(b))}))
    assert b.store.spent(qb.utc_day()) == pytest.approx(0.5 + est1)
    a = run(b.handle(sub("qpu", shots=10, sid="s4", client="c4")))
    assert a["ok"]


async def _first_id(b):
    return b.store.q("SELECT id FROM jobs ORDER BY submitted LIMIT 1")[0][0]


def test_one_real_job_per_client_per_day(tmp_path):
    b, _ = make(tmp_path, real=True)
    assert run(b.handle(sub("qpu", shots=10, sid="s1", client="c1")))["ok"]
    a = run(b.handle(sub("qpu", shots=10, sid="s2", client="c1")))
    assert not a["ok"] and "per visitor" in a["error"]


def test_validation(tmp_path):
    b, be = make(tmp_path, real=True)
    bad = [
        {"op": "submit", "mode": "sim", "sid": "s", "circuit": "zz"},
        {"op": "submit", "mode": "sim", "sid": "s", "circuit": "0102"},
        {"op": "submit", "mode": "sim", "sid": "s", "circuit": qc.bell().hex(), "shots": 5000},
        {"op": "submit", "mode": "qpu", "sid": "s", "circuit": qc.bell().hex(), "shots": 101},
        {"op": "submit", "mode": "qpu", "sid": "s", "circuit": qc.ghz(6).hex()},
        {"op": "submit", "mode": "warp", "sid": "s", "circuit": qc.bell().hex()},
        {"op": "submit", "mode": "sim", "circuit": qc.bell().hex()},           # no session
        {"op": "submit", "mode": "sim", "sid": "s", "circuit": qc.bell().hex(), "shots": "9"},
        {"op": "job", "id": "../etc"},
        {"op": "launch"},
    ]
    for req in bad:
        a = run(b.handle(req))
        assert not a["ok"], req
    assert be.submitted == []


def test_simulator_session_limit(tmp_path):
    b, _ = make(tmp_path, sim_per_session=2)
    assert run(b.handle(sub()))["ok"]
    assert run(b.handle(sub()))["ok"]
    a = run(b.handle(sub()))
    assert not a["ok"] and "simulator limit" in a["error"]


def test_failed_submit_keeps_the_estimate_reserved(tmp_path):
    b, be = make(tmp_path, real=True)

    def boom(*a):
        raise RuntimeError("vendor said no")
    be.submit = boom
    a = run(b.handle(sub("qpu", shots=10)))
    assert not a["ok"] and "refused the job" in a["error"]
    assert b.store.spent(qb.utc_day()) > 0


def test_info_and_socket_protocol(tmp_path):
    if sys.platform == "win32":
        pytest.skip("Unix sockets")

    async def go():
        be = FakeBackend(DEVICES)
        stop = asyncio.get_running_loop().create_future()
        a = qb.build_parser().parse_args(["--socket", str(tmp_path / "s.sock"), "--db", str(tmp_path / "d.db"),
                                          "--real", "--budget-per-day", "10"])
        task = asyncio.create_task(qb.amain(a, backend=be, stop=stop))
        for _ in range(100):
            if (tmp_path / "s.sock").exists():
                break
            await asyncio.sleep(0.02)
        r, w = await asyncio.open_unix_connection(str(tmp_path / "s.sock"))
        w.write(b'{"op": "info", "sid": "x"}\nnot json\n' + b"x" * 5000 + b"\n")
        await w.drain()
        info = json.loads(await r.readline())
        assert info["ok"] and info["real"] and info["budget_per_day"] == 10 and info["real_left_this_session"] == 1
        assert json.loads(await r.readline()) == {"ok": False, "error": "bad request"}
        w.close()
        stop.set_result(None)
        await task
    run(go())
