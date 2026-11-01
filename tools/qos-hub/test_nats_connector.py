"""Tests for the hub's NATS command channel (#297 slice 5), with the NATS client mocked: no server, no nats-py."""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qos_hub as hub  # noqa: E402
import provider_broker as pb  # noqa: E402
from nats_connector import CommandChannel, ConfigError, RateLimiter, subject_for  # noqa: E402


class FakeSession:
    def __init__(self, tools):
        self.tools, self.calls = tools, []

    async def list_tools(self):
        return NS(tools=[NS(name=n, description=f"{n} tool", inputSchema={"type": "object"}) for n in self.tools])

    def validate(self, name, arguments):
        if "bad" in arguments:
            raise ValueError("bad argument")

    async def call_tool(self, name, arguments):
        self.calls.append((name, arguments))
        return NS(content=[NS(type="text", text=json.dumps({"did": name, **arguments}))], isError=False)


class FakeNats:
    """What nats.connect returns: records the subscription and the drain."""

    def __init__(self):
        self.subs, self.drained, self.url, self.kw = {}, False, None, None

    async def subscribe(self, subject, cb=None):
        self.subs[subject] = cb
        return NS(subject=subject)

    async def drain(self):
        self.drained = True


class FakeMsg:
    def __init__(self, payload, reply="_INBOX.1"):
        self.data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.reply, self.sent = reply, []

    async def respond(self, data):
        self.sent.append(json.loads(data))


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def make(tmp_path, nats=None, rules=None):
    tmp_path.mkdir(parents=True, exist_ok=True)
    led = hub.HubLedger(str(tmp_path / "pb.db"))
    led.broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    k = FakeSession(["kannaka_recall", "kannaka_remember", "swarm_send", "compute_grant"])
    h = hub.Hub({"kannaka": k}, hub.Policy(rules if rules is not None else
                {"kannaka.kannaka_*": "allow", "kannaka.swarm_send": "ask", "kannaka.compute_grant": "deny"}), led)
    asyncio.run(h.refresh())
    cfg = {"name": "nick", "allow": ["kannaka.kannaka_recall", "kannaka.swarm_send", "kannaka.compute_grant"],
           **(nats or {})}
    clock = Clock()
    return CommandChannel(h, cfg, clock=clock), h, k, led, clock


def ask(ch, **req):
    base = {"v": 1, "id": "r1", "identity": "peer-a", "cmd": "kannaka.kannaka_recall", "args": {"q": "tide"}}
    return asyncio.run(ch.handle(json.dumps({**base, **req}).encode()))


def events(led, what="hub_nats_cmd"):
    return [e for e in led.broker.ledger(limit=100)["events"] if e["event"] == what]


def test_every_outcome_is_in_the_ledger_with_the_claimed_identity(tmp_path):
    ch, _h, _k, led, _c = make(tmp_path, nats={"rate": {"per_identity_per_min": 3, "global_per_min": 10}})
    ask(ch)
    ask(ch, cmd="kannaka.kannaka_remember")
    ask(ch, dry_run=True)
    ask(ch)
    got = [(e["identity"], e["outcome"]) for e in reversed(events(led))]
    assert got == [("peer-a", "forwarded"), ("peer-a", "not_allowed"), ("peer-a", "dry_run"), ("peer-a", "rate_limited")]


def test_allowed_command_runs_and_replies_with_the_envelope(tmp_path):
    ch, _h, k, _led, _c = make(tmp_path)
    r = ask(ch)
    assert r == {"v": 1, "id": "r1", "ok": True, "output": {"did": "kannaka_recall", "q": "tide"},
                 "error": None, "dry_run": False}
    assert k.calls == [("kannaka_recall", {"q": "tide"})]


def test_subject_and_name_rules():
    assert subject_for("nick") == "KAX.qos.nick.cmd"
    for bad in ("", "a.b", "a b", "A", "x" * 41, "*", ">", None):
        with pytest.raises(ConfigError):
            subject_for(bad)


def test_empty_allow_list_allows_nothing_even_if_the_hub_policy_would(tmp_path):
    ch, _h, k, _l, _c = make(tmp_path, nats={"allow": []})
    r = ask(ch)
    assert not r["ok"] and r["error"]["code"] == "not_allowed" and k.calls == []


def test_allow_list_is_narrower_than_hub_policy(tmp_path):
    ch, _h, k, _l, _c = make(tmp_path)
    r = ask(ch, cmd="kannaka.kannaka_remember")          # hub says allow; the channel's list does not
    assert r["error"]["code"] == "not_allowed" and k.calls == []


def test_hub_policy_still_applies_deny_and_ask(tmp_path):
    ch, h, k, _l, _c = make(tmp_path)
    r = ask(ch, cmd="kannaka.compute_grant")
    assert not r["ok"] and r["error"]["code"] == "denied" and k.calls == []
    r = ask(ch, cmd="kannaka.swarm_send", args={"text": "hi"})
    assert not r["ok"] and r["error"]["code"] == "approval_required" and k.calls == []
    aid = r["output"]["approval"]
    assert [p["approval"] for p in h.ledger.pending()] == [aid]


def test_hub_call_approved_is_never_reachable_and_cannot_be_allow_listed(tmp_path):
    ch, *_ = make(tmp_path, nats={"allow": ["kannaka.*"]})
    r = ask(ch, cmd="hub_call_approved", args={"approval": "h1"})
    assert r["error"]["code"] == "not_allowed"
    with pytest.raises(ConfigError):
        make(tmp_path / "second", nats={"allow": ["hub_*"]})


def test_a_bare_wildcard_allow_list_is_refused_at_config_time(tmp_path):
    with pytest.raises(ConfigError):
        make(tmp_path, nats={"allow": ["*"]})


def test_unknown_tool_and_downstream_error_shape(tmp_path):
    ch, *_ = make(tmp_path, nats={"allow": ["kannaka.*"]})
    r = ask(ch, cmd="kannaka.nope")
    assert r["error"]["code"] == "no_such_tool"


@pytest.mark.parametrize("raw,code,rid", ids=["notjson", "array", "toolarge", "noid", "v2", "badident", "badargs", "baddry"], argvalues=[
    (b"not json", "bad_request", None),
    (b"[1]", "bad_request", None),
    (b"x" * (64 * 1024 + 1), "too_large", None),
    (json.dumps({"v": 1, "identity": "a", "cmd": "c"}).encode(), "bad_request", None),
    (json.dumps({"v": 2, "id": "r9", "identity": "a", "cmd": "c"}).encode(), "unsupported_version", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a b", "cmd": "c"}).encode(), "bad_request", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a", "cmd": "c", "args": []}).encode(), "bad_request", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a", "cmd": "c", "dry_run": "yes"}).encode(), "bad_request", "r9"),
])
def test_malformed_requests_are_refused_with_an_envelope(tmp_path, raw, code, rid):
    ch, _h, k, _l, _c = make(tmp_path)
    r = asyncio.run(ch.handle(raw))
    assert r["v"] == 1 and r["ok"] is False and r["id"] == rid and r["error"]["code"] == code and k.calls == []


def test_per_identity_rate_limit_and_window_recovery(tmp_path):
    ch, _h, k, _l, clock = make(tmp_path, nats={"rate": {"per_identity_per_min": 2, "global_per_min": 100}})
    assert ask(ch)["ok"] and ask(ch)["ok"]
    r = ask(ch)
    assert not r["ok"] and r["error"]["code"] == "rate_limited" and len(k.calls) == 2
    assert ask(ch, identity="peer-b")["ok"], "another identity has its own budget"
    clock.t += 61
    assert ask(ch)["ok"], "the window slides"


def test_global_limit_holds_against_invented_identities(tmp_path):
    ch, _h, k, _l, _c = make(tmp_path, nats={"rate": {"per_identity_per_min": 5, "global_per_min": 3}})
    results = [ask(ch, identity=f"spoof-{i}") for i in range(5)]
    assert [r["ok"] for r in results] == [True, True, True, False, False] and len(k.calls) == 3
    assert results[3]["error"]["code"] == "rate_limited"


def test_rate_limiter_forgets_old_identities_and_refusals_do_not_count():
    clock = Clock()
    rl = RateLimiter(1, 1000, clock)
    assert rl.check("a") is None and rl.check("a") == "identity" and rl.check("a") == "identity"
    clock.t += 61
    assert rl.check("a") is None
    for i in range(1100):
        rl.check(f"id{i}")
    assert len(rl.by_id) <= 1024


def test_dry_run_decides_but_runs_and_stores_nothing(tmp_path):
    ch, h, k, _l, _c = make(tmp_path)
    r = ask(ch, dry_run=True)
    assert r["ok"] and r["dry_run"] and r["output"] == {"would": "run", "policy": "allow"}
    r = ask(ch, dry_run=True, cmd="kannaka.swarm_send", args={"text": "hi"})
    assert r["output"] == {"would": "ask", "policy": "ask"}
    r = ask(ch, dry_run=True, cmd="kannaka.compute_grant")
    assert r["output"] == {"would": "deny", "policy": "deny"}
    assert k.calls == [] and h.ledger.pending() == []


def test_dry_run_reports_bad_arguments_and_still_honours_the_allow_list(tmp_path):
    ch, *_ = make(tmp_path)
    r = ask(ch, dry_run=True, args={"bad": 1})
    assert not r["ok"] and r["dry_run"] and r["error"]["code"] == "bad_arguments"
    r = ask(ch, dry_run=True, cmd="kannaka.kannaka_remember")
    assert r["error"]["code"] == "not_allowed"


def test_dry_run_still_counts_against_the_rate_limit(tmp_path):
    ch, *_ = make(tmp_path, nats={"rate": {"per_identity_per_min": 1, "global_per_min": 10}})
    assert ask(ch, dry_run=True)["ok"]
    assert ask(ch, dry_run=True)["error"]["code"] == "rate_limited"


def test_channel_wide_dry_run_cannot_be_turned_off_by_a_request(tmp_path):
    ch, _h, k, _l, _c = make(tmp_path, nats={"dry_run": True})
    r = ask(ch, dry_run=False)
    assert r["dry_run"] and r["output"]["would"] == "run" and k.calls == []


def test_subscriber_lifecycle_replies_on_the_inbox_and_drops_reply_less_commands(tmp_path):
    ch, _h, k, led, _c = make(tmp_path)
    nc = FakeNats()

    async def connect(url, **kw):
        nc.url, nc.kw = url, kw
        return nc
    ch.connect = connect
    ch.cfg = {**ch.cfg, "url": "nats://x:4222", "creds_file": "/tmp/c.creds"}

    async def run():
        await ch.start()
        cb = nc.subs["KAX.qos.nick.cmd"]
        good = FakeMsg({"v": 1, "id": "m1", "identity": "p", "cmd": "kannaka.kannaka_recall", "args": {}})
        await cb(good)
        silent = FakeMsg({"v": 1, "id": "m2", "identity": "p", "cmd": "kannaka.kannaka_recall", "args": {}}, reply="")
        await cb(silent)
        await ch.stop()
        return good, silent
    good, silent = asyncio.run(run())
    assert nc.url == "nats://x:4222" and nc.kw == {"user_credentials": "/tmp/c.creds"}
    assert good.sent and good.sent[0]["ok"] and good.sent[0]["id"] == "m1"
    assert silent.sent == [] and len(k.calls) == 1, "a request with no reply inbox is not run"
    assert nc.drained


def test_a_crash_inside_a_command_becomes_an_error_reply(tmp_path):
    ch, h, _k, _l, _c = make(tmp_path)

    async def boom(*a, **kw):
        raise RuntimeError("secret-token-xyz")
    h.call = boom
    r = ask(ch)
    assert not r["ok"] and r["error"]["code"] == "internal_error" and "secret" not in json.dumps(r)


def test_missing_nats_py_is_a_clear_config_error(tmp_path, monkeypatch):
    ch, *_ = make(tmp_path)
    monkeypatch.setitem(sys.modules, "nats", None)
    with pytest.raises(ConfigError, match="nats-py"):
        asyncio.run(ch.start())
