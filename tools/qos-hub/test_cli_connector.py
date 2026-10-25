"""Tests for the hub's CLI connector (#297 slice 3), with a fake CLI that prints its argv."""
from __future__ import annotations

import asyncio
import json
import stat
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qos_hub as hub  # noqa: E402
import provider_broker as pb  # noqa: E402
from cli_connector import CliSession, ConfigError  # noqa: E402

FAKE = """#!{py}
import json, sys, time
args = sys.argv[1:]
if args[:1] == ["sleep"]:
    time.sleep(30)
if args[:1] == ["fail"]:
    print("boom", file=sys.stderr); sys.exit(3)
if args[:1] == ["text"]:
    print("plain words"); sys.exit(0)
print(json.dumps({{"argv": args}}))
"""


@pytest.fixture
def cli(tmp_path):
    p = tmp_path / "fakecli"
    p.write_text(FAKE.format(py=sys.executable))
    p.chmod(p.stat().st_mode | stat.S_IEXEC)
    return str(p)


def spec(binary, **extra):
    return {"binary": binary, "timeout_s": extra.pop("timeout_s", 10), "tools": {
        "observe": {"description": "observe", "argv": ["observe", "--json"]},
        "recall": {"description": "recall", "argv": ["recall", "{query}", "--top-k", "{k}"],
                   "params": {"query": {"type": "string", "max_len": 80},
                              "k": {"type": "integer", "min": 1, "max": 20, "default": 5}}},
        "market": {"description": "market", "argv": ["market", "{action}", "{id}"],
                   "params": {"action": {"type": "string", "enum": ["list", "show"]},
                              "id": {"type": "string", "pattern": "[a-z0-9-]{1,40}", "default": "all"}}},
        "flag": {"description": "flag", "argv": ["flag", "{on}"], "params": {"on": {"type": "boolean"}}},
        "sleep": {"description": "sleep", "argv": ["sleep"]},
        "fail": {"description": "fail", "argv": ["fail"]},
        "text": {"description": "text", "argv": ["text"]},
        **extra.pop("tools", {})}}


def run(s, name, **a):
    r = asyncio.run(s.call_tool(name, a))
    return json.loads(r.content[0].text), r.isError


def test_argv_is_exactly_the_template_with_validated_values(cli):
    s = CliSession(spec(cli))
    out, err = run(s, "observe")
    assert not err and out["stdout"] == {"argv": ["observe", "--json"]} and out["exit_code"] == 0
    out, _ = run(s, "recall", query="the tide; rm -rf / $(whoami) `id`", k=3)
    assert out["stdout"]["argv"] == ["recall", "the tide; rm -rf / $(whoami) `id`", "--top-k", "3"], \
        "one argv element, no shell: the metacharacters are inert text"
    out, _ = run(s, "recall", query="defaults")
    assert out["stdout"]["argv"][-1] == "5"
    out, _ = run(s, "flag", on=True)
    assert out["stdout"]["argv"] == ["flag", "true"]


@pytest.mark.parametrize("tool,args,why", [
    ("recall", {"query": "--delete-everything"}, "may not start with '-'"),
    ("recall", {"query": "x" * 81}, "too long"),
    ("recall", {"query": "ok", "k": 50}, "integer from 1 to 20"),
    ("recall", {"query": "ok", "k": True}, "integer"),
    ("recall", {}, "missing parameter query"),
    ("recall", {"query": "ok", "rogue": 1}, "unknown parameters"),
    ("market", {"action": "delete"}, "must be one of"),
    ("market", {"action": "show", "id": "a/../../etc"}, "does not match"),
    ("flag", {"on": "yes"}, "true or false"),
])
def test_bad_arguments_never_run(cli, tool, args, why):
    out, err = run(CliSession(spec(cli)), tool, **args)
    assert err and out["error"] == "bad_arguments" and why in out["message"]


def test_timeout_nonzero_exit_and_plain_text(cli):
    s = CliSession(spec(cli, timeout_s=1))
    out, err = run(s, "sleep")
    assert err and out["error"] == "timeout"
    out, err = run(s, "fail")
    assert err and out["exit_code"] == 3 and "boom" in out["stderr"]
    out, err = run(s, "text")
    assert not err and out["stdout"].strip() == "plain words"


@pytest.mark.parametrize("bad", [
    {"x": {"argv": ["a", "--name={who}"], "params": {"who": {"type": "string"}}}},   # placeholder inside a string
    {"x": {"argv": ["a", "{who}"]}},                                                   # placeholder without a param
    {"x": {"argv": ["a"], "params": {"who": {"type": "string"}}}},                    # param never used
    {"x": {"argv": "a b"}},                                                            # argv not a list
    {"x": {"argv": ["{n}"], "params": {"n": {"type": "float"}}}},                     # unknown type
])
def test_unsafe_configs_are_refused(cli, bad):
    with pytest.raises(ConfigError):
        CliSession({"binary": cli, "tools": bad})


def test_schema_is_strict(cli):
    tools = {t.name: t for t in asyncio.run(CliSession(spec(cli)).list_tools()).tools}
    sch = tools["recall"].inputSchema
    assert sch["additionalProperties"] is False and sch["required"] == ["query"]
    assert sch["properties"]["k"] == {"type": "integer", "minimum": 1, "maximum": 20, "default": 5}


def test_a_cli_under_hub_policy(cli, tmp_path):
    led = hub.HubLedger(str(tmp_path / "pb.db"))
    key = led.broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    h = hub.Hub({"kcli": CliSession(spec(cli))},
                hub.Policy({"kcli.observe": "allow", "kcli.recall": "allow", "kcli.market": "ask"}), led)
    asyncio.run(h.refresh())
    ok = asyncio.run(h.call("kcli.observe", {}))
    assert ok["ok"] and ok["result"]["stdout"] == {"argv": ["observe", "--json"]}
    ask = asyncio.run(h.call("kcli.market", {"action": "show", "id": "m-42"}))
    assert ask["error"] == "approval_required"
    led.decide(ask["approval"], key)
    done = asyncio.run(h.call("hub_call_approved", {"approval": ask["approval"]}))
    assert done["result"]["stdout"]["argv"] == ["market", "show", "m-42"]
    assert asyncio.run(h.call("kcli.flag", {"on": True}))["error"] == "approval_required", "unlisted: asks"


def test_command_prefix_and_working_directory(cli, tmp_path):
    (tmp_path / "charters").mkdir()
    s = CliSession({"binary": [sys.executable, cli], "cwd": str(tmp_path), "tools": {
        "govern": {"description": "steward govern", "argv": ["govern", "--charter", "{charter}", "--warrant", "{warrant}"],
                   "params": {"charter": {"type": "string", "pattern": "charters/[a-z0-9_-]{1,40}[.]yaml"},
                              "warrant": {"type": "string", "enum": ["granted", "revoked", "overscope"]}}}}})
    out, err = run(s, "govern", charter="charters/treasury.yaml", warrant="granted")
    assert not err and out["stdout"]["argv"] == ["govern", "--charter", "charters/treasury.yaml", "--warrant", "granted"]
    out, err = run(s, "govern", charter="../../etc/passwd", warrant="granted")
    assert err and "does not match" in out["message"]
    out, err = run(s, "govern", charter="charters/a.yaml", warrant="live:http://evil")
    assert err and "must be one of" in out["message"]
    with pytest.raises(ConfigError):
        CliSession({"binary": [], "tools": {}})
