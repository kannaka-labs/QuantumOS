"""Tests for the QuantumOS hub (#297): policy, approvals and the ledger, and one end-to-end
run over real MCP stdio with a fake downstream server."""
from __future__ import annotations

import asyncio
import json
import os
import sys
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qos_hub as hub  # noqa: E402
import provider_broker as pb  # noqa: E402


class FakeSession:
    def __init__(self, tools):
        self.tools, self.calls = tools, []

    async def list_tools(self):
        return NS(tools=[NS(name=n, description=f"{n} tool", inputSchema={"type": "object"}) for n in self.tools])

    async def call_tool(self, name, arguments):
        self.calls.append((name, arguments))
        return NS(content=[NS(type="text", text=json.dumps({"did": name, **arguments}))], isError=False)


class Broken:
    async def list_tools(self):
        raise RuntimeError("server crashed")


def make(tmp_path, rules=None, default="ask"):
    db = str(tmp_path / "pb.db")
    led = hub.HubLedger(db)
    key = led.broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    k = FakeSession(["kannaka_recall", "kannaka_remember", "swarm_send", "compute_grant"])
    h = hub.Hub({"kannaka": k, "dead": Broken()},
                hub.Policy(rules if rules is not None else
                           {"kannaka.kannaka_*": "allow", "kannaka.swarm_send": "ask",
                            "kannaka.compute_grant": "deny"}, default), led)
    asyncio.run(h.refresh())
    return h, k, key, led


def call(h, tool, **args):
    return asyncio.run(h.call(tool, args))


def test_policy_most_specific_match_wins_and_unknown_defaults_to_ask():
    p = hub.Policy({"kannaka.*": "allow", "kannaka.swarm_*": "ask", "kannaka.swarm_status": "allow"})
    assert p.of("kannaka.recall") == "allow"
    assert p.of("kannaka.swarm_send") == "ask"
    assert p.of("kannaka.swarm_status") == "allow"
    assert p.of("other.thing") == "ask"
    with pytest.raises(ValueError):
        hub.Policy({"x": "yes"})


def test_allow_forwards_and_is_ledgered(tmp_path):
    h, k, _, led = make(tmp_path)
    out = call(h, "kannaka.kannaka_recall", query="cats")
    assert out == {"ok": True, "result": {"did": "kannaka_recall", "query": "cats"}}
    assert k.calls == [("kannaka_recall", {"query": "cats"})]
    ev = led.broker.ledger(limit=5)["events"][0]
    assert ev["event"] == "hub_call" and ev["tool"] == "kannaka.kannaka_recall" and ev["decision"] == "allow"


def test_deny_never_reaches_the_server(tmp_path):
    h, k, _, _ = make(tmp_path)
    out = call(h, "kannaka.compute_grant", machine="m1", credits=50)
    assert out["error"] == "denied" and k.calls == []


def test_ask_stores_the_exact_call_and_only_the_person_can_release_it(tmp_path):
    h, k, key, led = make(tmp_path)
    out = call(h, "kannaka.swarm_send", text="hello constellation")
    assert out["error"] == "approval_required" and k.calls == []
    aid = out["approval"]
    assert f"approve {aid}" in out["tell_your_human"] and "cannot approve" in out["tell_your_human"]
    assert call(h, "hub_call_approved", approval=aid)["error"] == "not_approved"      # still pending
    with pytest.raises(pb.Refused):
        led.decide(aid, "not-the-key")
    assert led.pending()[0]["arguments"] == {"text": "hello constellation"}
    led.decide(aid, key)
    done = call(h, "hub_call_approved", approval=aid)
    assert done["ok"] and k.calls == [("swarm_send", {"text": "hello constellation"})], "exactly the stored call"
    assert call(h, "hub_call_approved", approval=aid)["error"] == "not_approved", "an approval is used once"
    # a denied request never runs
    aid2 = call(h, "kannaka.swarm_send", text="second")["approval"]
    led.decide(aid2, key, deny=True)
    assert call(h, "hub_call_approved", approval=aid2)["error"] == "not_approved" and len(k.calls) == 1


def test_a_stored_call_cannot_be_altered_or_outlive_its_hour(tmp_path):
    h, k, key, led = make(tmp_path)
    aid = call(h, "kannaka.swarm_send", text="original")["approval"]
    led.decide(aid, key)
    led.l.q("UPDATE hub_approvals SET arguments=? WHERE id=?", json.dumps({"text": "swapped"}), aid)
    assert call(h, "hub_call_approved", approval=aid)["error"] == "approval_tampered" and k.calls == []
    aid2 = call(h, "kannaka.swarm_send", text="late")["approval"]
    led.decide(aid2, key)
    led.l.q("UPDATE hub_approvals SET created=created-? WHERE id=?", hub.APPROVAL_TTL_S + 1, aid2)
    assert call(h, "hub_call_approved", approval=aid2)["error"] == "approval_expired"


def test_unknown_tools_ask_and_a_broken_server_does_not_hide_the_rest(tmp_path):
    h, k, _, led = make(tmp_path, rules={}, default="ask")
    names = [t["tool"] for t in call(h, "hub_tools")["tools"]]
    assert "kannaka.kannaka_recall" in names and not any(n.startswith("dead.") for n in names)
    assert call(h, "kannaka.kannaka_recall", query="x")["error"] == "approval_required"
    assert any(e["event"] == "hub_server_error" and e["server"] == "dead" for e in led.broker.ledger(limit=20)["events"])
    assert call(h, "nope.tool")["error"] == "no_such_tool"


def test_cli_approve_shows_the_call_and_needs_the_key(tmp_path, capsys, monkeypatch):
    h, k, key, led = make(tmp_path)
    aid = call(h, "kannaka.swarm_send", text="via cli")["approval"]
    db = str(tmp_path / "pb.db")
    monkeypatch.setattr(hub.getpass, "getpass", lambda prompt="": "wrong")
    assert hub.main(["approve", "--db", db, aid]) == 1
    monkeypatch.setattr(hub.getpass, "getpass", lambda prompt="": key)
    assert hub.main(["approve", "--db", db, aid]) == 0
    out = capsys.readouterr().out
    assert '"text": "via cli"' in out and '"state": "approved"' in out


def test_end_to_end_over_mcp_stdio(tmp_path):
    pytest.importorskip("mcp")
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    calls = tmp_path / "calls.ndjson"
    calls.touch()
    db = str(tmp_path / "pb.db")
    key = hub.HubLedger(db).broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    cfg = tmp_path / "hub.json"
    cfg.write_text(json.dumps({
        "servers": {"fake": {"command": sys.executable, "args": [str(HERE / "fake_downstream.py")],
                             "env": {"FAKE_CALLS": str(calls)}}},
        "policy": {"fake.recall": "allow", "fake.swarm_send": "ask", "fake.compute_grant": "deny"}}))

    async def run():
        params = StdioServerParameters(command=sys.executable,
                                       args=[str(HERE / "qos_hub.py"), "serve", "--config", str(cfg), "--db", db],
                                       env={**os.environ})
        async with stdio_client(params) as (r, w), ClientSession(r, w) as s:
            await s.initialize()
            names = sorted(t.name for t in (await s.list_tools()).tools)
            assert names == ["fake.recall", "fake.swarm_send", "hub_call_approved", "hub_ledger", "hub_tools"], names

            async def c(name, **a):
                return json.loads((await s.call_tool(name, a)).content[0].text)

            assert (await c("fake.recall", query="tides"))["result"]["memories"] == ["memory about tides"]
            ask = await c("fake.swarm_send", text="hi all")
            assert ask["error"] == "approval_required"
            hub.HubLedger(db).decide(ask["approval"], key)            # the person, out of band
            assert (await c("hub_call_approved", approval=ask["approval"]))["result"] == {"published": "hi all"}
            assert (await c("fake.compute_grant", machine="m", credits=5))["error"] in ("denied", "no_such_tool")
            led = await c("hub_ledger", limit=10)
            assert any(e["event"] == "hub_call" and e.get("decision") == "approved" for e in led["events"])

    asyncio.run(run())
    seen = [json.loads(line)["tool"] for line in calls.read_text().splitlines()]
    assert seen == ["recall", "swarm_send"], "the denied tool never reached the server"
