#!/usr/bin/env python3
"""MCP tools for the QuantumOS provider broker (#287, ADR-0026).

Point an agent (Claude or any MCP client) at this server and it can get real resources
from the providers the broker has (OCI, qBraid, ...) inside the budget its person set:

  broker_providers  what can be provisioned, and whether spending is on
  broker_quote      the price, before anything is created
  broker_provision  create it on a lease (or get an approval id to show the person)
  broker_status     a lease and the provider's view of its resource
  broker_teardown   end a lease and delete its resource
  broker_budget     the envelope, what today has committed, what is left
  broker_ledger     what was done and what it cost (estimate and billed, side by side)

There is deliberately no tool that approves a request or changes the budget: those need
the approver key, which the person types into `provider_broker.py approve` themselves.
When a provision needs approval, the result says so and gives the exact command for the
person to run; the agent must ask them and wait.

This is a separate server from scripts/qos_mcp.py, so the frozen v1 tool surface
(ADR-0020, contracts/mcp/v1-tools.json) is unchanged. All logic is in the broker; each
tool is one call over the broker's Unix socket (BrokerClient, stdlib only).

  QOS_BROKER_SOCKET=/run/qos-broker/broker.sock python3 broker_mcp.py      (stdio)
  Dep: mcp>=1.0
"""
from __future__ import annotations

import json
import os
import socket

DEFAULT_SOCKET = "/run/qos-broker/broker.sock"


class BrokerClient:
    """One JSON request per call over the broker's socket (no state kept here)."""

    def __init__(self, path: str | None = None, timeout: float = 120.0):
        self.path = path or os.environ.get("QOS_BROKER_SOCKET", DEFAULT_SOCKET)
        self.timeout = timeout

    def call(self, op: str, **args) -> dict:
        req = {"op": op, **{k: v for k, v in args.items() if v is not None}}
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.settimeout(self.timeout)
                s.connect(self.path)
                s.sendall((json.dumps(req) + "\n").encode())
                buf = b""
                while not buf.endswith(b"\n"):
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
        except OSError as e:
            return {"ok": False, "error": "broker_unreachable",
                    "message": f"the provider broker is not reachable at {self.path}: {e}"}
        try:
            ans = json.loads(buf)
        except ValueError:
            return {"ok": False, "error": "bad_reply", "message": "the broker sent something that is not JSON"}
        if ans.get("error") == "approval_required":
            ans["tell_your_human"] = (
                "This needs your person's approval. Show them the quote and ask them to run: "
                f"{ans.get('how', '')}. Then call broker_provision again with this quote id and "
                f"approval='{ans.get('approval', '')}'. You cannot approve it yourself.")
        return ans


_client = BrokerClient()


def build():
    from mcp.server.fastmcp import FastMCP

    mcp = FastMCP("quantumos-broker")

    @mcp.tool()
    def broker_providers() -> dict:
        """List the providers this broker has, what each can provision (kinds, resource
        classes, units, list prices and spec fields), and whether real spending is on.
        With spending off, only resources quoted at exactly $0 can be provisioned."""
        return _client.call("providers")

    @mcp.tool()
    def broker_quote(provider: str, kind: str, spec: dict, duration_s: int) -> dict:
        """Price a resource before creating it. provider and kind come from
        broker_providers; spec is the kind's options (for example {"gb": 1} for an OCI
        bucket, or {"mode": "sim", "circuit": "<hex>", "shots": 100} for a qBraid job);
        duration_s is how long the lease should last. Returns a quote id (valid 10
        minutes, usable once), the most the lease can cost, whether that figure is exact
        or an estimate, any free-tier note, and whether it fits the person's envelope."""
        return _client.call("quote", provider=provider, kind=kind, spec=spec, duration_s=duration_s)

    @mcp.tool()
    def broker_provision(quote: str, approval: str = "") -> dict:
        """Create the quoted resource on a lease. If it fits the person's envelope it is
        created at once; otherwise the result is error 'approval_required' with an approval
        id and the command your person must run. Ask them, wait, then call again with the
        same quote and approval=<id>. The lease ends (and the resource is deleted) at its
        expiry, or earlier with broker_teardown."""
        return _client.call("provision", quote=quote, approval=approval or None)

    @mcp.tool()
    def broker_status(lease: str) -> dict:
        """One lease: state, time left, estimated and billed cost, and the provider's own
        view of the resource."""
        return _client.call("status", lease=lease)

    @mcp.tool()
    def broker_teardown(lease: str) -> dict:
        """End a lease now and delete its resource. Idempotent. Returns the estimate and,
        when the provider reports one, the billed cost."""
        return _client.call("teardown", lease=lease)

    @mcp.tool()
    def broker_budget() -> dict:
        """The person's envelope (per day, per action, allowed classes, longest lease),
        what today has committed, what is left, active leases, and whether spending is on."""
        return _client.call("budget")

    @mcp.tool()
    def broker_ledger(limit: int = 20) -> dict:
        """The most recent ledger events, newest first: envelope changes, quotes, approval
        requests and decisions, provisions and ends, with estimates and billed costs."""
        return _client.call("ledger", limit=limit)

    return mcp


def main(argv=None) -> None:
    """Console-script entry point (``qos-broker-mcp``): serve the MCP tools over stdio.

    With no arguments it speaks MCP on stdin/stdout, which is what a client's
    ``command`` entry expects. ``--help`` and ``--version`` answer and exit, so
    typing the command by hand does not leave a silent process waiting on stdin.
    """
    import argparse
    import sys as _sys
    ap = argparse.ArgumentParser(prog="qos-broker-mcp", description="The provider broker as MCP tools for an agent: quote, provision, status, end, ledger. Talks to a running `qos-broker serve` over its Unix socket (QOS_BROKER_SOCKET). No arguments: serve over stdio.")
    ap.add_argument("--version", action="store_true", help="print the quantumos-host-tools version and exit")
    a = ap.parse_args(argv if argv is not None else _sys.argv[1:])
    if a.version:
        try:
            from importlib.metadata import version as _v
            print(_v("quantumos-host-tools"))
        except Exception:  # noqa: BLE001 - a checkout without the package installed
            print("dev")
        return
    build().run()


if __name__ == "__main__":
    main()
