#!/usr/bin/env python3
"""QuantumOS provider broker: quote, lease and tear down real resources for an agent,
inside a budget the agent cannot raise (ADR-0026, #284).

An agent (Claude over MCP, #287, or a QuantumOS guest later) asks for a resource: a
bucket, a VM, a GPU, a QPU job. The broker answers with a quote first. Provisioning
then needs either room in the envelope the PERSON set (an amount per UTC day, a cap
per action, the resource classes allowed, the longest lease), or the person's explicit
approval. Approving and changing the envelope need the approver key, which only the
person holds: it is shown once, when the envelope is first set, and stored only as an
scrypt hash. Nothing the agent can send over the socket approves anything.

Every resource lives on a lease. The lease is written to the ledger BEFORE the provider
is called, and the resource is tagged at the provider with the lease's tag
(qos-<broker id>-<lease id>), so a crash mid-provision is found and torn down at the next
start. When a lease expires, the reaper tears its resource down. Teardown and recovery
only ever touch resources carrying this broker's tag for a lease in this ledger: never
anything else in the account.

Real spending is off by default: without --allow-spend, only quotes whose maximum cost is
exactly 0 can be provisioned. Every quote says which of its figures are estimates.
Billed costs, when the provider reports them, sit next to the estimate in the ledger;
they are never merged.

Trust boundary, stated plainly: these rules hold against an agent that reaches the broker
only through its socket. An agent with a shell as the user that owns the ledger and the
provider credentials can bypass all of them. Run the broker as its own user, and give the
agent's user only the socket (group).

Plugins (providers) implement: name, offerings(), quote(kind, spec, duration_s) -> Quote,
provision(kind, spec, tag) -> resource id, status(rid), teardown(rid) (idempotent),
billed(rid) -> float | None, find(tag) -> [rid].

  provider_broker.py serve    --db broker.db --socket broker.sock [--allow-spend]
  provider_broker.py envelope --db broker.db --per-day 5 --per-action 2 --classes storage,compute --max-lease 24h
  provider_broker.py approve  --db broker.db <approval id>
  provider_broker.py ledger   --db broker.db
  provider_broker.py setup oci          (the least-privilege steps for a provider)

Socket protocol: one JSON object per line, one JSON answer per line. Agent ops only:
  {"op": "providers"} | {"op": "quote", "provider", "kind", "spec", "duration_s"}
  {"op": "provision", "quote": id[, "approval": id]} | {"op": "status", "lease": id}
  {"op": "teardown", "lease": id} | {"op": "budget"} | {"op": "ledger"[, "limit": n]}
"""
from __future__ import annotations

import argparse
import asyncio
import datetime
import getpass
import hashlib
import hmac
import json
import logging
import os
import re
import secrets
import sqlite3
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

log = logging.getLogger("provider-broker")

OWNER = "local"          # phase 1 is self-hosted: one person per broker (ADR-0026 §5)
QUOTE_TTL_S = 600        # a quote must be used within this long
MAX_LINE = 16384
ID = re.compile(r"^[a-z]{1,2}[0-9a-f]{10}$")
ACTIVE = ("provisioning", "active", "tearing_down")


class Refused(Exception):
    """A request the policy refuses. str() is shown to the agent; .code is machine-readable."""

    def __init__(self, code: str, message: str, **extra):
        super().__init__(message)
        self.code, self.extra = code, extra


@dataclass
class Quote:
    """What one resource would cost. `max_cost_usd` is the most the whole lease can cost
    at the quoted price; `exact` says whether the price is the provider's published one."""
    resource_class: str            # storage | compute | gpu | qpu | service
    unit: str                      # "hour", "job", "GB-month", ...
    unit_price_usd: float
    max_cost_usd: float
    exact: bool
    free_tier: str | None = None   # the provider's own wording, if it reports one
    notes: str = ""


@dataclass
class Envelope:
    per_day_usd: float
    per_action_usd: float
    classes: list[str] = field(default_factory=list)
    max_lease_s: int = 86400


def utc_day(t: float) -> str:
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime("%Y-%m-%d")


def new_id(prefix: str) -> str:
    return prefix + secrets.token_hex(5)


def hash_key(key: str, salt: bytes) -> str:
    return hashlib.scrypt(key.encode(), salt=salt, n=2 ** 14, r=8, p=1, dklen=32).hex()


# ---- ledger ------------------------------------------------------------------

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS quotes (id TEXT PRIMARY KEY, owner TEXT, provider TEXT, kind TEXT,
    spec TEXT, duration_s INTEGER, quote TEXT, created REAL, used INTEGER DEFAULT 0);
CREATE TABLE IF NOT EXISTS approvals (id TEXT PRIMARY KEY, owner TEXT, quote_id TEXT,
    state TEXT, created REAL, decided REAL);
CREATE TABLE IF NOT EXISTS leases (id TEXT PRIMARY KEY, owner TEXT, quote_id TEXT, provider TEXT,
    kind TEXT, resource_id TEXT, tag TEXT, state TEXT, created REAL, expires REAL, ended REAL,
    end_reason TEXT, est_cost REAL, billed REAL, day TEXT);
CREATE TABLE IF NOT EXISTS events (seq INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, owner TEXT,
    lease_id TEXT, kind TEXT, detail TEXT);
"""


class Ledger:
    def __init__(self, path: str):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.commit()
        if path != ":memory:":
            os.chmod(path, 0o600)

    def close(self) -> None:
        self.db.close()

    def __del__(self):
        try:
            self.db.close()
        except Exception:  # noqa: BLE001 - interpreter shutdown
            pass

    def q(self, sql: str, *args) -> list[sqlite3.Row]:
        cur = self.db.execute(sql, args)
        rows = cur.fetchall()
        self.db.commit()
        return rows

    def meta(self, k: str, default: str | None = None) -> str | None:
        r = self.q("SELECT v FROM meta WHERE k=?", k)
        return r[0]["v"] if r else default

    def set_meta(self, k: str, v: str) -> None:
        self.q("INSERT INTO meta(k, v) VALUES(?, ?) ON CONFLICT(k) DO UPDATE SET v=excluded.v", k, v)

    def event(self, ts: float, owner: str, lease_id: str | None, what: str, **detail) -> None:
        self.q("INSERT INTO events(ts, owner, lease_id, kind, detail) VALUES(?,?,?,?,?)",
               ts, owner, lease_id, what, json.dumps(detail, sort_keys=True))


# ---- the broker --------------------------------------------------------------

class Broker:
    def __init__(self, ledger: Ledger, plugins: dict, allow_spend: bool = False, clock=time.time):
        self.l, self.plugins, self.allow_spend, self.clock = ledger, plugins, allow_spend, clock
        self.lock = threading.Lock()     # one request at a time against the ledger and providers
        self.broker_id = ledger.meta("broker_id")
        if self.broker_id is None:
            self.broker_id = secrets.token_hex(4)
            ledger.set_meta("broker_id", self.broker_id)

    # ---- the person's side (approver key) ----
    def envelope(self, owner: str = OWNER) -> Envelope | None:
        raw = self.l.meta(f"envelope:{owner}")
        return Envelope(**json.loads(raw)) if raw else None

    def _check_key(self, owner: str, key: str | None) -> None:
        salt, want = self.l.meta(f"approver_salt:{owner}"), self.l.meta(f"approver_hash:{owner}")
        if salt is None:
            raise Refused("no_approver", "no approver key is set")
        if not key or not hmac.compare_digest(hash_key(key, bytes.fromhex(salt)), want):
            raise Refused("bad_key", "the approver key does not match")

    def set_envelope(self, env: Envelope, key: str | None = None, owner: str = OWNER) -> str | None:
        """Set the envelope. The first time, a new approver key is made and returned (show it
        to the person once). After that, changing the envelope needs the current key."""
        with self.lock:
            fresh = None
            if self.l.meta(f"approver_hash:{owner}") is None:
                fresh = secrets.token_urlsafe(16)
                salt = secrets.token_bytes(16)
                self.l.set_meta(f"approver_salt:{owner}", salt.hex())
                self.l.set_meta(f"approver_hash:{owner}", hash_key(fresh, salt))
            else:
                self._check_key(owner, key)
            if env.per_day_usd < 0 or env.per_action_usd < 0 or env.max_lease_s <= 0:
                raise Refused("bad_envelope", "amounts must be >= 0 and the lease positive")
            self.l.set_meta(f"envelope:{owner}", json.dumps(asdict(env)))
            self.l.event(self.clock(), owner, None, "envelope", **asdict(env))
            return fresh

    def approve(self, approval_id: str, key: str, owner: str = OWNER, deny: bool = False) -> dict:
        with self.lock:
            self._check_key(owner, key)
            r = self.l.q("SELECT * FROM approvals WHERE id=? AND owner=?", approval_id, owner)
            if not r or r[0]["state"] != "pending":
                raise Refused("no_such_approval", "no pending approval with that id")
            state = "denied" if deny else "approved"
            self.l.q("UPDATE approvals SET state=?, decided=? WHERE id=?", state, self.clock(), approval_id)
            self.l.event(self.clock(), owner, None, "approval_" + state, approval=approval_id,
                         quote=r[0]["quote_id"])
            return {"approval": approval_id, "state": state}

    # ---- the agent's side ----
    def providers(self) -> dict:
        return {"providers": {n: p.offerings() for n, p in sorted(self.plugins.items())},
                "spending": "on" if self.allow_spend else "off (only free quotes can be provisioned)"}

    def quote(self, provider: str, kind: str, spec: dict, duration_s: int, owner: str = OWNER) -> dict:
        with self.lock:
            p = self.plugins.get(provider)
            if p is None:
                raise Refused("no_provider", f"no provider {provider!r}")
            if not isinstance(spec, dict) or not isinstance(duration_s, int) or duration_s <= 0:
                raise Refused("bad_request", "spec must be an object and duration_s a positive integer")
            qt: Quote = p.quote(kind, spec, duration_s)
            qid = new_id("q")
            now = self.clock()
            self.l.q("INSERT INTO quotes(id, owner, provider, kind, spec, duration_s, quote, created) "
                     "VALUES(?,?,?,?,?,?,?,?)", qid, owner, provider, kind, json.dumps(spec),
                     duration_s, json.dumps(asdict(qt)), now)
            self.l.event(now, owner, None, "quote", quote=qid, provider=provider, kind=kind,
                         max_cost_usd=qt.max_cost_usd, exact=qt.exact)
            return {"quote": qid, "provider": provider, "kind": kind, "duration_s": duration_s,
                    **asdict(qt), "valid_s": QUOTE_TTL_S,
                    "within_envelope": self._fits(owner, qt, duration_s) is None}

    def _committed(self, owner: str, day: str) -> float:
        r = self.l.q("SELECT COALESCE(SUM(est_cost), 0) AS s FROM leases WHERE owner=? AND day=? "
                     "AND end_reason IS NOT 'failed'", owner, day)
        return float(r[0]["s"])

    def _fits(self, owner: str, qt: Quote, duration_s: int) -> str | None:
        """None if the quote fits the envelope, else why it needs the person's approval."""
        env = self.envelope(owner)
        if env is None:
            return "no envelope is set"
        if qt.resource_class not in env.classes:
            return f"resource class {qt.resource_class!r} is not in the envelope"
        if duration_s > env.max_lease_s:
            return f"lease of {duration_s} s is longer than the envelope's {env.max_lease_s} s"
        if qt.max_cost_usd > env.per_action_usd:
            return f"${qt.max_cost_usd:.2f} is over the per-action cap of ${env.per_action_usd:.2f}"
        left = env.per_day_usd - self._committed(owner, utc_day(self.clock()))
        if qt.max_cost_usd > left:
            return f"${qt.max_cost_usd:.2f} is over what is left today (${max(left, 0):.2f})"
        return None

    def provision(self, quote_id: str, approval_id: str | None = None, owner: str = OWNER) -> dict:
        with self.lock:
            r = self.l.q("SELECT * FROM quotes WHERE id=? AND owner=?", quote_id, owner)
            if not r:
                raise Refused("no_such_quote", "no quote with that id")
            row = r[0]
            if row["used"]:
                raise Refused("quote_used", "that quote was already used")
            now = self.clock()
            if now - row["created"] > QUOTE_TTL_S:
                raise Refused("quote_expired", "that quote expired; ask for a new one")
            qt = Quote(**json.loads(row["quote"]))
            if qt.max_cost_usd > 0 and not self.allow_spend:
                raise Refused("spending_off", "real spending is off on this broker: only free "
                              "resources can be provisioned (the person can start it with --allow-spend)")
            why = self._fits(owner, qt, row["duration_s"])
            if why is not None:
                if approval_id is None:
                    aid = new_id("a")
                    self.l.q("INSERT INTO approvals(id, owner, quote_id, state, created) VALUES(?,?,?,?,?)",
                             aid, owner, quote_id, "pending", now)
                    self.l.event(now, owner, None, "approval_requested", approval=aid, quote=quote_id,
                                 reason=why)
                    raise Refused("approval_required", f"needs the person's approval: {why}",
                                  approval=aid,
                                  how=f"ask your human to run: provider_broker.py approve {aid}")
                a = self.l.q("SELECT * FROM approvals WHERE id=? AND owner=? AND quote_id=?",
                             approval_id, owner, quote_id)
                if not a or a[0]["state"] != "approved":
                    state = a[0]["state"] if a else "unknown"
                    raise Refused("not_approved", f"approval {approval_id} is {state}")
                self.l.q("UPDATE approvals SET state='used' WHERE id=?", approval_id)
            lid = new_id("l")
            tag = f"qos-{self.broker_id}-{lid}"
            # The lease exists before the provider is called: a crash from here on is
            # recovered by its tag at the next start.
            self.l.q("UPDATE quotes SET used=1 WHERE id=?", quote_id)
            self.l.q("INSERT INTO leases(id, owner, quote_id, provider, kind, tag, state, created, expires, "
                     "est_cost, day) VALUES(?,?,?,?,?,?,?,?,?,?,?)", lid, owner, quote_id, row["provider"],
                     row["kind"], tag, "provisioning", now, now + row["duration_s"], qt.max_cost_usd,
                     utc_day(now))
            p = self.plugins[row["provider"]]
            try:
                rid = p.provision(row["kind"], json.loads(row["spec"]), tag)
            except Exception as e:  # noqa: BLE001 - recorded, then cleaned up by tag
                self._end(lid, "failed", note=f"{type(e).__name__}: {e}")
                self._sweep_tag(p, tag)
                raise Refused("provision_failed", f"the provider refused or failed: {e}", lease=lid) from None
            self.l.q("UPDATE leases SET resource_id=?, state='active' WHERE id=?", rid, lid)
            self.l.event(now, owner, lid, "provisioned", provider=row["provider"], kind=row["kind"],
                         resource=rid, est_cost=qt.max_cost_usd, expires=now + row["duration_s"])
            return {"lease": lid, "resource": rid, "provider": row["provider"], "kind": row["kind"],
                    "expires_in_s": row["duration_s"], "est_cost_usd": qt.max_cost_usd, "exact": qt.exact}

    def _lease(self, lease_id: str, owner: str) -> sqlite3.Row:
        r = self.l.q("SELECT * FROM leases WHERE id=? AND owner=?", lease_id, owner)
        if not r:
            raise Refused("no_such_lease", "no lease with that id")
        return r[0]

    def status(self, lease_id: str, owner: str = OWNER) -> dict:
        with self.lock:
            le = self._lease(lease_id, owner)
            out = {k: le[k] for k in ("id", "provider", "kind", "resource_id", "state", "end_reason",
                                      "est_cost", "billed")}
            out["expires_in_s"] = max(0, int(le["expires"] - self.clock())) if le["state"] == "active" else 0
            if le["state"] == "active" and le["resource_id"]:
                out["provider_status"] = self.plugins[le["provider"]].status(le["resource_id"])
            return out

    def teardown(self, lease_id: str, owner: str = OWNER, reason: str = "teardown") -> dict:
        with self.lock:
            return self._teardown(self._lease(lease_id, owner), reason)

    def _teardown(self, le: sqlite3.Row, reason: str) -> dict:
        if le["state"] == "ended":
            return {"lease": le["id"], "state": "ended", "end_reason": le["end_reason"]}
        p = self.plugins[le["provider"]]
        self.l.q("UPDATE leases SET state='tearing_down' WHERE id=?", le["id"])
        if le["resource_id"]:
            p.teardown(le["resource_id"])
        self._sweep_tag(p, le["tag"])
        billed = p.billed(le["resource_id"]) if le["resource_id"] else None
        self._end(le["id"], reason, billed=billed)
        return {"lease": le["id"], "state": "ended", "end_reason": reason, "billed_usd": billed,
                "est_cost_usd": le["est_cost"]}

    def _sweep_tag(self, p, tag: str) -> None:
        """Tear down whatever the provider holds under this lease's tag, and nothing else."""
        assert tag.startswith(f"qos-{self.broker_id}-")
        for rid in p.find(tag):
            p.teardown(rid)

    def _end(self, lid: str, reason: str, billed: float | None = None, note: str = "") -> None:
        now = self.clock()
        self.l.q("UPDATE leases SET state='ended', ended=?, end_reason=?, billed=? WHERE id=?",
                 now, reason, billed, lid)
        r = self.l.q("SELECT owner FROM leases WHERE id=?", lid)
        self.l.event(now, r[0]["owner"], lid, "ended", reason=reason, billed=billed, note=note)

    def budget(self, owner: str = OWNER) -> dict:
        with self.lock:
            env = self.envelope(owner)
            used = self._committed(owner, utc_day(self.clock()))
            active = self.l.q("SELECT COUNT(*) AS n FROM leases WHERE owner=? AND state='active'", owner)[0]["n"]
            return {"envelope": asdict(env) if env else None, "committed_today_usd": used,
                    "left_today_usd": (max(env.per_day_usd - used, 0) if env else 0.0),
                    "active_leases": active, "spending": "on" if self.allow_spend else "off"}

    def ledger(self, owner: str = OWNER, limit: int = 50) -> dict:
        rows = self.l.q("SELECT ts, lease_id, kind, detail FROM events WHERE owner=? ORDER BY seq DESC LIMIT ?",
                        owner, max(1, min(int(limit), 500)))
        return {"events": [{"ts": r["ts"], "lease": r["lease_id"], "event": r["kind"],
                            **json.loads(r["detail"])} for r in rows]}

    # ---- housekeeping ----
    def reap(self) -> int:
        """Tear down every lease past its expiry. Returns how many."""
        with self.lock:
            due = self.l.q("SELECT * FROM leases WHERE state='active' AND expires<=?", self.clock())
            for le in due:
                try:
                    self._teardown(le, "expired")
                except Exception as e:  # noqa: BLE001 - retried next pass
                    log.warning("reap %s failed: %s: %s", le["id"], type(e).__name__, e)
            return len(due)

    def recover(self) -> int:
        """At start: leases a crash left mid-provision or mid-teardown are torn down by tag."""
        with self.lock:
            stuck = self.l.q("SELECT * FROM leases WHERE state IN ('provisioning', 'tearing_down')")
            for le in stuck:
                p = self.plugins.get(le["provider"])
                if p is None:
                    continue
                self._sweep_tag(p, le["tag"])
                self._end(le["id"], "failed" if le["state"] == "provisioning" else "teardown",
                          note="recovered at start")
            return len(stuck)

    # ---- socket ----
    AGENT_OPS = ("providers", "quote", "provision", "status", "teardown", "budget", "ledger")

    def handle(self, req: dict) -> dict:
        op = req.get("op")
        try:
            if op not in self.AGENT_OPS:
                raise Refused("bad_op", f"unknown op {op!r}; agent ops are {', '.join(self.AGENT_OPS)}")
            if op == "providers":
                return {"ok": True, **self.providers()}
            if op == "quote":
                return {"ok": True, **self.quote(str(req.get("provider")), str(req.get("kind")),
                                                 req.get("spec") or {}, req.get("duration_s"))}
            if op == "provision":
                aid = req.get("approval")
                return {"ok": True, **self.provision(str(req.get("quote")), str(aid) if aid else None)}
            if op in ("status", "teardown"):
                return {"ok": True, **getattr(self, op)(str(req.get("lease")))}
            if op == "budget":
                return {"ok": True, **self.budget()}
            return {"ok": True, **self.ledger(limit=req.get("limit") or 50)}
        except Refused as e:
            return {"ok": False, "error": e.code, "message": str(e), **e.extra}
        except (TypeError, ValueError) as e:
            return {"ok": False, "error": "bad_request", "message": str(e)}
        except Exception as e:  # noqa: BLE001 - a provider SDK error must not drop the connection
            log.warning("op %s failed: %s: %s", op, type(e).__name__, e)
            return {"ok": False, "error": "provider_error", "message": f"{type(e).__name__}: {e}"}

    async def serve_conn(self, r: asyncio.StreamReader, w: asyncio.StreamWriter) -> None:
        try:
            while line := await r.readline():
                if len(line) > MAX_LINE:
                    ans = {"ok": False, "error": "bad_request", "message": "request too long"}
                else:
                    try:
                        req = json.loads(line)
                        ans = await asyncio.get_running_loop().run_in_executor(None, self.handle, req) \
                            if isinstance(req, dict) else {"ok": False, "error": "bad_request",
                                                           "message": "one JSON object per line"}
                    except ValueError:
                        ans = {"ok": False, "error": "bad_request", "message": "not JSON"}
                w.write((json.dumps(ans) + "\n").encode())
                await w.drain()
        except (ConnectionError, asyncio.LimitOverrunError, ValueError):
            pass
        finally:
            w.close()


# ---- plugins -----------------------------------------------------------------

def _sibling(name: str):
    """A plugin module: relative when installed (quantumos_agent.broker), plain from a checkout."""
    import importlib
    return importlib.import_module(f".{name}", __package__) if __package__ else importlib.import_module(name)


def load_plugins(names: list[str], a: argparse.Namespace | None = None) -> dict:
    out = {}
    for n in names:
        if n == "mock":
            out["mock"] = _sibling("plugin_mock").MockProvider()
        elif n == "oci":
            OciProvider = _sibling("plugin_oci").OciProvider
            if a is None or not a.oci_config or not a.oci_compartment:
                raise SystemExit("the oci provider needs --oci-config and --oci-compartment")
            out["oci"] = OciProvider(a.oci_config, a.oci_compartment, a.oci_profile)
        elif n == "qbraid":
            out["qbraid"] = _sibling("plugin_qbraid").QbraidProvider(a.qpu_socket if a is not None else "/run/qos-qpu/broker.sock")
        else:
            raise SystemExit(f"unknown provider plugin {n!r}")
    return out


def parse_duration(s: str) -> int:
    m = re.fullmatch(r"(\d+)([smhd]?)", s.strip())
    if not m:
        raise argparse.ArgumentTypeError(f"bad duration {s!r} (e.g. 90s, 30m, 24h, 7d)")
    return int(m.group(1)) * {"": 1, "s": 1, "m": 60, "h": 3600, "d": 86400}[m.group(2)]


async def amain(a: argparse.Namespace, broker: Broker | None = None, stop: asyncio.Future | None = None) -> int:
    broker = broker or Broker(Ledger(a.db), load_plugins(a.providers.split(","), a), a.allow_spend)
    n = broker.recover()
    if n:
        log.info("recovered %d interrupted leases", n)
    sock = Path(a.socket)
    sock.unlink(missing_ok=True)
    server = await asyncio.start_unix_server(broker.serve_conn, str(sock), limit=MAX_LINE * 2)
    os.chmod(sock, 0o660)
    log.info("listening %s providers=%s spending=%s broker=%s", sock, ",".join(broker.plugins),
             "on" if broker.allow_spend else "off", broker.broker_id)

    async def reaper():
        while True:
            await asyncio.sleep(a.reap_every)
            n = await asyncio.get_running_loop().run_in_executor(None, broker.reap)
            if n:
                log.info("reaped %d expired leases", n)

    task = asyncio.create_task(reaper())
    stop = stop or asyncio.get_running_loop().create_future()
    try:
        async with server:
            await stop
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--db", required=True)
    s.add_argument("--socket", required=True)
    s.add_argument("--providers", default="mock", help="comma-separated: mock, oci, qbraid")
    s.add_argument("--qpu-socket", default="/run/qos-qpu/broker.sock",
                   help="the host quantum broker's socket (qbraid provider)")
    s.add_argument("--oci-config", help="OCI config file (oci provider)")
    s.add_argument("--oci-profile", default="DEFAULT")
    s.add_argument("--oci-compartment", help="OCID of the compartment the broker may use")
    s.add_argument("--allow-spend", action="store_true", help="allow quotes that cost money (off: free only)")
    s.add_argument("--reap-every", type=float, default=30)
    e = sub.add_parser("envelope", help="set the budget envelope (the person, not the agent)")
    e.add_argument("--db", required=True)
    e.add_argument("--per-day", type=float, required=True, help="USD per UTC day")
    e.add_argument("--per-action", type=float, required=True, help="USD per provision")
    e.add_argument("--classes", required=True, help="e.g. storage,compute")
    e.add_argument("--max-lease", type=parse_duration, default=86400)
    p = sub.add_parser("approve", help="approve (or --deny) a pending request")
    p.add_argument("--db", required=True)
    p.add_argument("approval")
    p.add_argument("--deny", action="store_true")
    st = sub.add_parser("setup", help="print a provider's least-privilege setup steps")
    st.add_argument("provider", choices=("oci",))
    g = sub.add_parser("ledger")
    g.add_argument("--db", required=True)
    g.add_argument("--limit", type=int, default=50)
    return ap


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    if a.cmd == "serve":
        return asyncio.run(amain(a))
    if a.cmd == "setup":
        print(_sibling("plugin_oci").SETUP, end="")
        return 0
    b = Broker(Ledger(a.db), {})
    try:
        if a.cmd == "envelope":
            env = Envelope(a.per_day, a.per_action, [c for c in a.classes.split(",") if c], a.max_lease)
            first = b.l.meta(f"approver_hash:{OWNER}") is None
            fresh = b.set_envelope(env, None if first else getpass.getpass("approver key: "))
            if fresh:
                print("Approver key (shown once; keep it where your agent cannot read it):")
                print(fresh)
            print("envelope set:", json.dumps(asdict(env)))
        elif a.cmd == "approve":
            print(json.dumps(b.approve(a.approval, getpass.getpass("approver key: "), deny=a.deny)))
        else:
            print(json.dumps(b.ledger(limit=a.limit), indent=1))
    except Refused as err:
        print(f"refused: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    # Plugins import this module by name; make that the same module object as __main__,
    # so a plugin's Refused is the class the broker catches.
    sys.modules.setdefault("provider_broker", sys.modules[__name__])
    sys.exit(main())
