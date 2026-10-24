"""Tests for the broker's MCP tools (#287): the client against a real broker socket, and
the tool surface itself (when the mcp package is installed)."""
from __future__ import annotations

import asyncio
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import broker_mcp  # noqa: E402
import provider_broker as pb  # noqa: E402
from plugin_mock import MockProvider  # noqa: E402

TOOLS = {
    "broker_providers": [],
    "broker_quote": ["provider", "kind", "spec", "duration_s"],
    "broker_provision": ["quote", "approval"],
    "broker_status": ["lease"],
    "broker_teardown": ["lease"],
    "broker_budget": [],
    "broker_ledger": ["limit"],
}


class Running:
    """A provider broker serving its socket in a thread, as `serve` does."""

    def __init__(self, tmp_path):
        self.b = pb.Broker(pb.Ledger(str(tmp_path / "b.db")), {"mock": MockProvider()}, allow_spend=True)
        self.key = self.b.set_envelope(pb.Envelope(1.0, 0.5, ["storage", "compute"], 86400))
        self.sock = str(tmp_path / "b.sock")
        self.loop = asyncio.new_event_loop()
        ready = threading.Event()

        def run():
            asyncio.set_event_loop(self.loop)

            async def main():
                self.server = await asyncio.start_unix_server(self.b.serve_conn, self.sock)
                ready.set()
                async with self.server:
                    await self.server.serve_forever()
            try:
                self.loop.run_until_complete(main())
            except asyncio.CancelledError:
                pass
        self.t = threading.Thread(target=run, daemon=True)
        self.t.start()
        assert ready.wait(5)

    def close(self):
        def stop():
            self.server.close()
            for task in asyncio.all_tasks(self.loop):
                task.cancel()
        self.loop.call_soon_threadsafe(stop)
        self.t.join(5)
        self.loop.close()


@pytest.fixture
def running(tmp_path):
    r = Running(tmp_path)
    yield r
    r.close()


def test_client_full_lifecycle_and_approval_hint(running):
    c = broker_mcp.BrokerClient(running.sock, timeout=10)
    assert c.call("providers")["ok"]
    q = c.call("quote", provider="mock", kind="vm", spec={}, duration_s=3600)
    lease = c.call("provision", quote=q["quote"], approval=None)
    assert lease["ok"] and c.call("status", lease=lease["lease"])["state"] == "active"
    g = c.call("quote", provider="mock", kind="gpu", spec={}, duration_s=3600)
    ans = c.call("provision", quote=g["quote"])
    assert ans["error"] == "approval_required"
    assert "cannot approve it yourself" in ans["tell_your_human"] and ans["approval"] in ans["tell_your_human"]
    running.b.approve(ans["approval"], running.key)          # the person, with their key
    assert c.call("provision", quote=g["quote"], approval=ans["approval"])["ok"]
    assert c.call("teardown", lease=lease["lease"])["state"] == "ended"
    assert c.call("budget")["active_leases"] == 1
    assert len(c.call("ledger", limit=2)["events"]) == 2


def test_client_reports_an_unreachable_broker(tmp_path):
    ans = broker_mcp.BrokerClient(str(tmp_path / "none.sock"), timeout=2).call("budget")
    assert ans["ok"] is False and ans["error"] == "broker_unreachable"


def test_tool_surface_has_no_way_to_approve_or_raise_the_budget():
    pytest.importorskip("mcp")
    server = broker_mcp.build()
    tools = asyncio.run(server.list_tools())
    got = {t.name: sorted((t.inputSchema or {}).get("properties", {})) for t in tools}
    assert got == {k: sorted(v) for k, v in TOOLS.items()}
    for t in tools:
        assert not any(w in t.name for w in ("approve", "envelope", "allow")), t.name
