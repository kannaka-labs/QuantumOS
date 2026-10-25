"""Tests for the qBraid Lab plugin (#297 slice 4): options b (sole user) and c (alert, never
guess), against a stand-in for qbraid_core's ComputeClient."""
from __future__ import annotations

import itertools
import sys
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provider_broker as pb  # noqa: E402
from plugin_qbraid_lab import QbraidLabProvider  # noqa: E402

ids = itertools.count(1)
PROFILES = {   # the shape qbraid_core's list_profiles really returns (no pricing object)
    "2vCPU_4GB": NS(slug="2vCPU_4GB", gpu=False, plan="Free", has_capacity=True,
                    rate_dollar=0.002, rate_time_frame="min"),
    "gpu-l4": NS(slug="gpu-l4", gpu=True, plan="Standard", has_capacity=True,
                 rate_dollar=0.49, rate_time_frame="hour"),
    "unpriced": NS(slug="unpriced", gpu=False, plan="Free", has_capacity=True,
                   rate_dollar=None, rate_time_frame=None),
}


class FakeCompute:
    def __init__(self):
        self.server = {"running": False, "starting": False, "profile": None}
        self.instances: dict[str, NS] = {}
        self.crash_after_create = False
        self.calls: list[str] = []

    def list_profiles(self, **kw):
        return list(PROFILES.values())

    def get_profile(self, slug):          # like the real one: rejects uppercase slugs it lists
        raise RuntimeError("Profile slug can only contain lowercase letters, numbers, hyphens, and underscores.")

    def get_server_status(self, cluster_id=None):
        return NS(server_running=self.server["running"], server_starting=self.server["starting"],
                  current_profile=self.server["profile"])

    def start_server(self, profile, cluster_id=None):
        self.calls.append("start_server")
        self.server.update(running=True, profile=profile)
        if self.crash_after_create:
            raise SystemExit("crash after start")

    def stop_server(self, cluster_id=None):
        self.calls.append("stop_server")
        self.server.update(running=False, profile=None)

    def list_bma_instances(self):
        return list(self.instances.values())

    def provision_bma_instance(self, profile):
        self.calls.append("provision")
        i = NS(instance_id=f"inst-{next(ids)}", status=NS(value="RUNNING"), profile_slug=profile,
               ready=True, running_credits_per_min=0.2)
        self.instances[i.instance_id] = i
        if self.crash_after_create:
            raise SystemExit("crash after create")
        return i

    def get_bma_instance(self, iid):
        if iid not in self.instances:
            raise RuntimeError("404 not found")
        return self.instances[iid]

    def terminate_bma_instance(self, iid):
        self.calls.append(f"terminate {iid}")
        self.instances[iid].status = NS(value="TERMINATED")


def make(tmp_path, sole_user=False, client=None):
    c = client or FakeCompute()
    lab = QbraidLabProvider(client=c, journal=str(tmp_path / "journal.json"), sole_user=sole_user)
    b = pb.Broker(pb.Ledger(str(tmp_path / "b.db")), {"qbraid_lab": lab}, allow_spend=True)
    b.set_envelope(pb.Envelope(per_day_usd=1.0, per_action_usd=0.5, classes=["compute"], max_lease_s=86400))
    return b, lab, c


def test_quotes_come_from_live_pricing(tmp_path):
    b, _, _ = make(tmp_path)
    q = b.quote("qbraid_lab", "lab_server", {"profile": "2vCPU_4GB"}, 300)
    assert q["max_cost_usd"] == pytest.approx(0.2 * 0.01 * 5) and q["exact"] is False
    assert "free CPU-Lab hours" in q["free_tier"] and q["resource_class"] == "compute"
    g = b.quote("qbraid_lab", "instance", {"profile": "gpu-l4"}, 3600)
    assert g["resource_class"] == "gpu" and g["free_tier"] is None and "terminates" in g["notes"]
    assert g["max_cost_usd"] == pytest.approx(0.49, abs=1e-4), "$0.49/hour for an hour"
    for spec, code in (({"profile": "nope"}, "bad_spec"), ({}, "bad_spec"), ({"profile": "unpriced"}, "no_price")):
        with pytest.raises(pb.Refused) as e:
            b.quote("qbraid_lab", "instance", spec, 60)
        assert e.value.code == code


def test_lab_server_starts_only_when_stopped_and_stops_at_teardown(tmp_path):
    b, lab, c = make(tmp_path)
    c.server["running"] = True                                   # someone is using the Lab
    with pytest.raises(pb.Refused) as e:
        b.provision(b.quote("qbraid_lab", "lab_server", {"profile": "2vCPU_4GB"}, 300)["quote"])
    assert "lab_server_busy" in str(e.value) or e.value.code in ("provision_failed", "lab_server_busy")
    assert "start_server" not in c.calls and "stop_server" not in c.calls, "never touches a server it did not start"
    c.server["running"] = False
    le = b.provision(b.quote("qbraid_lab", "lab_server", {"profile": "2vCPU_4GB"}, 300)["quote"])
    assert le["resource"] == "lab_server:default" and b.status(le["lease"])["provider_status"]["state"] == "running"
    b.teardown(le["lease"])
    assert c.calls[-1] == "stop_server" and not c.server["running"]


def test_instance_teardown_terminates_not_stops(tmp_path):
    b, lab, c = make(tmp_path)
    le = b.provision(b.quote("qbraid_lab", "instance", {"profile": "2vCPU_4GB"}, 600)["quote"])
    rid = le["resource"]
    assert lab._load()[f"qos-{b.broker_id}-{le['lease']}"]["rid"] == rid
    b.teardown(le["lease"])
    assert c.calls[-1] == f"terminate {rid}" and lab._load() == {}
    lab.teardown(rid)                                             # idempotent: already terminated
    assert c.calls.count(f"terminate {rid}") == 1


def test_option_b_sole_user_refuses_foreign_instances_and_recovers_its_own(tmp_path):
    c = FakeCompute()
    c.instances["someone"] = NS(instance_id="someone", status=NS(value="RUNNING"), profile_slug="2vCPU_4GB")
    b, lab, _ = make(tmp_path, sole_user=True, client=c)
    with pytest.raises(pb.Refused) as e:
        b.provision(b.quote("qbraid_lab", "instance", {"profile": "2vCPU_4GB"}, 600)["quote"])
    assert "not_sole_user" in str(e.value) or "did not start" in str(e.value)
    assert "provision" not in c.calls
    del c.instances["someone"]
    c.crash_after_create = True
    with pytest.raises(SystemExit):                              # the process dies mid-provision
        b.provision(b.quote("qbraid_lab", "instance", {"profile": "2vCPU_4GB"}, 600)["quote"])
    c.crash_after_create = False
    (orphan,) = [i for i in c.instances.values()]
    lab2 = QbraidLabProvider(client=c, journal=str(tmp_path / "journal.json"), sole_user=True)
    b2 = pb.Broker(pb.Ledger(str(tmp_path / "b.db")), {"qbraid_lab": lab2}, allow_spend=True)
    assert b2.recover() == 1
    assert c.calls[-1] == f"terminate {orphan.instance_id}", "sole user: the new instance can only be ours"


def test_option_c_never_guesses_and_alerts_loudly(tmp_path, caplog):
    c = FakeCompute()
    b, lab, _ = make(tmp_path, sole_user=False, client=c)
    c.instances["preexisting"] = NS(instance_id="preexisting", status=NS(value="RUNNING"), profile_slug="2vCPU_4GB")
    c.crash_after_create = True
    with pytest.raises(SystemExit):
        b.provision(b.quote("qbraid_lab", "instance", {"profile": "2vCPU_4GB"}, 600)["quote"])
    c.crash_after_create = False
    new = sorted(set(c.instances) - {"preexisting"})
    c.instances["created-meanwhile"] = NS(instance_id="created-meanwhile", status=NS(value="RUNNING"),
                                          profile_slug="2vCPU_4GB")     # another tool on the same account
    lab2 = QbraidLabProvider(client=c, journal=str(tmp_path / "journal.json"), sole_user=False)
    b2 = pb.Broker(pb.Ledger(str(tmp_path / "b.db")), {"qbraid_lab": lab2}, allow_spend=True)
    caplog.set_level("ERROR", logger="provider-broker")
    assert b2.recover() == 1
    assert not any(x.startswith("terminate") for x in c.calls), "option c: nothing is terminated on a guess"
    ev = [e for e in b2.ledger()["events"] if e["event"] == "orphan_possible"]
    assert ev and set(ev[0]["candidates"]) == set(new) | {"created-meanwhile"}
    assert "possible orphan" in caplog.text


def test_a_harmless_crash_leaves_no_journal_entry(tmp_path):
    c = FakeCompute()
    b, lab, _ = make(tmp_path, client=c)

    def fail(profile):
        raise RuntimeError("qBraid refused")       # failed BEFORE anything was created
    c.provision_bma_instance = fail
    with pytest.raises(pb.Refused):
        b.provision(b.quote("qbraid_lab", "instance", {"profile": "2vCPU_4GB"}, 600)["quote"])
    assert lab._load() == {}, "nothing was created, so nothing is left to chase"
