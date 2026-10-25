"""qBraid provider plugin (#286): the existing host quantum broker behind the ADR-0026
interface.

This is an adapter, not a second implementation. Every qBraid rule stays where it lives
and keeps running for the console: tools/qpu-broker/qpu_broker.py holds the API key and
applies the simulator default, the per-job cap, the daily budget, the per-session and
per-client limits and the calibration reserve. This plugin speaks that broker's socket
protocol ({"op": "info" | "submit" | "job"}), so neither its behaviour nor the console
relay changes.

Kind:
  qpu_job  spec {"mode": "sim" | "qpu", "circuit": "<wire-format hex>", "shots": n}
           (the circuit wire format of scripts/qpu_circuit.py, as `qsh qpu` sends it)

Quotes: a simulator job is $0, exact. A real QPU job is quoted at the quantum broker's own
per-job cap, the most it will ever let one job cost (1 qBraid credit = $0.01), so the quote
is an upper bound, labelled estimate; the broker picks the cheapest suitable ONLINE QPU at
submit time and records the billed cost when the job finishes.

A job is the resource. Once submitted it cannot be withdrawn, so `teardown` only ends the
lease; the job still finishes and is billed. The lease tag is sent as the job's session id,
which ties it to the quantum broker's own ledger. That broker has no list-by-session call,
so `find` returns nothing: a crash between the submit and the lease recording the job id
leaves the job findable only in the quantum broker's ledger, under the tag.
"""
from __future__ import annotations

import json
import socket

try:                                   # installed: quantumos_agent.broker
    from .provider_broker import Quote, Refused
except ImportError:                    # run from a checkout (tools/provider-broker on sys.path)
    from provider_broker import Quote, Refused

USD_PER_CREDIT = 0.01
CLIENT = "provider-broker"     # the client key the quantum broker counts this broker's jobs under


class QbraidProvider:
    name = "qbraid"

    def __init__(self, socket_path: str = "/run/qos-qpu/broker.sock", timeout: float = 60.0):
        self.path, self.timeout = socket_path, timeout

    def _ask(self, req: dict) -> dict:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(self.timeout)
            try:
                s.connect(self.path)
            except OSError as e:
                raise Refused("qpu_broker_down", f"the quantum broker is not reachable at {self.path}: {e}") from None
            s.sendall((json.dumps(req) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
        ans = json.loads(buf or b"{}")
        if not ans.get("ok"):
            raise Refused("qpu_refused", ans.get("error") or "the quantum broker refused")
        return ans

    def offerings(self) -> dict:
        info = self._ask({"op": "info", "sid": "provider-broker-info", "client": CLIENT})
        return {"qpu_job": {"class": "qpu", "unit": "job",
                            "sim": {"price_usd": 0.0, "max_shots": info["sim_max_shots"]},
                            "qpu": {"enabled": info["real"], "max_job_usd": info["max_job_credits"] * USD_PER_CREDIT,
                                    "max_shots": info["real_max_shots"]},
                            "spec": {"mode": "sim | qpu", "circuit": "wire-format hex", "shots": "int"}}}

    @staticmethod
    def _spec(spec: dict) -> tuple[str, str, int]:
        mode, circuit, shots = spec.get("mode", "sim"), spec.get("circuit"), spec.get("shots", 0)
        if mode not in ("sim", "qpu"):
            raise Refused("bad_spec", "mode must be sim or qpu")
        if not isinstance(circuit, str) or not circuit:
            raise Refused("bad_spec", "circuit must be the wire-format hex string")
        if not isinstance(shots, int) or isinstance(shots, bool) or shots < 0:
            raise Refused("bad_spec", "shots must be a non-negative integer")
        return mode, circuit, shots

    def quote(self, kind: str, spec: dict, duration_s: int) -> Quote:
        if kind != "qpu_job":
            raise Refused("bad_kind", f"qbraid offers: qpu_job (asked for {kind!r})")
        mode, _, _ = self._spec(spec)
        if mode == "sim":
            return Quote("qpu", "job", 0.0, 0.0, exact=True, free_tier="qBraid simulator: free",
                         notes="runs on a free qBraid simulator")
        info = self._ask({"op": "info", "sid": "provider-broker-info", "client": CLIENT})
        if not info["real"]:
            raise Refused("qpu_off", "real QPU jobs are switched off on the quantum broker")
        cap = info["max_job_credits"] * USD_PER_CREDIT
        return Quote("qpu", "job", cap, cap, exact=False,
                     notes=(f"upper bound: the quantum broker's per-job cap ({info['max_job_credits']:g} credits); "
                            "it picks the cheapest suitable online QPU and records the billed cost"))

    def provision(self, kind: str, spec: dict, tag: str) -> str:
        if kind != "qpu_job":
            raise Refused("bad_kind", f"qbraid offers: qpu_job (asked for {kind!r})")
        mode, circuit, shots = self._spec(spec)
        ans = self._ask({"op": "submit", "sid": tag[:64], "client": CLIENT, "mode": mode,
                         "shots": shots, "circuit": circuit})
        return ans["job"]

    def status(self, rid: str) -> dict:
        j = self._ask({"op": "job", "id": rid})
        return {k: j.get(k) for k in ("state", "kind", "device", "provider_job", "shots", "counts",
                                      "est_cost", "cost", "cost_source", "detail")}

    def teardown(self, rid: str) -> None:
        return None    # a submitted job cannot be withdrawn; it finishes and is billed

    def billed(self, rid: str) -> float | None:
        try:
            j = self._ask({"op": "job", "id": rid})
        except Refused:
            return None
        if j.get("cost_source") == "billed" and j.get("cost") is not None:
            return round(float(j["cost"]) * USD_PER_CREDIT, 6)
        return None

    def find(self, tag: str) -> list[str]:
        return []      # no list-by-session call on the quantum broker (see the module docstring)
