#!/usr/bin/env python3
"""Offline checks for qos_agent's hub support: no VM, no API key, no network.

Behind the hub (`--server-cmd "qos-hub serve ..."`) tool names are namespaced,
e.g. `qos.qos_boot`. Both model APIs reject a dot in a tool name, so qos_agent
renames such tools for the model and maps every call back. This test pins:
  - illegal names are rewritten and mapped back; legal names are untouched
  - _WireSession.call_tool calls the MCP name, not the wire name
  - --server-cmd launches the given command instead of qos_mcp.py
  - cleanup finds namespaced shutdown tools

Needs only the `mcp` package (for Tool and StdioServerParameters).
Run: python3 scripts/test_qos_agent_wire.py
"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qos_agent  # noqa: E402
from mcp.types import Tool  # noqa: E402

FAILED = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {name}{'' if ok else ' — ' + detail}")
    if not ok:
        FAILED.append(name)


def tool(name):
    return Tool(name=name, description="d", inputSchema={"type": "object", "properties": {}})


class FakeSession:
    def __init__(self, names):
        self.names = names
        self.calls = []

    async def list_tools(self):
        class R:
            pass
        r = R()
        r.tools = [tool(n) for n in self.names]
        return r

    async def call_tool(self, name, arguments=None, *a, **kw):
        self.calls.append((name, arguments))
        return "ok"


def main():
    tools, mapping = qos_agent._wire_tools(
        [tool("qos.qos_boot"), tool("qos_status"), tool("kannaka.kannaka_status")])
    names = [t.name for t in tools]
    check("dotted names are rewritten", names == ["qos__qos_boot", "qos_status", "kannaka__kannaka_status"],
          str(names))
    check("every wire name is legal", all(qos_agent._WIRE_NAME_OK.match(n) for n in names), str(names))
    check("mapping returns the MCP name", mapping.get("qos__qos_boot") == "qos.qos_boot"
          and mapping.get("qos_status") == "qos_status", str(mapping))

    fake = FakeSession([])
    wire = qos_agent._WireSession(fake, mapping)
    asyncio.run(wire.call_tool("qos__qos_boot", {"qseed": "1"}))
    asyncio.run(wire.call_tool("qos_status", {}))
    check("call_tool maps back to the MCP name",
          fake.calls == [("qos.qos_boot", {"qseed": "1"}), ("qos_status", {})], str(fake.calls))
    check("other attributes pass through", wire.names == [])

    p = qos_agent._server_params("qos-hub serve --config /tmp/h.json --db /tmp/b.db")
    check("--server-cmd launches the given command",
          p.command == "qos-hub" and p.args == ["serve", "--config", "/tmp/h.json", "--db", "/tmp/b.db"],
          f"{p.command} {p.args}")
    p = qos_agent._server_params()
    check("default still launches qos_mcp.py", p.args and p.args[0].endswith("qos_mcp.py"), str(p.args))

    hub = FakeSession(["qos.qos_boot", "qos.qos_shutdown", "qos.qos_society_shutdown", "kannaka.kannaka_status"])
    asyncio.run(qos_agent._cleanup(hub))
    called = sorted(n for n, _ in hub.calls)
    check("cleanup calls the namespaced shutdown tools",
          called == ["qos.qos_shutdown", "qos.qos_society_shutdown"], str(called))
    direct = FakeSession(["qos_boot", "qos_shutdown", "qos_society_shutdown"])
    asyncio.run(qos_agent._cleanup(direct))
    check("cleanup still works without the hub",
          sorted(n for n, _ in direct.calls) == ["qos_shutdown", "qos_society_shutdown"], str(direct.calls))

    print(f"{'ALL PASSED' if not FAILED else str(len(FAILED)) + ' FAILED'}")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
