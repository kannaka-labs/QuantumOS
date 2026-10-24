"""Tests for the provider broker core (#284, ADR-0026), against the mock provider."""
from __future__ import annotations

import asyncio
import json
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import provider_broker as pb  # noqa: E402
from plugin_mock import MockProvider  # noqa: E402

DAY = 86400.0


class Clock:
    def __init__(self, t=1_800_000_000.0):
        self.t = t

    def __call__(self):
        return self.t


def make(tmp_path, allow_spend=True, env=None):
    clock = Clock()
    mock = MockProvider()
    b = pb.Broker(pb.Ledger(str(tmp_path / "broker.db")), {"mock": mock}, allow_spend=allow_spend, clock=clock)
    key = None
    if env is not False:
        key = b.set_envelope(env or pb.Envelope(per_day_usd=1.0, per_action_usd=0.5,
                                                classes=["storage", "compute"], max_lease_s=int(DAY)))
    return b, mock, clock, key


def refused(code, fn, *a, **kw):
    with pytest.raises(pb.Refused) as e:
        fn(*a, **kw)
    assert e.value.code == code, (e.value.code, str(e.value))
    return e.value


def test_quote_provision_teardown_and_ledger(tmp_path):
    b, mock, clock, key = make(tmp_path)
    assert len(key) >= 22
    q = b.quote("mock", "vm", {}, 3 * 3600)
    assert q["max_cost_usd"] == pytest.approx(0.03) and q["exact"] and q["within_envelope"]
    assert q["resource_class"] == "compute" and q["unit"] == "hour"
    lease = b.provision(q["quote"])
    (rid,) = mock.resources
    assert lease["resource"] == rid and mock.resources[rid]["tag"] == f"qos-{b.broker_id}-{lease['lease']}"
    assert b.status(lease["lease"])["provider_status"] == {"state": "running"}
    refused("quote_used", b.provision, q["quote"])
    out = b.teardown(lease["lease"])
    assert out["state"] == "ended" and out["est_cost_usd"] == pytest.approx(0.03) and out["billed_usd"] is None
    assert mock.resources == {} and mock.torn_down == [rid]
    assert b.teardown(lease["lease"])["state"] == "ended", "teardown is idempotent"
    kinds = [e["event"] for e in b.ledger()["events"]]
    assert kinds == ["ended", "provisioned", "quote", "envelope"]
    assert stat.S_IMODE((tmp_path / "broker.db").stat().st_mode) == 0o600


def test_spending_off_allows_only_free(tmp_path):
    b, mock, _, _ = make(tmp_path, allow_spend=False)
    refused("spending_off", b.provision, b.quote("mock", "vm", {}, 3600)["quote"])
    assert mock.resources == {}
    lease = b.provision(b.quote("mock", "bucket", {}, 3600)["quote"])
    assert lease["est_cost_usd"] == 0 and len(mock.resources) == 1


def test_outside_the_envelope_needs_the_persons_key(tmp_path):
    b, mock, _, key = make(tmp_path)
    # gpu is not an allowed class
    q = b.quote("mock", "gpu", {}, 3600)
    assert q["within_envelope"] is False
    e = refused("approval_required", b.provision, q["quote"])
    aid = e.extra["approval"]
    assert "approve " + aid in e.extra["how"]
    assert mock.resources == {}
    refused("not_approved", b.provision, q["quote"], aid)          # pending
    refused("bad_key", b.approve, aid, "guess")                    # the agent cannot approve
    refused("bad_key", b.approve, aid, None)
    assert b.approve(aid, key)["state"] == "approved"
    lease = b.provision(q["quote"], aid)
    assert lease["kind"] == "gpu"
    refused("no_such_approval", b.approve, aid, key)               # used
    # an approval is bound to its quote
    q2 = b.quote("mock", "gpu", {}, 3600)
    refused("not_approved", b.provision, q2["quote"], aid)


def test_daily_and_per_action_caps(tmp_path):
    b, _, clock, key = make(tmp_path, env=pb.Envelope(per_day_usd=0.05, per_action_usd=0.03,
                                                      classes=["compute"], max_lease_s=int(DAY)))
    b.provision(b.quote("mock", "vm", {}, 3 * 3600)["quote"])               # 0.03, fits
    e = refused("approval_required", b.provision, b.quote("mock", "vm", {}, 3 * 3600)["quote"])
    assert "left today" in str(e)                                           # 0.03 + 0.03 > 0.05
    e = refused("approval_required", b.provision, b.quote("mock", "vm", {}, 4 * 3600)["quote"])
    assert "per-action" in str(e)
    e = refused("approval_required", b.provision, b.quote("mock", "vm", {}, int(2 * DAY))["quote"])
    assert "longer than" in str(e)
    clock.t += DAY                                                          # a new UTC day
    b.provision(b.quote("mock", "vm", {}, 3 * 3600)["quote"])
    assert b.budget()["committed_today_usd"] == pytest.approx(0.03)


def test_no_envelope_means_ask(tmp_path):
    b, _, _, _ = make(tmp_path, env=False)
    e = refused("approval_required", b.provision, b.quote("mock", "bucket", {}, 3600)["quote"])
    assert "no envelope" in str(e)
    refused("no_approver", b.approve, e.extra["approval"], "x")


def test_changing_the_envelope_needs_the_current_key(tmp_path):
    b, _, _, key = make(tmp_path)
    bigger = pb.Envelope(per_day_usd=1000, per_action_usd=1000, classes=["gpu"], max_lease_s=int(DAY))
    refused("bad_key", b.set_envelope, bigger)                    # what an agent would try
    refused("bad_key", b.set_envelope, bigger, "not-the-key")
    assert b.set_envelope(bigger, key) is None, "the key is shown only the first time"
    assert b.envelope().per_day_usd == 1000


def test_quotes_expire(tmp_path):
    b, mock, clock, _ = make(tmp_path)
    q = b.quote("mock", "bucket", {}, 3600)
    clock.t += pb.QUOTE_TTL_S + 1
    refused("quote_expired", b.provision, q["quote"])
    assert mock.resources == {}


def test_leases_expire_and_are_reaped(tmp_path):
    b, mock, clock, _ = make(tmp_path)
    short = b.provision(b.quote("mock", "vm", {}, 3600)["quote"])
    long = b.provision(b.quote("mock", "bucket", {}, 7200)["quote"])
    clock.t += 3600
    assert b.reap() == 1
    assert b.status(short["lease"])["end_reason"] == "expired"
    assert b.status(long["lease"])["state"] == "active" and len(mock.resources) == 1
    clock.t += 3600
    assert b.reap() == 1 and mock.resources == {}


def test_failed_provision_is_recorded_and_cleaned_by_tag(tmp_path):
    b, mock, _, _ = make(tmp_path)
    mock.fail_next = True
    e = refused("provision_failed", b.provision, b.quote("mock", "vm", {}, 3600)["quote"])
    assert b.status(e.extra["lease"])["end_reason"] == "failed"
    mock.crash_after_create = True        # created at the provider, then the call failed
    refused("provision_failed", b.provision, b.quote("mock", "vm", {}, 3600)["quote"])
    assert mock.resources == {}, "a resource created before the failure is found by its tag and removed"
    assert b.budget()["committed_today_usd"] == 0, "failed leases do not use the budget"


def test_crash_mid_provision_is_recovered_at_start(tmp_path):
    b, mock, clock, _ = make(tmp_path)
    q = b.quote("mock", "vm", {}, 3600)
    real = mock.provision
    mock.provision = lambda kind, spec, tag: (real(kind, spec, tag), (_ for _ in ()).throw(SystemExit))[1]
    with pytest.raises(SystemExit):        # the process dies inside the provider call
        b.provision(q["quote"])
    mock.provision = real
    assert len(mock.resources) == 1
    b2 = pb.Broker(pb.Ledger(str(tmp_path / "broker.db")), {"mock": mock}, clock=clock)
    assert b2.broker_id == b.broker_id, "the broker id persists, so its tags stay recognisable"
    assert b2.recover() == 1 and mock.resources == {}


def test_never_touches_resources_it_did_not_create(tmp_path):
    b, mock, clock, _ = make(tmp_path)
    mock.resources["someone-elses-vm"] = {"kind": "vm", "tag": "qos-otherbroker-l0123456789", "state": "running"}
    mock.resources["untagged"] = {"kind": "vm", "tag": "", "state": "running"}
    lease = b.provision(b.quote("mock", "vm", {}, 3600)["quote"])
    clock.t += 7200
    b.reap()
    b.teardown(lease["lease"])
    b.recover()
    assert set(mock.resources) == {"someone-elses-vm", "untagged"}


def test_socket_serves_agent_ops_only(tmp_path):
    b, mock, _, key = make(tmp_path)

    async def run():
        sock = tmp_path / "b.sock"
        server = await asyncio.start_unix_server(b.serve_conn, str(sock))
        async with server:
            r, w = await asyncio.open_unix_connection(str(sock))
            try:
                await talk(r, w)
            finally:      # an open client would keep the server from closing
                w.close()
                await w.wait_closed()

    async def talk(r, w):
        async def ask(obj):
            w.write((json.dumps(obj) if isinstance(obj, dict) else obj).encode() + b"\n")
            await w.drain()
            return json.loads(await r.readline())

        assert (await ask({"op": "providers"}))["providers"]["mock"]["vm"]["unit"] == "hour"
        q = await ask({"op": "quote", "provider": "mock", "kind": "vm", "spec": {}, "duration_s": 3600})
        assert q["ok"] and q["within_envelope"]
        lease = await ask({"op": "provision", "quote": q["quote"]})
        assert lease["ok"] and lease["lease"].startswith("l")
        for op in ("approve", "set_envelope", "envelope"):         # human ops are not on the socket
            ans = await ask({"op": op, "approval": "a0123456789", "key": key})
            assert ans == {"ok": False, "error": "bad_op", "message": ans["message"]}
        g = await ask({"op": "quote", "provider": "mock", "kind": "gpu", "spec": {}, "duration_s": 3600})
        ans = await ask({"op": "provision", "quote": g["quote"]})
        assert ans["error"] == "approval_required" and ans["approval"].startswith("a")
        assert (await ask("not json"))["error"] == "bad_request"
        assert (await ask({"op": "quote", "provider": "mock", "kind": "nope", "spec": {},
                           "duration_s": 60}))["error"] == "bad_kind"
        assert (await ask({"op": "teardown", "lease": lease["lease"]}))["state"] == "ended"
        budget = await ask({"op": "budget"})
        assert budget["active_leases"] == 0 and budget["spending"] == "on"
        led = await ask({"op": "ledger", "limit": 3})
        assert len(led["events"]) == 3

    asyncio.run(run())


def test_cli_envelope_shows_the_key_once_then_needs_it(tmp_path, capsys, monkeypatch):
    db = str(tmp_path / "c.db")
    assert pb.main(["envelope", "--db", db, "--per-day", "2", "--per-action", "1",
                    "--classes", "storage", "--max-lease", "12h"]) == 0
    out = capsys.readouterr().out
    key = out.splitlines()[1]
    assert "shown once" in out and len(key) >= 22
    monkeypatch.setattr(pb.getpass, "getpass", lambda prompt="": "wrong")
    assert pb.main(["envelope", "--db", db, "--per-day", "99", "--per-action", "99", "--classes", "gpu"]) == 1
    monkeypatch.setattr(pb.getpass, "getpass", lambda prompt="": key)
    assert pb.main(["envelope", "--db", db, "--per-day", "3", "--per-action", "1", "--classes", "storage"]) == 0
    assert key not in capsys.readouterr().out, "the key is never shown again"
    assert pb.parse_duration("12h") == 43200 and pb.parse_duration("90") == 90
