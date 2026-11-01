"""NATS command channel for the QuantumOS hub (#297 slice 5, ADR-0027): the swarm drives this
person's QuantumOS VM by request and reply on KAX.qos.<name>.cmd, through the SAME policy the
hub applies to its own agent (allow / ask / deny, the ledger).

Config, under "nats" in the hub config:
  "nats": {"name": "nick",                       (subject is KAX.qos.nick.cmd)
           "url": "nats://127.0.0.1:4222",
           "creds_file": "~/.nats/qos.creds",    (optional; never logged)
           "allow": ["kannaka.kannaka_recall", "kannaka.kannaka_status"],
           "rate": {"per_identity_per_min": 20, "global_per_min": 120},
           "dry_run": false}

Request (JSON, at most 64 KiB):
  {"v": 1, "id": "any string up to 64 chars", "identity": "who is asking", "cmd": "kannaka.kannaka_recall",
   "args": {...}, "dry_run": false}
Reply (JSON):
  {"v": 1, "id": <the request id, or null if it could not be read>, "ok": true|false,
   "output": <the tool's result, or extra detail>, "error": null | {"code": ..., "message": ...},
   "dry_run": true|false}

Order of the gates, each of which refuses before anything runs:
  1. the envelope parses and is version 1;
  2. the per-identity and the global rate limits (a sliding minute);
  3. the allow-list: `cmd` must match one of the configured fnmatch patterns. The list is EMPTY
     by default, which allows nothing. It is a second, narrower gate than the hub policy: a tool
     the person lets their own agent use is not thereby open to the swarm;
  4. the hub policy: deny is refused; ask is stored for the person and replied as
     approval_required (the reply carries the approval id; the person approves locally, and
     hub_call_approved is NOT reachable over NATS); allow runs.
Dry run (per request, or forced for the whole channel by "dry_run": true) performs gates 1-4
and argument validation but runs nothing and stores no approval; the reply says what WOULD
happen: output {"would": "run" | "ask" | "deny", "policy": ...}.

Trust boundary, stated plainly: "identity" is a CLAIM made in the message. Plain NATS does not
authenticate it, so the per-identity limit stops a polite client, not an adversary; the global
limit and the NATS server's own subject permissions (publish rights on KAX.qos.<name>.cmd) are
what hold against one. Use accounts or creds that grant that subject only to peers you trust.
"""
from __future__ import annotations

import collections
import fnmatch
import json
import logging
import os
import re
import time

SCHEMA_VERSION = 1
MAX_REQUEST = 64 * 1024
MAX_IDENTITIES = 1024            # tracked identities; beyond this the oldest are forgotten
NAME = re.compile(r"^[a-z0-9][a-z0-9_-]{0,39}$")
TOKEN = re.compile(r"^[\x21-\x7e]{1,64}$")      # request ids and identities: printable ASCII, no spaces
NEVER_OVER_NATS = ("hub_call_approved",)       # spending an approval stays with the local agent
log = logging.getLogger("qos-hub.nats")


class ConfigError(ValueError):
    pass


def subject_for(name: str) -> str:
    if not isinstance(name, str) or not NAME.match(name):
        raise ConfigError("nats name must be 1-40 characters of a-z, 0-9, '-' or '_'")
    return f"KAX.qos.{name}.cmd"


class RateLimiter:
    """A sliding one-minute window per key, plus one for everything. A refusal does not count."""

    def __init__(self, per_identity: int, global_: int, clock=time.monotonic):
        if per_identity < 1 or global_ < 1:
            raise ConfigError("rate limits must be at least 1 per minute")
        self.per_identity, self.global_, self.clock = per_identity, global_, clock
        self.by_id: collections.OrderedDict[str, collections.deque] = collections.OrderedDict()
        self.all: collections.deque = collections.deque()

    @staticmethod
    def _trim(q: collections.deque, now: float) -> None:
        while q and now - q[0] >= 60:
            q.popleft()

    def check(self, identity: str) -> str | None:
        """None if allowed (and counted); else "identity" or "global", the limit that was hit."""
        now = self.clock()
        self._trim(self.all, now)
        q = self.by_id.get(identity)
        if q is None:
            q = self.by_id[identity] = collections.deque()
            while len(self.by_id) > MAX_IDENTITIES:
                self.by_id.popitem(last=False)
        else:
            self.by_id.move_to_end(identity)
        self._trim(q, now)
        if len(q) >= self.per_identity:
            return "identity"
        if len(self.all) >= self.global_:
            return "global"
        q.append(now)
        self.all.append(now)
        return None


def reply(rid, ok: bool, output=None, error: tuple[str, str] | None = None, dry_run: bool = False) -> dict:
    return {"v": SCHEMA_VERSION, "id": rid, "ok": ok, "output": output,
            "error": {"code": error[0], "message": error[1]} if error else None, "dry_run": dry_run}


class CommandChannel:
    """Answers requests on KAX.qos.<name>.cmd. `hub` is a qos_hub.Hub; `connect` is the NATS
    connect coroutine (nats.connect), injectable so tests need no server and no nats-py."""

    def __init__(self, hub, cfg: dict, connect=None, clock=time.monotonic):
        self.hub = hub
        self.subject = subject_for(cfg.get("name"))
        self.cfg = cfg
        allow = cfg.get("allow", [])
        if not isinstance(allow, list) or not all(isinstance(p, str) and p for p in allow):
            raise ConfigError("nats allow must be a list of command patterns")
        bad = [p for p in allow if any(fnmatch.fnmatchcase(n, p) for n in NEVER_OVER_NATS)]
        if bad:
            raise ConfigError(f"{bad} would expose {NEVER_OVER_NATS}, which is not available over NATS")
        self.allow = allow
        rate = cfg.get("rate") or {}
        self.limiter = RateLimiter(int(rate.get("per_identity_per_min", 20)), int(rate.get("global_per_min", 120)), clock)
        self.force_dry = bool(cfg.get("dry_run", False))
        self.connect = connect
        self.nc = None
        self.sub = None

    # ---- lifecycle ----
    async def start(self) -> None:
        connect = self.connect
        if connect is None:
            try:
                import nats                      # optional dependency: pip install quantumos-host-tools[nats]
            except ImportError:
                raise ConfigError("the nats channel needs nats-py: pip install 'quantumos-host-tools[nats]'") from None
            connect = nats.connect
        kw = {}
        if self.cfg.get("creds_file"):
            kw["user_credentials"] = os.path.expanduser(self.cfg["creds_file"])
        self.nc = await connect(self.cfg.get("url", "nats://127.0.0.1:4222"), **kw)
        self.sub = await self.nc.subscribe(self.subject, cb=self.on_message)
        self.hub.ledger.event("hub_nats_listening", subject=self.subject, dry_run=self.force_dry,
                              allow=len(self.allow))
        log.info("listening on %s", self.subject)

    async def stop(self) -> None:
        if self.nc is not None:
            try:
                await self.nc.drain()
            except Exception as e:  # noqa: BLE001
                log.warning("nats drain failed: %s", type(e).__name__)

    # ---- one request ----
    async def on_message(self, msg) -> None:
        if not getattr(msg, "reply", None):
            self.hub.ledger.event("hub_nats_cmd", outcome="no_reply_inbox")
            return                                # a command nobody can hear the result of is not run
        out = await self.handle(msg.data)
        try:
            await msg.respond(json.dumps(out, separators=(",", ":")).encode())
        except Exception as e:  # noqa: BLE001
            log.warning("reply failed: %s", type(e).__name__)

    def _ledger(self, identity, cmd, outcome, **extra) -> None:
        self.hub.ledger.event("hub_nats_cmd", identity=identity, cmd=cmd, outcome=outcome, **extra)

    async def handle(self, data: bytes) -> dict:
        """Bytes in, reply envelope out. Never raises."""
        try:
            return await self._handle(data)
        except Exception as e:  # noqa: BLE001 - a bad message must not take the subscriber down
            log.warning("handler error: %s", type(e).__name__)
            return reply(None, False, error=("internal_error", type(e).__name__))

    async def _handle(self, data: bytes) -> dict:
        if len(data) > MAX_REQUEST:
            return reply(None, False, error=("too_large", f"a request is at most {MAX_REQUEST} bytes"))
        try:
            req = json.loads(data)
        except ValueError:
            return reply(None, False, error=("bad_request", "the request is not JSON"))
        if not isinstance(req, dict):
            return reply(None, False, error=("bad_request", "the request must be a JSON object"))
        rid = req.get("id")
        if not isinstance(rid, str) or not TOKEN.match(rid):
            return reply(None, False, error=("bad_request", "id must be 1-64 printable characters"))
        if req.get("v") != SCHEMA_VERSION:
            return reply(rid, False, error=("unsupported_version", f"this channel speaks v{SCHEMA_VERSION}"))
        identity, cmd, args = req.get("identity"), req.get("cmd"), req.get("args", {})
        if not isinstance(identity, str) or not TOKEN.match(identity):
            return reply(rid, False, error=("bad_request", "identity must be 1-64 printable characters"))
        if not isinstance(cmd, str) or not cmd or len(cmd) > 128:
            return reply(rid, False, error=("bad_request", "cmd must be a tool name"))
        if not isinstance(args, dict):
            return reply(rid, False, error=("bad_request", "args must be an object"))
        want_dry = req.get("dry_run", False)
        if not isinstance(want_dry, bool):
            return reply(rid, False, error=("bad_request", "dry_run must be true or false"))
        dry = want_dry or self.force_dry

        hit = self.limiter.check(identity)
        if hit:
            self._ledger(identity, cmd, "rate_limited", limit=hit)
            return reply(rid, False, error=("rate_limited", f"the {hit} rate limit is reached; try again in a minute"), dry_run=dry)
        if cmd in NEVER_OVER_NATS or not any(fnmatch.fnmatchcase(cmd, p) for p in self.allow):
            self._ledger(identity, cmd, "not_allowed")
            return reply(rid, False, error=("not_allowed", f"{cmd} is not in this channel's allow-list"), dry_run=dry)
        if cmd not in self.hub.tools:
            self._ledger(identity, cmd, "no_such_tool")
            return reply(rid, False, error=("no_such_tool", f"no tool {cmd!r}"), dry_run=dry)

        policy = self.hub.policy.of(cmd)
        if dry:
            return self._plan(rid, identity, cmd, args, policy)

        self._ledger(identity, cmd, "forwarded", policy=policy)
        res = await self.hub.call(cmd, args)           # the hub's own gate: deny / ask / allow, and its ledger
        if res.get("ok"):
            return reply(rid, True, res.get("result"))
        code = res.get("error", "failed")
        detail = {"approval": res["approval"]} if "approval" in res else res.get("result")
        return reply(rid, False, detail, (code, str(res.get("message") or code)))

    def _plan(self, rid, identity, cmd, args, policy) -> dict:
        """What a real request would do, decided by the same policy and argument checks, with
        no call made and no approval stored."""
        would = {"allow": "run", "ask": "ask", "deny": "deny"}[policy]
        check = getattr(self.hub.down.get(self.hub.tools[cmd][0]), "validate", None)
        if would != "deny" and check is not None:
            try:
                check(self.hub.tools[cmd][1].name, args)
            except ValueError as e:
                self._ledger(identity, cmd, "dry_run", would="bad_arguments")
                return reply(rid, False, {"would": "refuse", "policy": policy},
                             ("bad_arguments", str(e)), dry_run=True)
        self._ledger(identity, cmd, "dry_run", would=would)
        return reply(rid, True, {"would": would, "policy": policy}, dry_run=True)
