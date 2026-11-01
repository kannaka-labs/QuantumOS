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
from nats_connector import CommandChannel, ConfigError, RateLimiter, RefusalLog, subject_for  # noqa: E402


class FakeSession:
    def __init__(self, tools, fail=None):
        self.tools, self.calls, self.fail = tools, [], fail

    async def list_tools(self):
        return NS(tools=[NS(name=n, description=f"{n} tool", inputSchema={"type": "object"}) for n in self.tools])

    def validate(self, name, arguments):
        if "bad" in arguments:
            raise ValueError("bad argument")

    async def call_tool(self, name, arguments):
        self.calls.append((name, arguments))
        if self.fail == "error":
            return NS(content=[NS(type="text", text="the tool said no")], isError=True)
        if self.fail == "raise":
            raise ConnectionResetError("downstream died")
        if self.fail == "hang":
            await asyncio.sleep(5)
        return NS(content=[NS(type="text", text=json.dumps({"did": name, **arguments}))], isError=False)


class FakeNats:
    """What nats.connect returns: records the subscription (and its options) and the drain."""

    def __init__(self):
        self.subs, self.sub_kw, self.drained, self.url, self.kw = {}, {}, False, None, None

    async def subscribe(self, subject, cb=None, **kw):
        self.subs[subject] = cb
        self.sub_kw[subject] = kw
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
    def __init__(self, t=1000.0):
        self.t = t

    def __call__(self):
        return self.t


def make(tmp_path, nats=None, rules=None, fail=None, wall=None):
    tmp_path.mkdir(parents=True, exist_ok=True)
    led = hub.HubLedger(str(tmp_path / "pb.db"))
    led.broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    k = FakeSession(["kannaka_recall", "kannaka_remember", "swarm_send", "compute_grant"], fail)
    h = hub.Hub({"kannaka": k}, hub.Policy(rules if rules is not None else
                {"kannaka.kannaka_*": "allow", "kannaka.swarm_send": "ask", "kannaka.compute_grant": "deny"}), led)
    asyncio.run(h.refresh())
    cfg = {"name": "nick", "allow": ["kannaka.kannaka_recall", "kannaka.swarm_send", "kannaka.compute_grant"],
           **(nats or {})}
    clock = Clock()
    return CommandChannel(h, cfg, clock=clock, wall=wall or Clock(2000.0)), h, k, led, clock


def ask(ch, **req):
    base = {"v": 1, "id": "r1", "identity": "peer-a", "cmd": "kannaka.kannaka_recall", "args": {"q": "tide"}}
    return asyncio.run(ch.handle(json.dumps({**base, **req}).encode()))


def events(led, what="hub_nats_cmd"):
    return [e for e in led.broker.ledger(limit=500)["events"] if e["event"] == what]


def connect_to(ch, nc):
    async def connect(url, **kw):
        nc.url, nc.kw = url, kw
        return nc
    ch.connect = connect


def msg(rid="m1", **over):
    return FakeMsg({"v": 1, "id": rid, "identity": "p", "cmd": "kannaka.kannaka_recall", "args": {}, **over})


def test_every_outcome_is_in_the_ledger_with_the_claimed_identity(tmp_path):
    ch, _h, _k, led, _c = make(tmp_path, nats={"rate": {"per_identity_per_min": 3, "global_per_min": 10}})
    ask(ch)
    ask(ch, cmd="kannaka.kannaka_remember")
    ask(ch, dry_run=True)
    ask(ch)
    got = [(e["identity"], e["outcome"]) for e in reversed(events(led))]
    assert got == [("peer-a", "forwarded"), ("peer-a", "ok"), ("peer-a", "not_allowed"), ("peer-a", "dry_run"),
                   ("peer-a", "rate_limited")]


def test_a_forwarded_request_records_how_it_ended(tmp_path):
    ch, h, _k, led, _c = make(tmp_path)
    ask(ch, cmd="kannaka.compute_grant")
    r = ask(ch, cmd="kannaka.swarm_send", args={"text": "hi"})
    got = [(e["outcome"], e.get("approval")) for e in reversed(events(led))]
    assert got == [("forwarded", None), ("denied", None), ("forwarded", None),
                   ("approval_required", r["output"]["approval"])]


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


def test_an_approval_asked_over_nats_shows_its_origin(tmp_path):
    ch, h, _k, led, _c = make(tmp_path)
    r = ask(ch, cmd="kannaka.swarm_send", args={"text": "hi"}, identity="spoof-7")
    aid = r["output"]["approval"]
    pending = h.ledger.pending()
    assert [(p["approval"], p["origin"]) for p in pending] == [(aid, "nats:spoof-7")]
    assert events(led, "hub_approval_requested")[0]["origin"] == "nats:spoof-7"
    # the agent's own asks still read "agent", and the person's local agent is who runs either
    asyncio.run(h.call("kannaka.swarm_send", {"text": "mine"}))
    assert sorted(p["origin"] for p in h.ledger.pending()) == ["agent", "nats:spoof-7"]


def test_an_old_database_gains_the_origin_column_in_place(tmp_path):
    import sqlite3
    db = str(tmp_path / "old.db")
    con = sqlite3.connect(db)
    con.executescript("CREATE TABLE hub_approvals (id TEXT PRIMARY KEY, owner TEXT, tool TEXT, arguments TEXT,"
                      " digest TEXT, state TEXT, created REAL, decided REAL);")
    con.execute("INSERT INTO hub_approvals VALUES('h1', ?, 't', '{}', 'd', 'pending', 1.0, NULL)", (pb.OWNER,))
    con.commit()
    con.close()
    led = hub.HubLedger(db)
    assert [(p["approval"], p["origin"]) for p in led.pending()] == [("h1", "agent")]
    hub.HubLedger(db)                                      # a second open is not a second ALTER


def test_the_hubs_own_tools_are_never_reachable_and_cannot_be_allow_listed(tmp_path):
    ch, *_ = make(tmp_path, nats={"allow": ["kannaka.*"]})
    for tool in ("hub_call_approved", "hub_ledger", "hub_tools"):
        r = ask(ch, cmd=tool, args={"approval": "h1"})
        assert r["error"]["code"] == "not_allowed", tool
    for i, pattern in enumerate((["hub_*"], ["hub_ledger"], ["*"])):
        with pytest.raises(ConfigError):
            make(tmp_path / f"p{i}", nats={"allow": pattern})


def test_bad_rate_and_limit_values_are_config_errors(tmp_path):
    for i, bad in enumerate(({"rate": {"per_identity_per_min": "twenty"}}, {"rate": {"global_per_min": 0}},
                             {"rate": [1]}, {"max_inflight": "many"}, {"max_backlog": 0}, {"max_age_s": -5})):
        with pytest.raises(ConfigError):
            make(tmp_path / f"bad{i}", nats=bad)


def test_unknown_tool_is_refused_before_the_hub(tmp_path):
    ch, _h, k, _l, _c = make(tmp_path, nats={"allow": ["kannaka.*"]})
    r = ask(ch, cmd="kannaka.nope")
    assert r["error"]["code"] == "no_such_tool" and k.calls == []


@pytest.mark.parametrize("fail,code", [("error", "failed"), ("raise", "server_error"), ("hang", "timeout")])
def test_downstream_failures_keep_the_envelope_and_the_code(tmp_path, fail, code):
    ch, h, k, led, _c = make(tmp_path, fail=fail)
    h.timeouts = {"kannaka": 0.05}
    r = ask(ch)
    assert r["v"] == 1 and r["id"] == "r1" and r["ok"] is False and r["error"]["code"] == code
    assert r["dry_run"] is False and len(k.calls) == 1
    if fail == "error":
        assert r["output"] == "the tool said no"
    assert [e["outcome"] for e in reversed(events(led))] == ["forwarded", code]


@pytest.mark.parametrize("raw,code,rid", ids=["notjson", "array", "toolarge", "noid", "v2", "badident", "badargs", "baddry", "badts"], argvalues=[
    (b"not json", "bad_request", None),
    (b"[1]", "bad_request", None),
    (b"x" * (64 * 1024 + 1), "too_large", None),
    (json.dumps({"v": 1, "identity": "a", "cmd": "c"}).encode(), "bad_request", None),
    (json.dumps({"v": 2, "id": "r9", "identity": "a", "cmd": "c"}).encode(), "unsupported_version", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a b", "cmd": "c"}).encode(), "bad_request", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a", "cmd": "c", "args": []}).encode(), "bad_request", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a", "cmd": "c", "dry_run": "yes"}).encode(), "bad_request", "r9"),
    (json.dumps({"v": 1, "id": "r9", "identity": "a", "cmd": "c", "ts": "now"}).encode(), "bad_request", "r9"),
])
def test_malformed_requests_are_refused_with_an_envelope_and_recorded(tmp_path, raw, code, rid):
    ch, _h, k, led, _c = make(tmp_path)
    r = asyncio.run(ch.handle(raw))
    assert r["v"] == 1 and r["ok"] is False and r["id"] == rid and r["error"]["code"] == code and k.calls == []
    assert [e["outcome"] for e in events(led)] == [code]


def test_a_stale_request_is_refused_not_run_late(tmp_path):
    wall = Clock(2000.0)
    ch, _h, k, _l, _c = make(tmp_path, nats={"max_age_s": 30}, wall=wall)
    assert ask(ch, ts=1990)["ok"], "29 s old is fine"
    r = ask(ch, ts=1900.5)
    assert not r["ok"] and r["error"]["code"] == "expired" and len(k.calls) == 1
    assert ask(ch)["ok"], "a request without ts is not aged"


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


def test_a_flood_of_refusals_writes_a_bounded_number_of_ledger_rows(tmp_path):
    ch, _h, k, led, clock = make(tmp_path, nats={"rate": {"per_identity_per_min": 5, "global_per_min": 1000}})
    for i in range(200):                                   # 200 spoofed identities, all asking for a tool not allowed
        ask(ch, identity=f"spoof-{i}", cmd="kannaka.kannaka_remember")
    for _ in range(50):
        asyncio.run(ch.handle(b"garbage"))
    rows = events(led)
    assert [e["outcome"] for e in rows] == ["bad_request", "not_allowed"], "one full row per kind per minute"
    assert rows[1]["identity"] == "spoof-0"
    assert events(led, "hub_nats_refusals") == [], "the window is still open"
    clock.t += 61
    ask(ch, identity="late", cmd="kannaka.kannaka_remember")   # closes not_allowed's window, opens a new one
    ch.refusals.close()                                        # stop() does this: closes bad_request's window too
    summary = {e["outcome"]: (e["count"], e["identities"]) for e in events(led, "hub_nats_refusals")}
    assert summary == {"not_allowed": (200, 64), "bad_request": (50, 1)}
    assert [e["outcome"] for e in reversed(events(led))][-1] == "not_allowed", "the late one opened a new window"
    assert k.calls == []


def test_refusal_log_writes_no_summary_for_a_single_refusal():
    out, clock = [], Clock()
    rl = RefusalLog(lambda what, **d: out.append((what, d)), clock)
    rl.refuse("busy")
    clock.t += 61
    rl.refuse("busy", "p")
    rl.close()
    assert [w for w, _ in out] == ["hub_nats_cmd", "hub_nats_cmd"]


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
    connect_to(ch, nc)
    ch.cfg = {**ch.cfg, "url": "nats://x:4222", "creds_file": "/tmp/c.creds"}

    async def run():
        await ch.start()
        cb = nc.subs["KAX.qos.nick.cmd"]
        good = msg("m1")
        await cb(good)
        silent = FakeMsg({"v": 1, "id": "m2", "identity": "p", "cmd": "kannaka.kannaka_recall", "args": {}}, reply="")
        await cb(silent)
        await nc.kw["disconnected_cb"]()
        await nc.kw["reconnected_cb"]()
        await ch.stop()
        return good, silent
    good, silent = asyncio.run(run())
    assert nc.url == "nats://x:4222"
    assert nc.kw["user_credentials"] == "/tmp/c.creds" and nc.kw["max_reconnect_attempts"] == -1
    assert nc.sub_kw["KAX.qos.nick.cmd"] == {"pending_msgs_limit": 64, "pending_bytes_limit": 4 << 20}
    assert good.sent and good.sent[0]["ok"] and good.sent[0]["id"] == "m1"
    assert silent.sent == [] and len(k.calls) == 1, "a request with no reply inbox is not run"
    assert [e["outcome"] for e in events(led) if e["outcome"] == "no_reply_inbox"] == ["no_reply_inbox"]
    kinds = [e["event"] for e in reversed(led.broker.ledger(limit=100)["events"])]
    assert [k_ for k_ in kinds if k_.startswith("hub_nats_") and k_ != "hub_nats_cmd"] == \
        ["hub_nats_listening", "hub_nats_down", "hub_nats_up"]
    assert nc.drained


def test_requests_run_concurrently_up_to_max_inflight_and_the_backlog_is_capped(tmp_path):
    ch, h, _k, led, _c = make(tmp_path, nats={"max_inflight": 2, "max_backlog": 1})
    nc = FakeNats()
    connect_to(ch, nc)
    running, peak, release = [0], [0], None

    async def slow(tool, args, origin="agent"):
        running[0] += 1
        peak[0] = max(peak[0], running[0])
        await release.wait()
        running[0] -= 1
        return {"ok": True, "result": origin}
    h.call = slow

    async def run():
        nonlocal release
        release = asyncio.Event()
        await ch.start()
        cb = nc.subs["KAX.qos.nick.cmd"]
        msgs = [msg(f"m{i}") for i in range(4)]
        for m in msgs:
            await cb(m)                                    # 2 run, 1 waits, the 4th is refused at once
        await asyncio.sleep(0.01)
        assert peak[0] == 2 and running[0] == 2
        assert msgs[3].sent[0]["error"]["code"] == "busy" and msgs[3].sent[0]["id"] == "m3"
        release.set()
        await ch.stop()                                    # waits for the three to answer
        return msgs
    msgs = asyncio.run(run())
    assert [m.sent[0]["ok"] for m in msgs[:3]] == [True, True, True]
    assert msgs[0].sent[0]["output"] == "nats:p", "the hub is told who asked"
    assert peak[0] == 2
    assert [e["outcome"] for e in events(led) if e["outcome"] == "busy"] == ["busy"]


def test_a_crash_inside_a_command_becomes_an_error_reply_that_keeps_the_id(tmp_path):
    ch, h, _k, led, _c = make(tmp_path)

    async def boom(*a, **kw):
        raise RuntimeError("secret-token-xyz")
    h.call = boom
    r = ask(ch)
    assert not r["ok"] and r["error"]["code"] == "internal_error" and "secret" not in json.dumps(r)
    assert r["id"] == "r1"
    assert [e["outcome"] for e in reversed(events(led))] == ["forwarded", "internal_error"]
    assert "secret" not in json.dumps(events(led))


def test_missing_nats_py_is_a_clear_config_error(tmp_path, monkeypatch):
    ch, *_ = make(tmp_path)
    monkeypatch.setitem(sys.modules, "nats", None)
    with pytest.raises(ConfigError, match="nats-py"):
        asyncio.run(ch.start())
