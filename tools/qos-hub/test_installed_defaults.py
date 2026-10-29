"""What the 2026-10-25 end-to-end test of the installed package found, held in place:
- a Python launcher's PYTHONHOME must not reach the programs the hub starts (uv venv on Windows);
- the person is never asked to approve a call that cannot run;
- a slow server answers with a timeout instead of holding the agent;
- CLI output reaches the agent without terminal colour codes;
- an installed package finds its kernel through QOS_KERNEL, and the kannaka binary by platform."""
from __future__ import annotations

import asyncio
import json
import os
import stat
import sys
from pathlib import Path
from types import SimpleNamespace as NS

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "provider-broker"))
sys.path.insert(0, str(HERE.parent.parent / "scripts"))
import cli_connector  # noqa: E402
import provider_broker as pb  # noqa: E402
import qos_bridge  # noqa: E402
import qos_hub as hub  # noqa: E402
from cli_connector import CliSession  # noqa: E402

FAKE = """#!{py}
import json, os, sys
if sys.argv[1:2] == ["env"]:
    print(json.dumps({{k: os.environ.get(k) for k in ("PYTHONHOME", "UV_INTERNAL__PYTHONHOME", "KEEP", "SET")}}))
else:
    print("\\x1b[1;33mStatus\\x1b[0m \\x1b[32m\\u25cf UP\\x1b[0m")
    print("\\x1b[2mdim\\x1b[0m", file=sys.stderr)
"""


@pytest.fixture
def cli(tmp_path):
    p = tmp_path / "fakecli"
    p.write_text(FAKE.format(py=sys.executable))
    p.chmod(p.stat().st_mode | stat.S_IEXEC)
    return str(p)


def session(binary, **env):
    return CliSession({"binary": [sys.executable, binary], "env": env, "timeout_s": 10, "tools": {
        "env": {"description": "env", "argv": ["env"]},
        "show": {"description": "show", "argv": ["show"]},
        "make": {"description": "make", "argv": ["make", "{name}"],
                 "params": {"name": {"type": "string", "pattern": "[A-Za-z0-9_-]{1,32}"}}}}})


def run(s, name, **a):
    return json.loads(asyncio.run(s.call_tool(name, a)).content[0].text)


def test_launcher_variables_do_not_reach_child_programs(cli, monkeypatch):
    monkeypatch.setenv("PYTHONHOME", "/some/other/interpreter")
    monkeypatch.setenv("UV_INTERNAL__PYTHONHOME", "/some/other/interpreter")
    monkeypatch.setenv("KEEP", "yes")
    assert "PYTHONHOME" not in cli_connector.child_env(None) and cli_connector.child_env(None)["KEEP"] == "yes"
    assert cli_connector.child_env({"PYTHONHOME": "/explicit"})["PYTHONHOME"] == "/explicit", "an explicit entry wins"
    monkeypatch.delenv("PYTHONHOME")       # the test's own python must start; check the strip on the child's view
    monkeypatch.setenv("UV_INTERNAL__PYTHONHOME", "/x")
    out = run(session(cli, SET="1"), "env")["stdout"]
    assert out["UV_INTERNAL__PYTHONHOME"] is None and out["KEEP"] == "yes" and out["SET"] == "1"


def test_cli_output_reaches_the_agent_without_colour_codes(cli):
    out = run(session(cli), "show")
    assert "\x1b" not in out["stdout"] and "Status" in out["stdout"] and "UP" in out["stdout"]
    assert "\x1b" not in out["stderr"] and "dim" in out["stderr"]


def make_hub(tmp_path, down, rules, timeouts=None):
    led = hub.HubLedger(str(tmp_path / "pb.db"))
    led.broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    h = hub.Hub(down, hub.Policy(rules), led, timeouts)
    asyncio.run(h.refresh())
    return h, led


def test_an_invalid_call_is_refused_before_the_person_is_asked(tmp_path, cli):
    h, led = make_hub(tmp_path, {"c": session(cli)}, {"c.*": "ask"})
    for bad in ({"name": "--help"}, {"name": "x; rm -rf /"}, {"name": "ok", "extra": 1}, {}):
        r = asyncio.run(h.call("c.make", bad))
        assert r["error"] == "bad_arguments" and "approval" not in r, (bad, r)
    assert led.pending() == [], "nothing was stored for the person to approve"
    r = asyncio.run(h.call("c.make", {"name": "fine"}))
    assert r["error"] == "approval_required" and len(led.pending()) == 1


class Slow:
    async def list_tools(self):
        return NS(tools=[NS(name="wait", description="", inputSchema={"type": "object"})])

    async def call_tool(self, name, arguments):
        await asyncio.sleep(30)


def test_a_slow_server_answers_with_a_timeout(tmp_path):
    h, led = make_hub(tmp_path, {"slow": Slow()}, {"slow.*": "allow"}, {"slow": 0.2})
    r = asyncio.run(h.call("slow.wait", {}))
    assert r["error"] == "timeout" and "0.2" in r["message"]
    assert any(e["event"] == "hub_server_error" for e in led.broker.ledger()["events"])


def test_an_installed_package_finds_its_kernel_through_QOS_KERNEL(tmp_path, monkeypatch):
    k = tmp_path / "quantumos-kernel.elf32"
    k.write_bytes(b"\x7fELF")
    monkeypatch.setenv("QOS_KERNEL", str(k))
    assert qos_bridge._default_kernel() == str(k)
    assert qos_bridge.QosVM().kernel == str(k)
    monkeypatch.delenv("QOS_KERNEL")
    assert qos_bridge._default_kernel().endswith(os.path.join("build", "x86_64", "kernel.elf32"))


def test_the_kannaka_binary_default_fits_the_platform(monkeypatch):
    monkeypatch.delenv("QOS_KANNAKA_BIN", raising=False)
    monkeypatch.setattr("shutil.which", lambda name: None)
    path = qos_bridge._default_kannaka_bin()
    assert path.endswith("kannaka.exe" if os.name == "nt" else os.path.join("release", "kannaka"))
    monkeypatch.setattr("shutil.which", lambda name: "/usr/local/bin/kannaka")
    assert qos_bridge._default_kannaka_bin() == "/usr/local/bin/kannaka"
    monkeypatch.setenv("QOS_KANNAKA_BIN", "/stub")
    assert qos_bridge._default_kannaka_bin() == "/stub"


def test_the_example_scada_templates_name_real_commands():
    d = json.loads((HERE / "hub.example.json").read_text())
    tools = d["servers"]["scada"]["cli"]["tools"]
    real = {"status", "sites", "assets", "events", "blockchain"}      # 0xSCADA CLI top-level commands
    assert {t["argv"][0] for t in tools.values()} <= real, "no template may invent a command (there is no 'alarms')"
    assert d["policy"]["scada.asset_create"] == "ask"


def test_the_example_templates_for_the_stale_repos_name_real_commands():
    """kannaka-hdl, kannaka-apps and kannaka-buzz entries: real subcommands, writes ask."""
    d = json.loads((HERE / "hub.example.json").read_text())
    first = lambda s: {t["argv"][0] for t in d["servers"][s]["cli"]["tools"].values()}
    assert first("hdl") <= {"check", "grow"}                 # kannaka-hdl's CLI verbs
    assert first("apps") <= {"list", "run"}                  # khdl-app <run|list>
    assert first("buzz") <= {"channels", "messages"}         # buzz-cli groups used here
    plan = d["servers"]["apps"]["cli"]["tools"]["plan"]["argv"]
    assert plan[-1] == "--dry-run", "plan must never execute an app"
    for tool, policy in (("apps.run", "ask"), ("buzz.send", "ask"), ("apps.plan", "allow"), ("hdl.grow", "allow")):
        assert d["policy"][tool] == policy
    for s in ("hdl", "apps", "buzz"):
        for t in d["servers"][s]["cli"]["tools"].values():
            for a in t["argv"]:
                assert "{" not in a or (a.startswith("{") and a.endswith("}")), f"{s}: placeholder must fill a whole argv slot: {a}"


class Colourful:
    async def list_tools(self):
        return NS(tools=[NS(name="status", description="", inputSchema={"type": "object"})])

    async def call_tool(self, name, arguments):
        return NS(content=[NS(type="text", text="[1;33mFirst time?[0m [32mready[0m")], isError=False)


def test_mcp_server_text_reaches_the_agent_without_colour_codes(tmp_path):
    h, _ = make_hub(tmp_path, {"octo": Colourful()}, {"octo.*": "allow"})
    r = asyncio.run(h.call("octo.status", {}))
    assert r["ok"] and r["result"] == "First time? ready"
