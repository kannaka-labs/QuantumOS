"""COM1 side channel between a console guest and the host quantum broker (#262).

The console VMs have no NIC and no vsock: COM1 is their only channel. qsh's `qpu`
command writes a request as an OSC escape on COM1,

    ESC ] qpu ; <op> ; <tag> [; <args>] BEL

and waits for the answer the launcher writes back on COM1 input,

    ESC ] qres ; <tag> ; <state> ; <job> ; <text> BEL

This module sits between the guest and the visitor's WebSocket:

  * from_guest() removes every qpu/qres escape from what the visitor sees, so the guest
    can neither show the visitor a forged answer nor leak a request into the page. A
    request is captured only up to MAX_PAYLOAD bytes and UNTERMINATED_S seconds; an
    oversized or unterminated one is dropped, never buffered for long.
  * from_visitor() removes every OSC escape from the visitor's keystrokes, so a visitor
    cannot type a request or an answer into the guest.
  * A captured request is checked against a strict grammar, rate-limited per session
    (one in flight, MAX_REQUESTS per WINDOW_S), and only then sent to the broker over its
    Unix socket, with the session id and the salted client hash added here, where the
    guest cannot forge them. The broker applies the spending policy.

The answer's text names the device that ran the job, e.g. "QPU rigetti:...:cepheus-1-108q"
or "qBraid simulator qbraid:qbraid:sim:qir-sv".
"""
from __future__ import annotations

import asyncio
import collections
import json
import logging
import re
import time
from typing import Awaitable, Callable

log = logging.getLogger("qos-console")

PRE_QPU = b"\x1b]qpu;"
PRE_QRES = b"\x1b]qres;"
BEL = 0x07
ESC = 0x1B
MAX_PAYLOAD = 400
UNTERMINATED_S = 2.0
MAX_REQUESTS = 30
WINDOW_S = 600.0
BROKER_TIMEOUT_S = 25.0

REQ_INFO = re.compile(rb"^I;(\d{1,9})$")
REQ_JOB = re.compile(rb"^J;(\d{1,9});(q[0-9a-f]{10})$")
REQ_SUBMIT = re.compile(rb"^S;(\d{1,9});([sq]);(\d{1,6});((?:[0-9a-f]{2}){8,128})$")
REQ_TAG = re.compile(rb"^[IJS];(\d{1,9})(?:;|$)")

STATE_LETTER = {"submitting": "Q", "queued": "Q", "running": "R", "done": "D",
                "failed": "E", "cancelled": "E"}


def clean(s: object, limit: int = 160) -> str:
    """Printable ASCII only; '|' and ';' are the answer's separators."""
    t = "".join(c if 32 <= ord(c) < 127 else "?" for c in str(s))
    return t.replace("|", "/")[:limit]


def label(kind: str, device: str) -> str:
    return f"QPU {device}" if kind == "qpu" else f"qBraid simulator {device}"


def format_counts(counts: dict | None, n: int | None = None) -> str:
    """Bitstrings as qBraid returns them: the last character is qubit 0."""
    if not counts:
        return "no counts"
    order = f" (q{n - 1}..q0)" if n and n > 1 else ""
    items = sorted(counts.items(), key=lambda kv: (-int(kv[1]), kv[0]))
    shown = " ".join(f"{k}={v}" for k, v in items[:8])
    more = f" (+{len(items) - 8} more)" if len(items) > 8 else ""
    return f"counts{order} {shown}{more}"


def format_job(a: dict) -> tuple[str, str, list[str]]:
    """(state letter, job id, lines) for a broker job answer."""
    st = STATE_LETTER.get(a.get("state", ""), "E")
    jid = clean(a.get("job", ""), 16)
    src = label(a.get("kind", ""), clean(a.get("device", ""), 80))
    lines: list[str] = []
    if st in ("Q", "R"):
        verb = "running" if st == "R" else "queued"
        lines.append(f"{verb} on {src}, job {jid} ({a.get('shots')} shots)")
        if a.get("kind") == "qpu":
            lines.append(f"estimated cost {a.get('est_cost', 0):.2f} qBraid credits")
            lines.append(f"QPU queues can take minutes to hours; check later with: qpu job {jid}")
    elif st == "D":
        lines.append(f"{src} job {jid}: done, {a.get('shots')} shots, {a.get('finished')}")
        lines.append(format_counts(a.get("counts"), a.get("n_qubits")))
        cost = a.get("cost")
        if cost is not None:
            lines.append(f"cost {cost:g} qBraid credits (billed)")
        elif a.get("kind") == "qpu":
            lines.append(f"cost not reported yet; estimate {a.get('est_cost', 0):.2f} credits")
    else:
        lines.append(f"{src} job {jid}: {clean(a.get('state', 'failed'))}"
                     + (f" ({clean(a.get('detail'), 80)})" if a.get("detail") else ""))
    lines.append(f"provider job {clean(a.get('provider_job') or '-', 120)}")
    return st, jid, lines


def format_info(a: dict) -> list[str]:
    lines = ["host broker attached; it holds the qBraid key, the guest never sees it",
             f"simulator: a free qBraid simulator, up to {a.get('sim_max_shots')} shots"]
    if a.get("real"):
        lines.append(f"real QPU: on, {a.get('budget_per_day'):g} credits/day for all visitors "
                     f"({a.get('spent_today', 0):.2f} used today), at most {a.get('max_job_credits'):g} "
                     f"per job, up to {a.get('real_max_shots')} shots")
        lines.append(f"real QPU jobs left in this session: {a.get('real_left_this_session')}")
    else:
        lines.append("real QPU: switched off on this host")
    return lines


class QpuRelay:
    def __init__(self, broker_socket: str, sid: str, client: str,
                 write_guest: Callable[[bytes], Awaitable[None]], clock=time.monotonic):
        self.broker_socket = broker_socket
        self.sid = sid
        self.client = client
        self.write_guest = write_guest
        self.clock = clock
        # guest -> visitor scanner
        self.hold = b""          # a possible start of a qpu/qres escape
        self.mode = 0            # 0 normal, 1 capturing a request, 2 discarding
        self.buf = bytearray()
        self.since = 0.0
        # visitor -> guest filter
        self.v_esc = False       # last visitor byte was ESC
        self.v_osc = False       # inside a visitor OSC
        # rate limit
        self.recent: collections.deque[float] = collections.deque()
        self.inflight: asyncio.Task | None = None
        self.tasks: set[asyncio.Task] = set()
        self.requests = 0

    # ---- guest -> visitor ----------------------------------------------------
    def from_guest(self, data: bytes) -> bytes:
        out = bytearray()
        for b in data:
            if self.mode and self.clock() - self.since > UNTERMINATED_S:
                self.mode = 0           # unterminated: drop what was captured
                self.buf.clear()
            if self.mode == 1:
                if b == BEL:
                    self.mode = 0
                    self._request(bytes(self.buf))
                    self.buf.clear()
                elif len(self.buf) >= MAX_PAYLOAD or b in (ESC, 0x0A, 0x0D):
                    self.mode = 2 if b not in (ESC, 0x0A, 0x0D) else 0
                    self.buf.clear()
                    log.info("qpu request dropped sid=%s (oversized or broken)", self.sid)
                else:
                    self.buf.append(b)
                continue
            if self.mode == 2:
                if b in (BEL, 0x0A, 0x0D):
                    self.mode = 0
                continue
            if self.hold or b == ESC:
                h = self.hold + bytes([b])
                if PRE_QPU.startswith(h) or PRE_QRES.startswith(h):
                    if h == PRE_QPU:
                        self.mode, self.since, self.hold = 1, self.clock(), b""
                    elif h == PRE_QRES:
                        self.mode, self.since, self.hold = 2, self.clock(), b""
                    else:
                        self.hold = h
                    continue
                # not ours: release the held bytes; a new ESC may start a new match
                out += self.hold
                self.hold = b""
                if b == ESC:
                    self.hold = b"\x1b"
                    continue
            out.append(b)
        return bytes(out)

    # ---- visitor -> guest ----------------------------------------------------
    def from_visitor(self, data: bytes) -> bytes:
        out = bytearray()
        for b in data:
            if self.v_osc:
                if b in (BEL, 0x0A, 0x0D):
                    self.v_osc = False
                    if b != BEL:
                        out.append(b)
                elif b == ESC:
                    self.v_osc = False
                    self.v_esc = True
                continue
            if self.v_esc:
                self.v_esc = False
                if b == ord("]"):
                    self.v_osc = True
                    continue
                if b == ord("\\"):
                    continue        # the tail of an ESC \ string terminator
                out.append(ESC)
                if b == ESC:
                    self.v_esc = True
                    continue
                out.append(b)
                continue
            if b == ESC:
                self.v_esc = True
                continue
            out.append(b)
        return bytes(out)

    # ---- requests ------------------------------------------------------------
    def _request(self, payload: bytes) -> None:
        m = REQ_TAG.match(payload)
        if not m:
            log.info("qpu request rejected sid=%s (grammar)", self.sid)
            return
        tag = m.group(1).decode()
        now = self.clock()
        while self.recent and now - self.recent[0] > WINDOW_S:
            self.recent.popleft()
        if self.inflight is not None and not self.inflight.done():
            self._spawn(self._answer(tag, "E", "", ["busy: one request at a time"]))
            return
        if len(self.recent) >= MAX_REQUESTS:
            self._spawn(self._answer(tag, "E", "", [f"rate limit: {MAX_REQUESTS} requests per "
                                                    f"{int(WINDOW_S // 60)} minutes per session"]))
            return
        req: dict | None = None
        if REQ_INFO.match(payload):
            req = {"op": "info"}
        elif mj := REQ_JOB.match(payload):
            req = {"op": "job", "id": mj.group(2).decode()}
        elif ms := REQ_SUBMIT.match(payload):
            req = {"op": "submit", "mode": "qpu" if ms.group(2) == b"q" else "sim",
                   "shots": int(ms.group(3)), "circuit": ms.group(4).decode()}
        if req is None:
            self._spawn(self._answer(tag, "E", "", ["malformed request"]))
            return
        self.recent.append(now)
        self.requests += 1
        req.update(sid=self.sid, client=self.client)
        self.inflight = self._spawn(self._handle(tag, req))

    def _spawn(self, coro) -> asyncio.Task:
        t = asyncio.get_running_loop().create_task(coro)
        self.tasks.add(t)
        t.add_done_callback(self.tasks.discard)
        return t

    async def ask_broker(self, req: dict) -> dict:
        r, w = await asyncio.wait_for(asyncio.open_unix_connection(self.broker_socket), 5)
        try:
            w.write(json.dumps(req).encode() + b"\n")
            await w.drain()
            line = await asyncio.wait_for(r.readline(), BROKER_TIMEOUT_S)
            return json.loads(line)
        finally:
            w.close()

    async def _handle(self, tag: str, req: dict) -> None:
        try:
            a = await self.ask_broker(req)
        except Exception as e:  # noqa: BLE001 - the visitor gets a plain answer
            log.warning("qpu broker unreachable sid=%s: %s", self.sid, type(e).__name__)
            await self._answer(tag, "E", "", ["the host quantum broker did not answer"])
            return
        if not a.get("ok"):
            await self._answer(tag, "E", "", [f"refused: {clean(a.get('error', 'unknown'), 200)}"])
        elif req["op"] == "info":
            await self._answer(tag, "I", "", format_info(a))
        else:
            st, jid, lines = format_job(a)
            await self._answer(tag, st, jid, lines)

    async def _answer(self, tag: str, state: str, job: str, lines: list[str]) -> None:
        text = "|".join(clean(x, 200) for x in lines)
        msg = f"{state};{job};{text}".encode()[:560]
        await self.write_guest(PRE_QRES + tag.encode() + b";" + msg + bytes([BEL]))

    async def close(self) -> None:
        for t in list(self.tasks):
            t.cancel()
        if self.tasks:
            await asyncio.gather(*self.tasks, return_exceptions=True)
