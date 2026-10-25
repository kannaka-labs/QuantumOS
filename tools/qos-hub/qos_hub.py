#!/usr/bin/env python3
"""QuantumOS hub: one MCP endpoint in front of the ecosystem's MCP servers, with a policy
on every tool (ADR-0026's agent surface, generalised; #297).

Point an agent at this one server instead of at each tool's own. The hub starts the
downstream MCP servers named in its config (kannaka memory and swarm, the provider broker,
0xSCADA, ...), re-exports their tools under a namespace ("kannaka.kannaka_recall"), and
decides every call by policy:

  allow   forwarded at once (reads, local writes)
  ask     NOT forwarded: the exact call (tool + arguments) is stored and an approval id is
          returned. The person approves it with the approver key, then the agent runs it
          with hub_call_approved(approval). Arguments are frozen at approval time, so an
          approval can never be spent on a different call. (Publishing to a shared bus,
          spending credits, anything the person wants to see first.)
  deny    refused

A tool the config does not mention gets the config's "default" policy, which defaults to
"ask": a new tool appearing downstream never becomes silently callable.

Every call, its decision and its outcome go to the ledger. The ledger and the approver key
are the provider broker's (tools/provider-broker): one database, one key, so the person
approves hub calls and paid resources the same way, with `qos_hub.py approve <id>`.

Config (JSON):
  {"servers": {"kannaka": {"command": "node", "args": [".../kannaka-mcp.mjs"], "env": {}},
               "broker":  {"command": "python3", "args": [".../broker_mcp.py"],
                           "env": {"QOS_BROKER_SOCKET": "/run/qos-broker/broker.sock"}}},
   "policy": {"kannaka.kannaka_recall": "allow", "kannaka.swarm_send": "ask",
              "kannaka.compute_grant": "deny", "broker.*": "allow"},
   "default": "ask"}
Policy keys are tool names or fnmatch patterns; the most specific (longest) match wins.
A server entry may instead be {"cli": {...}}: a CLI with no MCP server, exposed through
allowlisted argv templates (cli_connector.py).
Or {"url": "https://host/api/mcp", "auth": {"bearer_file": "~/token.txt"} | {"bearer_env": "NAME"}}:
an MCP server over HTTP (streamable HTTP; plain JSON-RPC replies work too). The token is read
from that file or variable at start, sent as "Authorization: Bearer ...", and never logged or
stored in the config. Short-lived tokens (KAX: 15 minutes) can be renewed: add
"refresh_url" (POST with the current token, reply {"token": ...}) and "refresh_every_s"; the
new token is used from the next request on and written back to bearer_file (0600).

Hub tools (always present): hub_tools (every tool with its policy), hub_call_approved,
hub_ledger. There is deliberately no tool to approve or to change the policy.

  qos_hub.py serve   --config hub.json --db broker.db      (stdio MCP server)
  qos_hub.py approve --db broker.db <approval id> [--deny]  (the person; prompts for the key)
  qos_hub.py pending --db broker.db

Trust boundary: as for the broker, these rules hold against an agent that reaches tools only
through this server. Downstream servers run with the hub's own permissions.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import fnmatch
import getpass
import hashlib
import json
import logging
import os
import secrets
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "provider-broker"))
import provider_broker as pb  # noqa: E402

log = logging.getLogger("qos-hub")

POLICIES = ("allow", "ask", "deny")
SEP = "."
APPROVAL_TTL_S = 3600
MAX_RESULT = 64 * 1024

HUB_SCHEMA = """
CREATE TABLE IF NOT EXISTS hub_approvals (id TEXT PRIMARY KEY, owner TEXT, tool TEXT, arguments TEXT,
    digest TEXT, state TEXT, created REAL, decided REAL);
"""


def digest(tool: str, arguments: dict) -> str:
    return hashlib.sha256(json.dumps([tool, arguments], sort_keys=True, separators=(",", ":")).encode()).hexdigest()


class Policy:
    def __init__(self, rules: dict, default: str = "ask"):
        for k, v in list(rules.items()) + [("default", default)]:
            if v not in POLICIES:
                raise ValueError(f"policy for {k!r} must be one of {POLICIES}, not {v!r}")
        self.rules, self.default = dict(rules), default

    def of(self, tool: str) -> str:
        if tool in self.rules:
            return self.rules[tool]
        hits = [(len(p), v) for p, v in self.rules.items() if fnmatch.fnmatchcase(tool, p)]
        return max(hits)[1] if hits else self.default


class HubLedger:
    """Hub approvals and events, in the provider broker's database (same approver key)."""

    def __init__(self, db_path: str, clock=time.time):
        self.l = pb.Ledger(db_path)
        self.l.db.executescript(HUB_SCHEMA)
        self.l.db.commit()
        self.broker = pb.Broker(self.l, {})        # for the approver key only
        self.clock = clock

    def event(self, what: str, **detail) -> None:
        self.l.event(self.clock(), pb.OWNER, None, what, **detail)

    def request(self, tool: str, arguments: dict) -> str:
        aid = "h" + secrets.token_hex(5)
        self.l.q("INSERT INTO hub_approvals(id, owner, tool, arguments, digest, state, created) VALUES(?,?,?,?,?,?,?)",
                 aid, pb.OWNER, tool, json.dumps(arguments, sort_keys=True), digest(tool, arguments),
                 "pending", self.clock())
        self.event("hub_approval_requested", approval=aid, tool=tool, digest=digest(tool, arguments)[:16])
        return aid

    def decide(self, aid: str, key: str, deny: bool = False) -> dict:
        with self.broker.lock:
            self.broker._check_key(pb.OWNER, key)
            r = self.l.q("SELECT * FROM hub_approvals WHERE id=? AND owner=?", aid, pb.OWNER)
            if not r or r[0]["state"] != "pending":
                raise pb.Refused("no_such_approval", "no pending hub approval with that id")
            state = "denied" if deny else "approved"
            self.l.q("UPDATE hub_approvals SET state=?, decided=? WHERE id=?", state, self.clock(), aid)
            self.event("hub_approval_" + state, approval=aid, tool=r[0]["tool"])
            return {"approval": aid, "tool": r[0]["tool"], "state": state}

    def take(self, aid: str) -> tuple[str, dict]:
        """An approved, unexpired call, marked used. Raises Refused otherwise."""
        r = self.l.q("SELECT * FROM hub_approvals WHERE id=? AND owner=?", aid, pb.OWNER)
        if not r:
            raise pb.Refused("no_such_approval", "no hub approval with that id")
        row = r[0]
        if row["state"] != "approved":
            raise pb.Refused("not_approved", f"approval {aid} is {row['state']}")
        if self.clock() - row["created"] > APPROVAL_TTL_S:
            raise pb.Refused("approval_expired", "that approval is more than an hour old; ask again")
        args = json.loads(row["arguments"])
        if digest(row["tool"], args) != row["digest"]:
            raise pb.Refused("approval_tampered", "the stored call does not match its digest")
        self.l.q("UPDATE hub_approvals SET state='used' WHERE id=?", aid)
        return row["tool"], args

    def pending(self) -> list[dict]:
        rows = self.l.q("SELECT id, tool, arguments, created FROM hub_approvals WHERE owner=? AND state='pending' "
                        "ORDER BY created", pb.OWNER)
        return [{"approval": r["id"], "tool": r["tool"], "arguments": json.loads(r["arguments"]),
                 "age_s": int(self.clock() - r["created"])} for r in rows]


class Owned:
    """One downstream server held open by its own task, so a server that fails (a 401 while
    connecting, a crash later) takes down only itself, never the hub."""

    def __init__(self, ns: str, ctx, ledger: HubLedger):
        self.ns, self.ctx, self.ledger = ns, ctx, ledger
        self.done = asyncio.Event()
        self.task: asyncio.Task | None = None

    async def start(self):
        ready: asyncio.Future = asyncio.get_running_loop().create_future()

        async def owner():
            try:
                async with self.ctx as session:
                    ready.set_result(session)
                    await self.done.wait()
            except BaseException as e:  # noqa: BLE001
                if not ready.done():
                    ready.set_exception(e)
                elif not isinstance(e, asyncio.CancelledError):
                    log.warning("server %s stopped: %s", self.ns, type(e).__name__)
                    self.ledger.event("hub_server_error", server=self.ns, error=type(e).__name__)
        self.task = asyncio.create_task(owner())
        return await ready

    async def stop(self) -> None:
        self.done.set()
        if self.task is not None:
            with contextlib.suppress(BaseException):
                await asyncio.wait_for(self.task, 5)


class Hub:
    """The policy core, independent of MCP plumbing (so it is testable with plain callables).
    `downstream` maps a namespace to an object with async list_tools() -> [Tool] and
    async call_tool(name, arguments) -> CallToolResult."""

    def __init__(self, downstream: dict, policy: Policy, ledger: HubLedger):
        self.down, self.policy, self.ledger = downstream, policy, ledger
        self.tools: dict[str, tuple[str, object]] = {}     # exported name -> (namespace, Tool)

    async def refresh(self) -> None:
        tools = {}
        for ns, session in self.down.items():
            try:
                listed = await session.list_tools()
            except Exception as e:  # noqa: BLE001 - one broken server must not hide the rest
                log.warning("list_tools on %s failed: %s", ns, e)
                self.ledger.event("hub_server_error", server=ns, error=f"{type(e).__name__}: {e}"[:200])
                continue
            for t in listed.tools:
                tools[ns + SEP + t.name] = (ns, t)
        self.tools = tools

    def catalog(self) -> list[dict]:
        return [{"tool": name, "policy": self.policy.of(name), "description": (t.description or "")[:200]}
                for name, (_ns, t) in sorted(self.tools.items())]

    async def _forward(self, tool: str, arguments: dict) -> dict:
        ns, t = self.tools[tool]
        try:
            r = await self.down[ns].call_tool(t.name, arguments)
        except Exception as e:  # noqa: BLE001 - a dead server answers with an error, never a crash
            self.ledger.event("hub_server_error", server=ns, error=type(e).__name__)
            return {"ok": False, "error": "server_error", "message": f"{ns} failed: {type(e).__name__}"}
        texts = [c.text for c in r.content if getattr(c, "type", "") == "text"]
        body = "\n".join(texts)
        if len(body) > MAX_RESULT:
            body = body[:MAX_RESULT] + "\n[truncated by the hub]"
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = None
        return {"ok": not r.isError, "result": parsed if parsed is not None else body}

    async def call(self, tool: str, arguments: dict) -> dict:
        if tool == "hub_tools":
            return {"ok": True, "tools": self.catalog()}
        if tool == "hub_ledger":
            return {"ok": True, **self.ledger.broker.ledger(limit=int(arguments.get("limit") or 20))}
        if tool == "hub_call_approved":
            try:
                target, args = self.ledger.take(str(arguments.get("approval", "")))
            except pb.Refused as e:
                return {"ok": False, "error": e.code, "message": str(e)}
            if target not in self.tools:
                return {"ok": False, "error": "no_such_tool", "message": f"{target} is not available now"}
            out = await self._forward(target, args)
            self.ledger.event("hub_call", tool=target, decision="approved", ok=out["ok"],
                              approval=str(arguments.get("approval")))
            return out
        if tool not in self.tools:
            return {"ok": False, "error": "no_such_tool", "message": f"no tool {tool!r}; see hub_tools"}
        decision = self.policy.of(tool)
        if decision == "deny":
            self.ledger.event("hub_call", tool=tool, decision="deny")
            return {"ok": False, "error": "denied", "message": f"{tool} is denied by this hub's policy"}
        if decision == "ask":
            aid = self.ledger.request(tool, arguments)
            return {"ok": False, "error": "approval_required", "approval": aid,
                    "tell_your_human": (f"{tool} needs your person's approval. Show them the call and ask them to "
                                        f"run: qos_hub.py approve {aid}. Then call hub_call_approved with "
                                        f"approval='{aid}' (it runs exactly the stored call). You cannot approve it.")}
        out = await self._forward(tool, arguments)
        self.ledger.event("hub_call", tool=tool, decision="allow", ok=out["ok"])
        return out


class BearerToken:
    """The current bearer token for one HTTP server, attached to every request (an httpx
    Auth), so a refresh takes effect on the open session without reconnecting."""

    def __init__(self, token: str, spec: dict, ledger: "HubLedger", ns: str):
        self.token, self.ledger, self.ns = token, ledger, ns
        auth = spec.get("auth") or {}
        self.refresh_url = auth.get("refresh_url")
        self.every = float(auth.get("refresh_every_s", 600))
        self.file = Path(os.path.expanduser(auth["bearer_file"])) if "bearer_file" in auth else None

    def httpx_auth(self):
        import httpx
        holder = self

        class _Auth(httpx.Auth):
            def auth_flow(self, request):
                request.headers["Authorization"] = f"Bearer {holder.token}"
                yield request
        return _Auth()

    async def refresh_once(self) -> bool:
        import httpx
        try:
            async with httpx.AsyncClient(timeout=20) as c:
                r = await c.post(self.refresh_url, headers={"Authorization": f"Bearer {self.token}"})
            r.raise_for_status()
            new = (r.json() or {}).get("token")
            if not isinstance(new, str) or not new:
                raise ValueError("no token in the refresh reply")
        except Exception as e:  # noqa: BLE001 - keep the current token; never echo either one
            log.warning("token refresh for %s failed: %s", self.ns, type(e).__name__)
            self.ledger.event("hub_token_refresh", server=self.ns, ok=False, error=type(e).__name__)
            return False
        self.token = new
        if self.file is not None:
            tmp = self.file.with_name(self.file.name + ".tmp")
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(new)
            os.replace(tmp, self.file)
        self.ledger.event("hub_token_refresh", server=self.ns, ok=True)
        return True

    async def keep_fresh(self) -> None:
        while True:
            await asyncio.sleep(self.every)
            await self.refresh_once()


def http_headers(spec: dict) -> dict:
    """The Authorization header for an HTTP server, from a token file or variable named in
    the config. Raises ValueError if the config names one that is missing or empty."""
    auth = spec.get("auth") or {}
    token = None
    if "bearer_file" in auth:
        try:
            token = Path(os.path.expanduser(auth["bearer_file"])).read_text().strip()
        except OSError as e:
            raise ValueError(f"cannot read the bearer token file: {type(e).__name__}") from None
    elif "bearer_env" in auth:
        token = os.environ.get(auth["bearer_env"], "").strip()
    elif auth:
        raise ValueError("auth must be {'bearer_file': path} or {'bearer_env': NAME}")
    if auth and not token:
        raise ValueError("the configured bearer token is empty")
    return {"Authorization": f"Bearer {token}"} if token else {}


HUB_TOOLS = {
    "hub_tools": ("Every tool this hub offers, with its policy: allow (runs now), ask (needs the "
                  "person's approval first) or deny.", {"type": "object", "properties": {}}),
    "hub_call_approved": ("Run a call the person approved, exactly as it was stored when it was asked "
                          "for (the arguments cannot be changed).",
                          {"type": "object", "properties": {"approval": {"type": "string"}}, "required": ["approval"]}),
    "hub_ledger": ("Recent ledger events, newest first: hub calls and their decisions, approvals, and the "
                   "provider broker's quotes, leases and costs.",
                   {"type": "object", "properties": {"limit": {"type": "integer"}}}),
}


# ---- MCP plumbing ----------------------------------------------------------------

async def serve(config: dict, db: str) -> None:
    import mcp.types as types
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    from mcp.client.streamable_http import streamablehttp_client
    from mcp.server.lowlevel import Server
    from mcp.server.stdio import stdio_server

    ledger = HubLedger(db)

    def stdio_ctx(spec):
        params = StdioServerParameters(command=os.path.expanduser(spec["command"]),
                                       args=[os.path.expanduser(x) for x in spec.get("args") or []],
                                       env={**os.environ, **{k: os.path.expanduser(v) for k, v in
                                                             (spec.get("env") or {}).items()}})

        @contextlib.asynccontextmanager
        async def ctx():
            async with stdio_client(params) as (r, w), ClientSession(r, w) as s:
                await s.initialize()
                yield s
        return ctx()

    refreshers: list[asyncio.Task] = []

    def http_ctx(ns, spec):
        headers = http_headers(spec)
        auth = None
        if headers and (spec.get("auth") or {}).get("refresh_url"):
            bearer = BearerToken(headers.pop("Authorization").split(" ", 1)[1], spec, ledger, ns)
            auth = bearer.httpx_auth()
            refreshers.append(asyncio.create_task(bearer.keep_fresh()))

        @contextlib.asynccontextmanager
        async def ctx():
            async with streamablehttp_client(spec["url"], headers=headers, auth=auth) as (r, w, _sid), \
                    ClientSession(r, w) as s:
                await s.initialize()
                yield s
        return ctx()

    owners: list[Owned] = []
    down = {}
    for ns, spec in (config.get("servers") or {}).items():
        if SEP in ns:
            raise SystemExit(f"server name {ns!r} must not contain {SEP!r}")
        if "cli" in spec:                     # a CLI without an MCP server (cli_connector.py)
            from cli_connector import CliSession
            down[ns] = CliSession(spec["cli"])
            continue
        try:
            owned = Owned(ns, http_ctx(ns, spec) if "url" in spec else stdio_ctx(spec), ledger)
            down[ns] = await owned.start()
            owners.append(owned)
        except BaseException as e:  # noqa: BLE001 - start what can start; never echo a token
            if isinstance(e, (KeyboardInterrupt, SystemExit)):
                raise
            log.warning("server %s did not start: %s", ns, type(e).__name__)
            ledger.event("hub_server_error", server=ns, error=type(e).__name__)
    try:
        hub = Hub(down, Policy(config.get("policy") or {}, config.get("default", "ask")), ledger)
        await hub.refresh()
        server = Server("quantumos-hub")

        @server.list_tools()
        async def list_tools() -> list[types.Tool]:
            out = [types.Tool(name=n, description=d, inputSchema=s) for n, (d, s) in HUB_TOOLS.items()]
            for name, (_ns, t) in sorted(hub.tools.items()):
                if hub.policy.of(name) == "deny":
                    continue                      # not offered at all
                tag = {"allow": "", "ask": "[needs the person's approval] "}[hub.policy.of(name)]
                out.append(types.Tool(name=name, description=tag + (t.description or ""),
                                      inputSchema=t.inputSchema))
            return out

        @server.call_tool(validate_input=False)
        async def call_tool(name: str, arguments: dict) -> list[types.TextContent]:
            res = await hub.call(name, arguments or {})
            return [types.TextContent(type="text", text=json.dumps(res))]

        async with stdio_server() as (r, w):
            await server.run(r, w, server.create_initialization_options())
    finally:
        for t in refreshers:
            t.cancel()
        for o in owners:
            await o.stop()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--config", required=True)
    s.add_argument("--db", required=True, help="the provider broker's database (shared ledger and approver key)")
    a_ = sub.add_parser("approve")
    a_.add_argument("--db", required=True)
    a_.add_argument("approval")
    a_.add_argument("--deny", action="store_true")
    p = sub.add_parser("pending")
    p.add_argument("--db", required=True)
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stderr)
    if a.cmd == "serve":
        asyncio.run(serve(json.loads(Path(a.config).read_text()), a.db))
        return 0
    led = HubLedger(a.db)
    try:
        if a.cmd == "pending":
            print(json.dumps(led.pending(), indent=1))
        else:
            for item in led.pending():
                if item["approval"] == a.approval:
                    print("You are approving:", json.dumps({"tool": item["tool"], "arguments": item["arguments"]}))
            print(json.dumps(led.decide(a.approval, getpass.getpass("approver key: "), deny=a.deny)))
    except pb.Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
