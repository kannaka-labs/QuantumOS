"""NATS command channel for the QuantumOS hub (#297 slice 5, ADR-0027): the swarm drives this
person's QuantumOS VM by request and reply on KAX.qos.<name>.cmd, through the SAME policy the
hub applies to its own agent (allow / ask / deny, the ledger).

Config, under "nats" in the hub config:
  "nats": {"name": "nick",                       (subject is KAX.qos.nick.cmd)
           "url": "nats://127.0.0.1:4222",
           "creds_file": "~/.nats/qos.creds",    (optional; never logged)
           "allow": ["kannaka.kannaka_recall", "kannaka.kannaka_status"],
           "rate": {"per_identity_per_min": 20, "global_per_min": 120},
           "max_inflight": 4,                    (requests running at once; the rest wait, up to max_backlog)
           "max_backlog": 64,                    (waiting requests; beyond this a request is refused "busy")
           "max_age_s": 30,                      (a request whose "ts" is older than this is refused "expired")
           "dry_run": false}

Request (JSON, at most 64 KiB):
  {"v": 1, "id": "any string up to 64 chars", "identity": "who is asking", "cmd": "kannaka.kannaka_recall",
   "args": {...}, "dry_run": false, "ts": <unix seconds when sent, optional>}
Reply (JSON):
  {"v": 1, "id": <the request id, or null if it could not be read>, "ok": true|false,
   "output": <the tool's result, or extra detail>, "error": null | {"code": ..., "message": ...},
   "dry_run": true|false}

Order of the gates, each of which refuses before anything runs:
  1. the envelope parses and is version 1, and is not older than max_age_s (when it carries "ts");
  2. the per-identity and the global rate limits (a sliding minute);
  3. the allow-list: `cmd` must match one of the configured fnmatch patterns. The list is EMPTY
     by default, which allows nothing. It is a second, narrower gate than the hub policy: a tool
     the person lets their own agent use is not thereby open to the swarm;
  4. the hub policy: deny is refused; ask is stored for the person, marked with its origin
     (nats:<identity>) so `pending` shows where it came from, and replied as approval_required
     (the reply carries the approval id; the person approves locally; the person's OWN agent
     runs the stored call with hub_call_approved, which is NOT reachable over NATS; the peer
     never sees that result); allow runs.
Dry run (per request, or forced for the whole channel by "dry_run": true) performs gates 1-4
and argument validation but runs nothing and stores no approval; the reply says what WOULD
happen: output {"would": "run" | "ask" | "deny", "policy": ...}.

The ledger: every request that reaches the hub leaves a `hub_nats_cmd` row when it is forwarded
and another with how it ended (ok, denied, approval_required, timeout, server_error, failed...).
Refusals are bounded so a flood cannot grow the database: the first refusal of each kind in a
minute is written in full (with the claimed identity), the rest are counted, and one
`hub_nats_refusals` row with the count closes the window. Connection loss and recovery are
`hub_nats_down` / `hub_nats_up` / `hub_nats_closed` rows; the client reconnects without limit.

Trust boundary, stated plainly: "identity" is a CLAIM made in the message. Plain NATS does not
authenticate it, so the per-identity limit stops a polite client, not an adversary; the global
limit, the backlog cap and the NATS server's own subject permissions (publish rights on
KAX.qos.<name>.cmd) are what hold against one. Use accounts or creds that grant that subject
only to peers you trust.
"""
from __future__ import annotations

import asyncio
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
PENDING_MSGS = 64                # the NATS client's own queue for this subscription (slow consumer beyond)
PENDING_BYTES = 4 << 20
NAME = re.compile(r"^[a-z0-9][a-z0-9_-]{0,39}$")
TOKEN = re.compile(r"^[\x21-\x7e]{1,64}$")      # request ids and identities: printable ASCII, no spaces
NEVER_OVER_NATS = ("hub_call_approved", "hub_ledger", "hub_tools")   # the hub's own tools stay with the local agent
log = logging.getLogger("qos-hub.nats")


class ConfigError(ValueError):
    pass


def subject_for(name: str) -> str:
    if not isinstance(name, str) or not NAME.match(name):
        raise ConfigError("nats name must be 1-40 characters of a-z, 0-9, '-' or '_'")
    return f"KAX.qos.{name}.cmd"


def _positive_int(value, what: str, default: int) -> int:
    if value is None:
        return default
    try:
        n = int(value)
    except (TypeError, ValueError):
        raise ConfigError(f"nats {what} must be a whole number, not {value!r}") from None
    if n < 1:
        raise ConfigError(f"nats {what} must be at least 1")
    return n


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


class RefusalLog:
    """Bounds what a flood can write to the ledger. Per kind of refusal, the first in a window is
    written in full as a hub_nats_cmd row; later ones are counted (and their distinct identities,
    up to 64) and written as ONE hub_nats_refusals row when the window closes or on stop()."""

    WINDOW_S = 60

    def __init__(self, event, clock=time.monotonic):
        self.event, self.clock = event, clock
        self.windows: dict[str, dict] = {}

    def refuse(self, kind: str, identity: str | None = None, cmd: str | None = None, **extra) -> None:
        now = self.clock()
        w = self.windows.get(kind)
        if w is not None and now - w["start"] >= self.WINDOW_S:
            self._flush(kind, w)
            w = None
        if w is None:
            self.windows[kind] = {"start": now, "count": 1, "identities": {identity}}
            self.event("hub_nats_cmd", identity=identity, cmd=cmd, outcome=kind, **extra)
            return
        w["count"] += 1
        if len(w["identities"]) < 64:
            w["identities"].add(identity)

    def _flush(self, kind: str, w: dict) -> None:
        if w["count"] > 1:                    # the first one was written in full already
            self.event("hub_nats_refusals", outcome=kind, count=w["count"], identities=len(w["identities"]),
                       window_s=self.WINDOW_S)
        self.windows.pop(kind, None)

    def close(self) -> None:
        for kind, w in list(self.windows.items()):
            self._flush(kind, w)


def reply(rid, ok: bool, output=None, error: tuple[str, str] | None = None, dry_run: bool = False) -> dict:
    return {"v": SCHEMA_VERSION, "id": rid, "ok": ok, "output": output,
            "error": {"code": error[0], "message": error[1]} if error else None, "dry_run": dry_run}


def peek_id(data: bytes):
    """The request id if the bytes hold a well-formed one, else None; never raises."""
    try:
        req = json.loads(data[:MAX_REQUEST])
    except (ValueError, UnicodeDecodeError):
        return None
    rid = req.get("id") if isinstance(req, dict) else None
    return rid if isinstance(rid, str) and TOKEN.match(rid) else None


class CommandChannel:
    """Answers requests on KAX.qos.<name>.cmd. `hub` is a qos_hub.Hub; `connect` is the NATS
    connect coroutine (nats.connect), injectable so tests need no server and no nats-py."""

    def __init__(self, hub, cfg: dict, connect=None, clock=time.monotonic, wall=time.time):
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
        if not isinstance(rate, dict):
            raise ConfigError("nats rate must be an object")
        self.limiter = RateLimiter(_positive_int(rate.get("per_identity_per_min"), "rate.per_identity_per_min", 20),
                                   _positive_int(rate.get("global_per_min"), "rate.global_per_min", 120), clock)
        self.max_inflight = _positive_int(cfg.get("max_inflight"), "max_inflight", 4)
        self.max_backlog = _positive_int(cfg.get("max_backlog"), "max_backlog", 64)
        self.max_age_s = _positive_int(cfg.get("max_age_s"), "max_age_s", 30)
        self.force_dry = bool(cfg.get("dry_run", False))
        self.connect = connect
        self.wall = wall
        self.refusals = RefusalLog(self.hub.ledger.event, clock)
        self.nc = None
        self.sub = None
        self.sem: asyncio.Semaphore | None = None
        self.tasks: set[asyncio.Task] = set()

    # ---- lifecycle ----
    async def start(self) -> None:
        connect = self.connect
        if connect is None:
            try:
                import nats                      # optional dependency: pip install quantumos-host-tools[nats]
            except ImportError:
                raise ConfigError("the nats channel needs nats-py: pip install 'quantumos-host-tools[nats]'") from None
            connect = nats.connect
        kw = {"max_reconnect_attempts": -1,      # keep trying; a channel that gives up silently is a down VM
              "disconnected_cb": self._disconnected, "reconnected_cb": self._reconnected,
              "closed_cb": self._closed, "error_cb": self._error}
        if self.cfg.get("creds_file"):
            kw["user_credentials"] = os.path.expanduser(self.cfg["creds_file"])
        self.sem = asyncio.Semaphore(self.max_inflight)
        self.nc = await connect(self.cfg.get("url", "nats://127.0.0.1:4222"), **kw)
        self.sub = await self.nc.subscribe(self.subject, cb=self.on_message,
                                           pending_msgs_limit=PENDING_MSGS, pending_bytes_limit=PENDING_BYTES)
        self.hub.ledger.event("hub_nats_listening", subject=self.subject, dry_run=self.force_dry,
                              allow=len(self.allow), max_inflight=self.max_inflight)
        log.info("listening on %s", self.subject)

    async def stop(self) -> None:
        if self.tasks:                           # let the requests already running answer
            await asyncio.gather(*self.tasks, return_exceptions=True)
        if self.nc is not None:
            try:
                await self.nc.drain()
            except Exception as e:  # noqa: BLE001
                log.warning("nats drain failed: %s", type(e).__name__)
        self.refusals.close()

    async def _disconnected(self) -> None:
        self.hub.ledger.event("hub_nats_down", subject=self.subject)
        log.warning("nats disconnected; reconnecting")

    async def _reconnected(self) -> None:
        self.hub.ledger.event("hub_nats_up", subject=self.subject)
        log.info("nats reconnected")

    async def _closed(self) -> None:
        self.hub.ledger.event("hub_nats_closed", subject=self.subject)
        log.error("nats connection closed; the channel is no longer listening")

    async def _error(self, e) -> None:
        log.warning("nats error: %s", type(e).__name__)   # slow-consumer and friends: log only, never the ledger

    # ---- one request ----
    async def on_message(self, msg) -> None:
        if not getattr(msg, "reply", None):
            self.refusals.refuse("no_reply_inbox")
            return                                # a command nobody can hear the result of is not run
        if len(self.tasks) >= self.max_inflight + self.max_backlog:   # running + waiting; past that, refuse at once
            self.refusals.refuse("busy")
            await self._respond(msg, reply(peek_id(msg.data), False,
                                           error=("busy", f"{self.max_backlog} requests are already waiting")))
            return
        task = asyncio.ensure_future(self._serve(msg))
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)

    async def _serve(self, msg) -> None:
        async with self.sem:
            out = await self.handle(msg.data)
        await self._respond(msg, out)

    @staticmethod
    async def _respond(msg, out: dict) -> None:
        try:
            await msg.respond(json.dumps(out, separators=(",", ":")).encode())
        except Exception as e:  # noqa: BLE001
            log.warning("reply failed: %s", type(e).__name__)

    def _ledger(self, identity, cmd, outcome, **extra) -> None:
        self.hub.ledger.event("hub_nats_cmd", identity=identity, cmd=cmd, outcome=outcome, **extra)

    def _refuse(self, rid, identity, cmd, code: str, message: str, dry: bool = False, **extra) -> dict:
        self.refusals.refuse(code, identity, cmd, **extra)
        return reply(rid, False, error=(code, message), dry_run=dry)

    async def handle(self, data: bytes) -> dict:
        """Bytes in, reply envelope out. Never raises."""
        try:
            return await self._handle(data)
        except Exception as e:  # noqa: BLE001 - a bad message must not take the subscriber down
            log.warning("handler error: %s", type(e).__name__)
            self.refusals.refuse("internal_error", error=type(e).__name__)
            return reply(peek_id(data), False, error=("internal_error", type(e).__name__))

    async def _handle(self, data: bytes) -> dict:
        if len(data) > MAX_REQUEST:
            return self._refuse(None, None, None, "too_large", f"a request is at most {MAX_REQUEST} bytes")
        try:
            req = json.loads(data)
        except ValueError:
            return self._refuse(None, None, None, "bad_request", "the request is not JSON")
        if not isinstance(req, dict):
            return self._refuse(None, None, None, "bad_request", "the request must be a JSON object")
        rid = req.get("id")
        if not isinstance(rid, str) or not TOKEN.match(rid):
            return self._refuse(None, None, None, "bad_request", "id must be 1-64 printable characters")
        if req.get("v") != SCHEMA_VERSION:
            return self._refuse(rid, None, None, "unsupported_version", f"this channel speaks v{SCHEMA_VERSION}")
        identity, cmd, args = req.get("identity"), req.get("cmd"), req.get("args", {})
        if not isinstance(identity, str) or not TOKEN.match(identity):
            return self._refuse(rid, None, None, "bad_request", "identity must be 1-64 printable characters")
        if not isinstance(cmd, str) or not cmd or len(cmd) > 128:
            return self._refuse(rid, identity, None, "bad_request", "cmd must be a tool name")
        if not isinstance(args, dict):
            return self._refuse(rid, identity, cmd, "bad_request", "args must be an object")
        want_dry = req.get("dry_run", False)
        if not isinstance(want_dry, bool):
            return self._refuse(rid, identity, cmd, "bad_request", "dry_run must be true or false")
        dry = want_dry or self.force_dry
        ts = req.get("ts")
        if ts is not None:
            if isinstance(ts, bool) or not isinstance(ts, (int, float)):
                return self._refuse(rid, identity, cmd, "bad_request", "ts must be unix seconds")
            age = self.wall() - ts
            if age > self.max_age_s:
                return self._refuse(rid, identity, cmd, "expired",
                                    f"the request is {age:.0f} s old; this channel answers within {self.max_age_s} s",
                                    dry, age_s=int(age))

        hit = self.limiter.check(identity)
        if hit:
            return self._refuse(rid, identity, cmd, "rate_limited",
                                f"the {hit} rate limit is reached; try again in a minute", dry, limit=hit)
        if cmd in NEVER_OVER_NATS or not any(fnmatch.fnmatchcase(cmd, p) for p in self.allow):
            return self._refuse(rid, identity, cmd, "not_allowed", f"{cmd} is not in this channel's allow-list", dry)
        if cmd not in self.hub.tools:
            return self._refuse(rid, identity, cmd, "no_such_tool", f"no tool {cmd!r}", dry)

        policy = self.hub.policy.of(cmd)
        if dry:
            return self._plan(rid, identity, cmd, args, policy)

        self._ledger(identity, cmd, "forwarded", policy=policy)
        res = await self.hub.call(cmd, args, origin=f"nats:{identity}")   # the hub's own gate: deny / ask / allow
        if res.get("ok"):
            self._ledger(identity, cmd, "ok")
            return reply(rid, True, res.get("result"))
        code = str(res.get("error") or "failed")
        extra = {"approval": res["approval"]} if "approval" in res else {}
        self._ledger(identity, cmd, code, **extra)              # how the forwarded request ended
        detail = extra or res.get("result")
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
