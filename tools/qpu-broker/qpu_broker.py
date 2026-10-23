#!/usr/bin/env python3
"""QuantumOS host quantum broker: runs guest-submitted circuits on qBraid (#262).

The guest never holds a credential and has no network. On the public console the
launcher (tools/qos-console) relays a guest's request from COM1 to this broker over a
Unix socket and adds the session id and a salted client hash, which the guest cannot
forge. The broker is the only process that holds the qBraid API key (QBRAID_API_KEY,
from a root-only EnvironmentFile), and every money rule lives here, not in the UI:

  * Circuits are validated before anything is sent: wire format (scripts/qpu_circuit.py),
    qubit, op and shot caps, and a gate set we can translate exactly.
  * The simulator is the default. A simulator is picked at run time from qBraid's device
    list: online, enough qubits, and free (every price field 0).
  * A real QPU job needs: real QPUs switched on (--real), a daily credit budget across all
    visitors (--budget-per-day), a per-job cap (--max-job-credits), at most one real job
    per session and per client per day. The device is the cheapest suitable one that is
    ONLINE now, by an estimate made from the SDK's live pricing before submit. A QPU whose
    price is missing or all zero is skipped (its real cost is billed elsewhere, so we
    cannot bound it). The estimate is logged before submit and the billed cost after.
  * Real jobs are asynchronous: submit returns "queued on <device>, job <id>" at once and
    the result is fetched later (`qpu job <id>`), from any session.

Every answer carries provenance: kind (simulator or QPU), device id, broker job id,
provider job id, shots, timestamps and cost.

Protocol: one JSON object per line on the Unix socket, one JSON answer per line.
  {"op": "info", "sid": ..., "client": ...}
  {"op": "submit", "sid": ..., "client": ..., "mode": "sim"|"qpu", "shots": N, "circuit": "<hex>"}
  {"op": "job", "id": "<broker job id>"}

  qpu_broker.py --socket /run/qos-qpu/broker.sock --db /var/lib/qos-qpu/broker.db [--real]
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import re
import secrets
import sqlite3
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
for p in (HERE, HERE.parent.parent / "scripts"):
    if (p / "qpu_circuit.py").exists():
        sys.path.insert(0, str(p))
        break
import qpu_circuit as qc  # noqa: E402

log = logging.getLogger("qpu-broker")

MAX_LINE = 4096
JOB_ID = re.compile(r"^q[0-9a-f]{10}$")
HEX = re.compile(r"^[0-9a-f]+$")
TERMINAL = {"done", "failed", "cancelled"}


class Refused(Exception):
    """A request the policy refuses; the message is shown to the visitor."""


# ---- circuits ----------------------------------------------------------------

def _mcz(n: int) -> list[str]:
    """A Z controlled on all of q[0..n-1] (phase -1 on |1..1>), in qelib1 gates."""
    if n == 1:
        return ["z q[0];"]
    if n == 2:
        return ["cz q[0],q[1];"]
    if n == 3:
        return ["h q[2];", "ccx q[0],q[1],q[2];", "h q[2];"]
    raise Refused("Grover oracle/diffusion is translated for up to 3 qubits")


def to_qasm2(circuit: bytes) -> tuple[int, str]:
    """The wire-format circuit as OpenQASM 2 with every qubit measured into c[i].
    ORACLE flips the sign of one basis state (qubit 0 = least significant bit), and
    DIFFUSION is H X MCZ X H, which equals PennyLane's GroverOperator up to a global
    phase, so measured counts are the same as the exact engine's probabilities."""
    n, _probe, ops = qc.parse(circuit)
    body: list[str] = []
    for opcode, a, b in ops:
        if opcode in (qc.QC_OP_CNOT, qc.QC_OP_CZ):
            used = (a, b)
        elif opcode in (qc.QC_OP_H, qc.QC_OP_X, qc.QC_OP_Z, qc.QC_OP_S):
            used = (a,)
        else:
            used = ()
        for qb in used:
            if qb >= n:
                raise Refused(f"gate on qubit {qb} outside a {n}-qubit register")
        if opcode == qc.QC_OP_H:
            body.append(f"h q[{a}];")
        elif opcode == qc.QC_OP_X:
            body.append(f"x q[{a}];")
        elif opcode == qc.QC_OP_Z:
            body.append(f"z q[{a}];")
        elif opcode == qc.QC_OP_S:
            body.append(f"s q[{a}];")
        elif opcode in (qc.QC_OP_CNOT, qc.QC_OP_CZ):
            if a == b:
                raise Refused("two-qubit gate with the same control and target")
            body.append(f"{'cx' if opcode == qc.QC_OP_CNOT else 'cz'} q[{a}],q[{b}];")
        elif opcode == qc.QC_OP_ORACLE:
            target = a | (b << 8)
            if target >= (1 << n):
                raise Refused("oracle target outside the register")
            flips = [f"x q[{i}];" for i in range(n) if not (target >> i) & 1]
            body += flips + _mcz(n) + flips
        elif opcode == qc.QC_OP_DIFFUSION:
            hs = [f"h q[{i}];" for i in range(n)]
            xs = [f"x q[{i}];" for i in range(n)]
            body += hs + xs + _mcz(n) + xs + hs
        else:
            raise Refused(f"unknown opcode {opcode}")
    text = ['OPENQASM 2.0;', 'include "qelib1.inc";', f"qreg q[{n}];", f"creg c[{n}];"]
    text += body + ["measure q -> c;"]
    return n, "\n".join(text) + "\n"


# ---- policy and records ------------------------------------------------------

@dataclass
class Policy:
    real: bool = False                 # real QPUs switched on at all
    budget_per_day: float = 10.0       # credits per UTC day, all visitors together
    max_job_credits: float = 5.0       # one job's estimate may not exceed this
    real_per_session: int = 1
    real_per_client_per_day: int = 1
    sim_per_session: int = 10
    sim_per_day: int = 500
    sim_max_qubits: int = 8
    real_max_qubits: int = 5
    max_ops: int = 40
    sim_max_shots: int = 1000
    sim_default_shots: int = 100
    real_max_shots: int = 100
    real_default_shots: int = 20
    # Per-minute-priced QPUs bill execution time, which the SDK does not quote before
    # submit. Model: shots x ms_per_shot + overhead_ms, times margin (measured on Rigetti
    # Cepheus-1-108Q 2026-09-07: 100 shots ran 35 ms and billed 7.13 credits).
    ms_per_shot: float = 0.4
    overhead_ms: float = 5.0
    time_margin: float = 1.5
    allow: tuple[str, ...] = ()        # optional allow-list of QPU device ids
    device_cache_s: float = 300.0


@dataclass
class Device:
    id: str
    simulator: bool
    qubits: int
    per_task: float | None
    per_shot: float | None
    per_minute: float | None
    status: str = "UNKNOWN"
    handle: object = field(default=None, repr=False)
    qasm: bool = True      # the SDK profile says it takes OpenQASM (gate model, not analog)

    def priced(self) -> bool:
        p = (self.per_task, self.per_shot, self.per_minute)
        return all(x is not None for x in p) and any(x > 0 for x in p)

    def free(self) -> bool:
        return all(x is not None and x == 0 for x in (self.per_task, self.per_shot, self.per_minute))

    def estimate(self, shots: int, pol: Policy) -> float:
        ms = (shots * pol.ms_per_shot + pol.overhead_ms) * pol.time_margin
        return (self.per_task or 0) + (self.per_shot or 0) * shots + (self.per_minute or 0) * ms / 60000


class Store:
    """SQLite record of every job and the day's spend; survives restarts."""

    def __init__(self, path: str):
        self.db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.lock = threading.Lock()
        self.db.execute("""CREATE TABLE IF NOT EXISTS jobs (
            id TEXT PRIMARY KEY, kind TEXT, device TEXT, provider_job TEXT, sid TEXT, client TEXT,
            shots INTEGER, n_qubits INTEGER, circuit TEXT, qasm TEXT, state TEXT, detail TEXT,
            est_cost REAL, cost REAL, cost_source TEXT, counts TEXT,
            submitted REAL, finished REAL, day TEXT)""")

    def close(self) -> None:
        with self.lock:
            self.db.close()

    def __del__(self):
        try:
            self.db.close()
        except Exception:  # noqa: BLE001 - interpreter shutdown
            pass

    def q(self, sql: str, *args):
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def insert(self, **row) -> None:
        cols = ",".join(row)
        self.q(f"INSERT INTO jobs ({cols}) VALUES ({','.join('?' * len(row))})", *row.values())

    def update(self, jid: str, **row) -> None:
        sets = ",".join(f"{k}=?" for k in row)
        self.q(f"UPDATE jobs SET {sets} WHERE id=?", *row.values(), jid)

    def get(self, jid: str) -> dict | None:
        with self.lock:
            cur = self.db.execute("SELECT * FROM jobs WHERE id=?", (jid,))
            r = cur.fetchone()
            return dict(zip([c[0] for c in cur.description], r)) if r else None

    def count(self, where: str, *args) -> int:
        return self.q(f"SELECT COUNT(*) FROM jobs WHERE {where}", *args)[0][0]

    def spent(self, day: str) -> float:
        """Credits committed today by real jobs: the billed cost when known, else the estimate."""
        r = self.q("SELECT COALESCE(SUM(COALESCE(cost, est_cost)), 0) FROM jobs "
                   "WHERE kind='qpu' AND day=? AND state != 'refused'", day)
        return float(r[0][0])

    def open_jobs(self) -> list[dict]:
        ids = [r[0] for r in self.q("SELECT id FROM jobs WHERE state IN ('queued','running')")]
        return [j for j in (self.get(i) for i in ids) if j]


def utc_day(t: float | None = None) -> str:
    return time.strftime("%Y-%m-%d", time.gmtime(t if t is not None else time.time()))


def iso(t: float | None) -> str | None:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t)) if t else None


# ---- qBraid ------------------------------------------------------------------

def _credits(v) -> float | None:
    if v is None:
        return None
    try:
        return float(str(v))
    except ValueError:
        return None


class QbraidBackend:
    """The only code that talks to qBraid. Blocking calls; the broker runs them in threads."""

    def __init__(self):
        from qbraid.runtime import QbraidProvider
        self.provider = QbraidProvider(api_key=os.environ.get("QBRAID_API_KEY") or None)

    def devices(self) -> list[Device]:
        out = []
        for d in self.provider.get_devices():
            prof = d.profile
            pricing = getattr(prof, "pricing", None)
            spec = getattr(prof, "program_spec", None)
            specs = spec if isinstance(spec, (list, tuple)) else [spec] if spec else []
            out.append(Device(
                id=d.id, simulator=bool(getattr(prof, "simulator", False)),
                qubits=int(getattr(prof, "num_qubits", 0) or 0),
                per_task=_credits(getattr(pricing, "perTask", None)) if pricing else None,
                per_shot=_credits(getattr(pricing, "perShot", None)) if pricing else None,
                per_minute=_credits(getattr(pricing, "perMinute", None)) if pricing else None,
                handle=d, qasm=any(getattr(s, "alias", "") in ("qasm2", "qasm3") for s in specs)))
        return out

    def status(self, dev: Device) -> str:
        return dev.handle.status().name

    def submit(self, dev: Device, qasm: str, shots: int) -> str:
        return dev.handle.run(qasm, shots=shots).id

    def poll(self, provider_job: str) -> dict:
        """{'state': queued|running|done|failed|cancelled, 'counts', 'cost', 'detail'}"""
        from qbraid.runtime.native.job import QbraidJob
        job = QbraidJob(provider_job, client=self.provider.client)
        st = job.status().name
        if st in ("INITIALIZING", "QUEUED", "VALIDATING", "HOLD"):
            return {"state": "queued"}
        if st == "RUNNING":
            return {"state": "running"}
        if st == "COMPLETED":
            res = job.result()
            details = getattr(res, "details", None) or {}
            return {"state": "done", "counts": dict(res.data.get_counts()),
                    "cost": _credits(details.get("cost")) if isinstance(details, dict) else None}
        cost = None
        try:
            cost = _credits(getattr(job.client.get_job(provider_job), "cost", None))
        except Exception:  # noqa: BLE001 - the estimate stays on record
            pass
        return {"state": "cancelled" if st == "CANCELLED" else "failed", "cost": cost, "detail": st}


# ---- the broker --------------------------------------------------------------

class Broker:
    def __init__(self, backend, store: Store, policy: Policy, workers: int = 4):
        self.backend = backend
        self.store = store
        self.pol = policy
        self.pool = ThreadPoolExecutor(max_workers=workers)
        self.submit_lock = asyncio.Lock()   # budget check and reservation are atomic
        self.refresh_lock = asyncio.Lock()  # one poll per job at a time (poller vs `qpu job`)
        self._devices: list[Device] = []
        self._devices_at = 0.0

    async def _run(self, fn, *a):
        return await asyncio.get_running_loop().run_in_executor(self.pool, fn, *a)

    async def devices(self) -> list[Device]:
        if not self._devices or time.monotonic() - self._devices_at > self.pol.device_cache_s:
            self._devices = await self._run(self.backend.devices)
            self._devices_at = time.monotonic()
        return self._devices

    async def pick(self, mode: str, n: int, shots: int) -> tuple[Device, float]:
        """The cheapest suitable device that reports ONLINE right now, with its estimate."""
        devs = await self.devices()
        if mode == "sim":
            cands = [d for d in devs if d.simulator and d.qasm and d.free() and d.qubits >= n]
            # free simulators cost the same; prefer qBraid's own, then by id, for stable output
            cands.sort(key=lambda d: (not d.id.startswith("qbraid:"), d.id))
        else:
            cands = [d for d in devs if not d.simulator and d.qasm and d.priced() and d.qubits >= n
                     and (not self.pol.allow or d.id in self.pol.allow)]
            cands.sort(key=lambda d: (d.estimate(shots, self.pol), d.id))
        for d in cands:
            est = d.estimate(shots, self.pol) if mode == "qpu" else 0.0
            if mode == "qpu" and est > self.pol.max_job_credits:
                break     # sorted by estimate: nothing further is cheaper
            try:
                d.status = await self._run(self.backend.status, d)
            except Exception as e:  # noqa: BLE001
                log.warning("status %s failed: %s", d.id, e)
                continue
            if d.status == "ONLINE":
                return d, est
        if mode == "qpu":
            cheapest = min((d.estimate(shots, self.pol) for d in cands), default=None)
            if cheapest is not None and cheapest > self.pol.max_job_credits:
                raise Refused(f"the cheapest suitable QPU is estimated at {cheapest:.2f} credits for "
                              f"{shots} shots, over the {self.pol.max_job_credits:g}-credit per-job cap")
            raise Refused("no suitable QPU is online within the per-job cap right now")
        raise Refused("no free qBraid simulator is online right now")

    def info(self, req: dict) -> dict:
        day = utc_day()
        sid = str(req.get("sid", ""))
        used = self.store.count("kind='qpu' AND sid=? AND state!='refused'", sid)
        return {"ok": True, "real": self.pol.real, "budget_per_day": self.pol.budget_per_day,
                "spent_today": round(self.store.spent(day), 4), "max_job_credits": self.pol.max_job_credits,
                "real_left_this_session": max(0, self.pol.real_per_session - used),
                "real_max_shots": self.pol.real_max_shots, "sim_max_shots": self.pol.sim_max_shots}

    def validate(self, req: dict) -> tuple[str, int, bytes, int, str]:
        mode = req.get("mode")
        if mode not in ("sim", "qpu"):
            raise Refused("mode must be sim or qpu")
        hexs = str(req.get("circuit", ""))
        if not hexs or len(hexs) % 2 or len(hexs) > 2 * (qc.QC_HDR + 3 * self.pol.max_ops) or not HEX.match(hexs):
            raise Refused("malformed circuit")
        circuit = bytes.fromhex(hexs)
        try:
            n, _probe, ops = qc.parse(circuit)
        except ValueError as e:
            raise Refused(f"malformed circuit: {e}") from None
        cap = self.pol.real_max_qubits if mode == "qpu" else self.pol.sim_max_qubits
        if not 1 <= n <= cap:
            raise Refused(f"{n} qubits; the {'QPU' if mode == 'qpu' else 'simulator'} limit is {cap}")
        if len(ops) > self.pol.max_ops:
            raise Refused(f"{len(ops)} ops; the limit is {self.pol.max_ops}")
        shots = req.get("shots") or 0
        if not isinstance(shots, int) or shots < 0:
            raise Refused("bad shot count")
        if shots == 0:
            shots = self.pol.real_default_shots if mode == "qpu" else self.pol.sim_default_shots
        smax = self.pol.real_max_shots if mode == "qpu" else self.pol.sim_max_shots
        if shots > smax:
            raise Refused(f"{shots} shots; the limit is {smax}")
        n2, qasm = to_qasm2(circuit)
        return mode, shots, circuit, n2, qasm

    async def submit(self, req: dict) -> dict:
        sid = str(req.get("sid", ""))[:64]
        client = str(req.get("client", ""))[:64]
        if not sid:
            raise Refused("no session")
        mode, shots, circuit, n, qasm = self.validate(req)
        day = utc_day()
        async with self.submit_lock:
            if mode == "sim":
                if self.store.count("kind='sim' AND sid=?", sid) >= self.pol.sim_per_session:
                    raise Refused(f"simulator limit reached for this session ({self.pol.sim_per_session} jobs)")
                if self.store.count("kind='sim' AND day=?", day) >= self.pol.sim_per_day:
                    raise Refused("today's simulator limit is used up; try tomorrow (UTC)")
            else:
                if not self.pol.real:
                    raise Refused("real QPU jobs are switched off on this host; `qpu run` uses the simulator")
                if self.store.count("kind='qpu' AND sid=? AND state!='refused'", sid) >= self.pol.real_per_session:
                    raise Refused(f"{self.pol.real_per_session} real QPU job per session; this session has used it")
                if client and self.store.count("kind='qpu' AND client=? AND day=? AND state!='refused'",
                                               client, day) >= self.pol.real_per_client_per_day:
                    raise Refused("one real QPU job per visitor per day; this visitor has used today's")
            dev, est = await self.pick(mode, n, shots)
            if mode == "qpu":
                left = self.pol.budget_per_day - self.store.spent(day)
                if est > left:
                    raise Refused(f"today's QPU budget has {max(left, 0):.2f} of {self.pol.budget_per_day:g} "
                                  f"credits left; this job is estimated at {est:.2f}")
            jid = "q" + secrets.token_hex(5)
            now = time.time()
            self.store.insert(id=jid, kind=mode, device=dev.id, provider_job=None, sid=sid, client=client,
                              shots=shots, n_qubits=n, circuit=circuit.hex(), qasm=qasm, state="submitting",
                              detail=None, est_cost=est, cost=None, cost_source=None, counts=None,
                              submitted=now, finished=None, day=day)
            log.info("submit job=%s kind=%s device=%s shots=%d est_credits=%.4f sid=%s client=%s",
                     jid, mode, dev.id, shots, est, sid, client)
            try:
                pj = await self._run(self.backend.submit, dev, qasm, shots)
            except Exception as e:  # noqa: BLE001 - recorded; the estimate stays reserved until reconciled
                self.store.update(jid, state="failed", detail=f"submit: {type(e).__name__}: {str(e)[:200]}",
                                  finished=time.time())
                log.warning("submit failed job=%s: %s", jid, e)
                raise Refused(f"{dev.id} refused the job ({type(e).__name__})") from None
            self.store.update(jid, provider_job=pj, state="queued")
            log.info("queued job=%s provider_job=%s", jid, pj)
        return self.describe(self.store.get(jid))

    def describe(self, j: dict) -> dict:
        return {"ok": True, "job": j["id"], "kind": j["kind"], "device": j["device"],
                "provider_job": j["provider_job"], "state": j["state"], "shots": j["shots"],
                "n_qubits": j["n_qubits"], "est_cost": j["est_cost"], "cost": j["cost"],
                "cost_source": j["cost_source"], "counts": json.loads(j["counts"]) if j["counts"] else None,
                "submitted": iso(j["submitted"]), "finished": iso(j["finished"]), "detail": j["detail"]}

    async def refresh(self, j: dict) -> dict:
        async with self.refresh_lock:
            return await self._refresh(self.store.get(j["id"]) or j)

    async def _refresh(self, j: dict) -> dict:
        if j["state"] not in ("queued", "running") or not j["provider_job"]:
            return j
        try:
            r = await self._run(self.backend.poll, j["provider_job"])
        except Exception as e:  # noqa: BLE001
            log.warning("poll job=%s failed: %s", j["id"], e)
            return j
        upd: dict = {"state": r["state"]}
        if r["state"] in TERMINAL:
            upd["finished"] = time.time()
            if r.get("counts") is not None:
                upd["counts"] = json.dumps(r["counts"], sort_keys=True)
            if r.get("cost") is not None:
                upd["cost"], upd["cost_source"] = r["cost"], "billed"
            else:
                upd["cost_source"] = "estimate"
            if r.get("detail"):
                upd["detail"] = r["detail"]
            log.info("finished job=%s state=%s device=%s cost=%s est=%.4f", j["id"], r["state"],
                     j["device"], upd.get("cost"), j["est_cost"] or 0)
        self.store.update(j["id"], **upd)
        return self.store.get(j["id"])

    async def job(self, req: dict) -> dict:
        jid = str(req.get("id", ""))
        if not JOB_ID.match(jid):
            raise Refused("job ids look like q followed by 10 hex digits")
        j = self.store.get(jid)
        if j is None:
            raise Refused(f"no job {jid} on this host")
        return self.describe(await self.refresh(j))

    async def poller(self, every: float = 15.0) -> None:
        while True:
            await asyncio.sleep(every)
            for j in self.store.open_jobs():
                await self.refresh(j)

    async def handle(self, req: dict) -> dict:
        op = req.get("op")
        try:
            if op == "info":
                return self.info(req)
            if op == "submit":
                return await self.submit(req)
            if op == "job":
                return await self.job(req)
            raise Refused("unknown op")
        except Refused as e:
            return {"ok": False, "error": str(e)}

    async def serve_conn(self, r: asyncio.StreamReader, w: asyncio.StreamWriter) -> None:
        try:
            while True:
                line = await r.readline()
                if not line:
                    break
                if len(line) > MAX_LINE:
                    ans = {"ok": False, "error": "request too long"}
                else:
                    try:
                        req = json.loads(line)
                        ans = await self.handle(req) if isinstance(req, dict) else {"ok": False, "error": "bad request"}
                    except json.JSONDecodeError:
                        ans = {"ok": False, "error": "bad request"}
                    except Exception as e:  # noqa: BLE001
                        log.exception("request failed")
                        ans = {"ok": False, "error": f"broker error ({type(e).__name__})"}
                w.write(json.dumps(ans).encode() + b"\n")
                await w.drain()
        except (ConnectionError, asyncio.LimitOverrunError, ValueError):
            pass
        finally:
            w.close()


async def amain(a: argparse.Namespace, backend=None, stop: asyncio.Future | None = None) -> int:
    pol = Policy(real=a.real, budget_per_day=a.budget_per_day, max_job_credits=a.max_job_credits,
                 real_per_session=a.real_per_session, real_per_client_per_day=a.real_per_client_per_day,
                 allow=tuple(x for x in a.allow.split(",") if x))
    Path(a.db).parent.mkdir(parents=True, exist_ok=True)
    broker = Broker(backend or QbraidBackend(), Store(a.db), pol)
    sock = Path(a.socket)
    sock.unlink(missing_ok=True)
    server = await asyncio.start_unix_server(broker.serve_conn, str(sock), limit=MAX_LINE * 2)
    os.chmod(sock, 0o660)
    poll = asyncio.create_task(broker.poller(a.poll_seconds))
    log.info("listening %s real=%s budget_per_day=%g max_job_credits=%g", sock, pol.real,
             pol.budget_per_day, pol.max_job_credits)
    if stop is None:
        stop = asyncio.get_running_loop().create_future()
        import signal
        for s in (signal.SIGTERM, signal.SIGINT):
            asyncio.get_running_loop().add_signal_handler(s, lambda: stop.done() or stop.set_result(None))
    try:
        async with server:
            await stop
    finally:
        poll.cancel()
        await asyncio.gather(poll, return_exceptions=True)
        broker.pool.shutdown(wait=False, cancel_futures=True)
        broker.store.close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--real", action="store_true", help="allow real-QPU jobs (within the budget)")
    ap.add_argument("--budget-per-day", type=float, default=10.0, help="credits per UTC day, all visitors")
    ap.add_argument("--max-job-credits", type=float, default=5.0)
    ap.add_argument("--real-per-session", type=int, default=1)
    ap.add_argument("--real-per-client-per-day", type=int, default=1)
    ap.add_argument("--allow", default="", help="comma-separated QPU device ids (empty: any priced QPU)")
    ap.add_argument("--poll-seconds", type=float, default=15.0)
    return ap


def main(argv=None) -> int:
    a = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    return asyncio.run(amain(a))


if __name__ == "__main__":
    sys.exit(main())
