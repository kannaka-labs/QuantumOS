"""Tests for the qBraid plugin (#286): the provider broker driving the REAL quantum broker
(tools/qpu-broker/qpu_broker.py) over its socket, with that broker's own fake qBraid backend.
So the money rules exercised here are the quantum broker's actual code, unchanged."""
from __future__ import annotations

import asyncio
import sys
import threading
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
for p in (HERE, ROOT / "tools" / "qpu-broker", ROOT / "scripts"):
    sys.path.insert(0, str(p))
import provider_broker as pb  # noqa: E402
import qpu_broker as qb  # noqa: E402
import qpu_circuit as qc  # noqa: E402
from plugin_qbraid import QbraidProvider  # noqa: E402
from test_qpu_broker import DEVICES, FakeBackend  # noqa: E402

BELL = qc.bell().hex()


class QuantumBroker:
    """The real quantum broker on a temp socket, in its own thread and event loop."""

    def __init__(self, tmp_path, **pol):
        self.sock = str(tmp_path / "qpu.sock")
        self.backend = FakeBackend(DEVICES)
        self.store = qb.Store(str(tmp_path / "qpu.db"))
        self.qb = qb.Broker(self.backend, self.store, qb.Policy(**pol))
        self.loop = asyncio.new_event_loop()
        self.ready = threading.Event()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()
        assert self.ready.wait(5)

    def _run(self):
        asyncio.set_event_loop(self.loop)

        async def main():
            self.server = await asyncio.start_unix_server(self.qb.serve_conn, self.sock)
            self.ready.set()
            async with self.server:
                await self.server.serve_forever()

        try:
            self.loop.run_until_complete(main())
        except asyncio.CancelledError:
            pass

    def close(self):
        def stop():
            self.server.close()
            for task in asyncio.all_tasks(self.loop):
                task.cancel()
        self.loop.call_soon_threadsafe(stop)
        self.t.join(5)
        self.qb.pool.shutdown(wait=False, cancel_futures=True)
        self.store.close()
        self.loop.close()


@pytest.fixture
def qbroker(tmp_path):
    made = []

    def make(**pol):
        q = QuantumBroker(tmp_path, **pol)
        made.append(q)
        return q
    yield make
    for q in made:
        q.close()


def broker(tmp_path, qsock, allow_spend=True, classes=("qpu",)):
    b = pb.Broker(pb.Ledger(str(tmp_path / "pb.db")), {"qbraid": QbraidProvider(qsock, timeout=10)},
                  allow_spend=allow_spend)
    key = b.set_envelope(pb.Envelope(per_day_usd=0.20, per_action_usd=0.10, classes=list(classes),
                                     max_lease_s=86400))
    return b, key


def test_simulator_job_is_free_and_runs_through_the_quantum_broker(tmp_path, qbroker):
    q = qbroker()
    b, _ = broker(tmp_path, q.sock, allow_spend=False)      # free, so allowed even with spending off
    quote = b.quote("qbraid", "qpu_job", {"mode": "sim", "circuit": BELL, "shots": 50}, 3600)
    assert quote["max_cost_usd"] == 0 and quote["exact"] is True and quote["within_envelope"]
    lease = b.provision(quote["quote"])
    job = lease["resource"]
    assert job.startswith("q") and len(job) == 11
    row = q.store.get(job)
    assert row["sid"] == f"qos-{b.broker_id}-{lease['lease']}" and row["client"] == "provider-broker"
    assert row["kind"] == "sim" and row["shots"] == 50
    st = b.status(lease["lease"])["provider_status"]
    assert st["state"] == "queued" and st["kind"] == "sim"
    q.backend.polls["pj-1"] = {"state": "done", "counts": {"00": 26, "11": 24}, "cost": 0.0}
    assert b.status(lease["lease"])["provider_status"]["counts"] == {"00": 26, "11": 24}
    out = b.teardown(lease["lease"])
    assert out["billed_usd"] == 0.0


def test_qpu_job_quoted_at_the_quantum_brokers_cap_and_billed_in_usd(tmp_path, qbroker):
    q = qbroker(real=True, budget_per_day=10, max_job_credits=5)
    b, key = broker(tmp_path, q.sock)
    quote = b.quote("qbraid", "qpu_job", {"mode": "qpu", "circuit": BELL, "shots": 20}, 3600)
    assert quote["max_cost_usd"] == pytest.approx(0.05) and quote["exact"] is False
    assert "upper bound" in quote["notes"]
    lease = b.provision(quote["quote"])
    row = q.store.get(lease["resource"])
    assert row["kind"] == "qpu" and row["device"].startswith("rigetti:")    # the quantum broker chose
    q.backend.polls["pj-1"] = {"state": "done", "counts": {"00": 9, "11": 11}, "cost": 4.1724}
    out = b.teardown(lease["lease"])
    assert out["billed_usd"] == pytest.approx(0.041724) and out["est_cost_usd"] == pytest.approx(0.05)


def test_the_quantum_brokers_rules_still_apply(tmp_path, qbroker):
    q = qbroker()                                          # real QPUs off on the quantum broker
    b, _ = broker(tmp_path, q.sock)
    with pytest.raises(pb.Refused) as e:
        b.quote("qbraid", "qpu_job", {"mode": "qpu", "circuit": BELL}, 3600)
    assert e.value.code == "qpu_off"
    q2 = qbroker(real=True, real_per_client_per_day=1)
    b2 = pb.Broker(pb.Ledger(str(tmp_path / "pb2.db")), {"qbraid": QbraidProvider(q2.sock, timeout=10)},
                   allow_spend=True)
    b2.set_envelope(pb.Envelope(1.0, 1.0, ["qpu"], 86400))
    b2.provision(b2.quote("qbraid", "qpu_job", {"mode": "qpu", "circuit": BELL}, 3600)["quote"])
    with pytest.raises(pb.Refused) as e:                   # its one-real-job-per-client-per-day rule
        b2.provision(b2.quote("qbraid", "qpu_job", {"mode": "qpu", "circuit": BELL}, 3600)["quote"])
    assert e.value.code == "provision_failed" and "per visitor per day" in str(e.value)
    with pytest.raises(pb.Refused) as e:                   # its circuit validation
        b2.provision(b2.quote("qbraid", "qpu_job", {"mode": "sim", "circuit": "zz"}, 3600)["quote"])
    assert "malformed circuit" in str(e.value)


def test_a_qpu_job_still_needs_the_provider_brokers_envelope(tmp_path, qbroker):
    q = qbroker(real=True)
    b, _ = broker(tmp_path, q.sock, classes=("storage",))   # qpu not allowed by the person
    quote = b.quote("qbraid", "qpu_job", {"mode": "qpu", "circuit": BELL}, 3600)
    with pytest.raises(pb.Refused) as e:
        b.provision(quote["quote"])
    assert e.value.code == "approval_required"
    assert q.backend.submitted == [], "nothing reached qBraid without the person's approval"


def test_bad_spec_and_unreachable_broker(tmp_path):
    b, _ = broker(tmp_path, str(tmp_path / "nothing.sock"))
    for spec in ({"mode": "x", "circuit": BELL}, {"mode": "sim"}, {"mode": "sim", "circuit": BELL, "shots": -1}):
        with pytest.raises(pb.Refused) as e:
            b.quote("qbraid", "qpu_job", spec, 60)
        assert e.value.code == "bad_spec"
    with pytest.raises(pb.Refused) as e:
        b.quote("qbraid", "qpu_job", {"mode": "qpu", "circuit": BELL}, 60)
    assert e.value.code == "qpu_broker_down"
