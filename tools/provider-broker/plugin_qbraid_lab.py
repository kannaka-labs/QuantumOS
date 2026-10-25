"""qBraid Lab compute plugin (#297 slice 4): the account's Lab server and on-demand
instances, priced per minute, with the person's own qBraid key.

Kinds (spec {"profile": "<slug>"} for both; `broker providers` lists the profiles):
  lab_server  the account's one Jupyter Lab server, started on a profile. There is only ever
              one per account, so it is ours if the broker started it, and the broker starts
              it only when it is stopped (`lab_server_busy` otherwise).
  instance    an on-demand machine. qBraid instances CANNOT be tagged, so a crash between
              creating one and recording its id would leave an instance no tag can find.

How a crash is handled for `instance` (Nick, 2026-10-25: options b and c):
  * An intent journal is written BEFORE every create: the lease tag and the instance ids
    that already exist. The id is added once the create returns.
  * Option b, `sole_user=True` (the person declares the broker the account's only Lab
    user): provisioning is refused while any instance exists that the journal does not own.
    After a crash, an instance that is not in the journal's "already existed" list and not
    owned by another lease can only be ours, so it is returned for teardown.
  * Option c, the default: provisioning is allowed, but after a crash the plugin never
    guesses. It returns nothing for teardown, logs an error naming the candidate instances,
    and queues an alert that the broker writes to the ledger as `orphan_possible`. The person
    decides; an untracked instance bills until someone terminates it.

Quotes are per minute from qBraid's live profile pricing (1 credit = $0.01), for the whole
lease, labelled estimates: Free-plan CPU profiles may be covered by qBraid's free CPU-Lab
hours, so the real charge can be lower. Teardown stops the Lab server, and TERMINATES an
instance (a stopped instance still bills for its disk).
"""
from __future__ import annotations

import json
import logging
import os
import time
from pathlib import Path

try:                                   # installed: quantumos_agent.broker
    from .provider_broker import Quote, Refused
except ImportError:                    # run from a checkout
    from provider_broker import Quote, Refused

log = logging.getLogger("provider-broker")
USD_PER_CREDIT = 0.01
KINDS = ("lab_server", "instance")
GONE = {"terminated", "terminating", "deleted"}


def _client():
    from qbraid_core.services.compute import ComputeClient
    key = os.environ.get("QBRAID_API_KEY")
    return ComputeClient(api_key=key) if key else ComputeClient()


def _state(obj) -> str:
    st = getattr(obj, "status", None)
    st = getattr(st, "value", st)
    return str(st or getattr(obj, "state", "") or "").lower()


class QbraidLabProvider:
    name = "qbraid_lab"

    def __init__(self, client=None, journal: str | None = None, sole_user: bool = False,
                 cluster: str | None = None, clock=time.time):
        self.c = client or _client()
        self.sole_user, self.cluster, self.clock = sole_user, cluster, clock
        self.journal_path = Path(os.path.expanduser(journal or "~/.qos-broker/qbraid_lab_journal.json"))
        self._alerts: list[dict] = []

    # ---- journal ----
    def _load(self) -> dict:
        try:
            return json.loads(self.journal_path.read_text())
        except (OSError, ValueError):
            return {}

    def _save(self, j: dict) -> None:
        self.journal_path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        tmp = self.journal_path.with_name(self.journal_path.name + ".tmp")
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(j, f, sort_keys=True)
        os.replace(tmp, self.journal_path)

    def _owned(self, j: dict) -> set[str]:
        return {e["rid"] for e in j.values() if e.get("rid") and e.get("kind") == "instance"}

    def _instance_ids(self) -> list[str]:
        return [i.instance_id for i in self.c.list_bma_instances() if _state(i) not in GONE]

    def drain_alerts(self) -> list[dict]:
        out, self._alerts = self._alerts, []
        return out

    # ---- catalog and quotes ----
    def offerings(self) -> dict:
        profs = []
        for p in self.c.list_profiles():
            profs.append({"profile": p.slug, "gpu": bool(getattr(p, "gpu", False)), "plan": getattr(p, "plan", None),
                          "capacity": getattr(p, "has_capacity", None)})
        return {"lab_server": {"class": "compute or gpu", "unit": "minute", "spec": {"profile": "slug"},
                               "note": "the account's one Lab server; started only when it is stopped"},
                "instance": {"class": "compute or gpu", "unit": "minute", "spec": {"profile": "slug"},
                             "sole_user": self.sole_user,
                             "note": "terminated at teardown; see the plugin docstring for crash handling"},
                "profiles": profs}

    @staticmethod
    def _credits_per_min(p) -> float:
        """qBraid's listed `$rate / time_frame`, as credits per minute. An unknown time frame is
        read as per-minute, the conservative (most expensive) reading."""
        usd = getattr(p, "rate_dollar", None)
        if usd is None:
            return 0.0
        tf = str(getattr(p, "rate_time_frame", "") or "").lower()
        usd_min = float(usd) / 60 if ("hour" in tf or tf in ("hr", "h")) else             float(usd) * 60 if "sec" in tf else float(usd)
        return round(usd_min / USD_PER_CREDIT, 6)

    def _profile(self, spec: dict):
        """The profile as LISTED (qBraid's get_profile rejects slugs its own list returns, such
        as '2vCPU_4GB', so the list is the source of truth for both existence and price)."""
        slug = spec.get("profile")
        if not isinstance(slug, str) or not slug or len(slug) > 64:
            raise Refused("bad_spec", "profile must be a qBraid compute profile slug")
        match = [p for p in self.c.list_profiles() if getattr(p, "slug", None) == slug]
        if not match:
            raise Refused("bad_spec", f"no qBraid profile {slug!r}; see `providers` for the list")
        det = match[0]
        rate = self._credits_per_min(det)
        if not rate:
            raise Refused("no_price", f"qBraid lists no price for {slug!r}, so it cannot be bounded")
        return det, rate

    def quote(self, kind: str, spec: dict, duration_s: int) -> Quote:
        if kind not in KINDS:
            raise Refused("bad_kind", f"qbraid_lab offers: {', '.join(KINDS)} (asked for {kind!r})")
        det, credits_per_min = self._profile(spec)
        usd_min = credits_per_min * USD_PER_CREDIT
        minutes = -(-duration_s // 60)
        free = getattr(det, "plan", None) == "Free" and not getattr(det, "gpu", False)
        return Quote("gpu" if getattr(det, "gpu", False) else "compute", "minute", round(usd_min, 6),
                     round(usd_min * minutes, 6), exact=False,
                     free_tier="may be covered by qBraid's free CPU-Lab hours" if free else None,
                     notes=(f"{credits_per_min:g} credits/min from qBraid's live pricing, for {minutes} min; "
                            + ("teardown stops the server" if kind == "lab_server"
                               else "teardown terminates the instance (a stopped one still bills its disk)")))

    # ---- provision ----
    def provision(self, kind: str, spec: dict, tag: str) -> str:
        if kind not in KINDS:
            raise Refused("bad_kind", f"qbraid_lab offers: {', '.join(KINDS)}")
        det, _ = self._profile(spec)
        j = self._load()
        if kind == "lab_server":
            st = self.c.get_server_status(cluster_id=self.cluster)
            if getattr(st, "server_running", False) or getattr(st, "server_starting", False):
                raise Refused("lab_server_busy", "the account's Lab server is already running; the broker only "
                                                 "starts it when it is stopped, so it never takes over someone else's")
            j[tag] = {"kind": kind, "rid": None, "at": self.clock()}
            self._save(j)
            self.c.start_server(det.slug, cluster_id=self.cluster)
            rid = f"lab_server:{self.cluster or 'default'}"
        else:
            existing = self._instance_ids()
            if self.sole_user:
                foreign = sorted(set(existing) - self._owned(j))
                if foreign:
                    raise Refused("not_sole_user", "sole-user mode, but the account has instances the broker did "
                                                   f"not start ({', '.join(foreign)}); refusing to provision")
            j[tag] = {"kind": kind, "rid": None, "at": self.clock(), "pre": existing}
            self._save(j)
            inst = self.c.provision_bma_instance(det.slug)
            rid = getattr(inst, "instance_id", None)
            if not rid:
                raise RuntimeError("qBraid returned no instance id")
        j = self._load()
        j[tag]["rid"] = rid
        self._save(j)
        return rid

    # ---- status / teardown ----
    def status(self, rid: str) -> dict:
        if rid.startswith("lab_server:"):
            st = self.c.get_server_status(cluster_id=self.cluster)
            return {"state": "running" if getattr(st, "server_running", False) else
                    ("starting" if getattr(st, "server_starting", False) else "stopped"),
                    "profile": getattr(st, "current_profile", None)}
        try:
            inst = self.c.get_bma_instance(rid)
        except Exception as e:  # noqa: BLE001
            if "404" in str(e) or "not found" in str(e).lower():
                return {"state": "gone"}
            raise
        return {"state": _state(inst) or "unknown", "profile": getattr(inst, "profile_slug", None),
                "ready": getattr(inst, "ready", None),
                "running_credits_per_min": getattr(inst, "running_credits_per_min", None)}

    def teardown(self, rid: str) -> None:
        if rid.startswith("lab_server:"):
            st = self.c.get_server_status(cluster_id=self.cluster)
            if getattr(st, "server_running", False) or getattr(st, "server_starting", False):
                self.c.stop_server(cluster_id=self.cluster)
        else:
            if self.status(rid)["state"] not in GONE | {"gone"}:
                self.c.terminate_bma_instance(rid)
        j = self._load()
        for tag, e in list(j.items()):
            if e.get("rid") == rid:
                del j[tag]
        self._save(j)

    def billed(self, rid: str) -> float | None:
        return None   # qBraid reports usage per account, not per lease

    def find(self, tag: str) -> list[str]:
        j = self._load()
        e = j.get(tag)
        if not e:
            return []
        if e.get("rid"):
            return [e["rid"]] if self.status(e["rid"])["state"] not in GONE | {"gone", "stopped"} else []
        # A crash between the intent and the recorded id.
        if e["kind"] == "lab_server":
            st = self.c.get_server_status(cluster_id=self.cluster)
            running = getattr(st, "server_running", False) or getattr(st, "server_starting", False)
            if running:
                return [f"lab_server:{self.cluster or 'default'}"]   # we checked it was stopped first
            del j[tag]
            self._save(j)
            return []
        candidates = sorted(set(self._instance_ids()) - set(e.get("pre") or []) - self._owned(j))
        if not candidates:
            del j[tag]
            self._save(j)
            return []
        if self.sole_user:
            return candidates                     # option b: nobody else creates instances here
        alert = {"tag": tag, "candidates": candidates,
                 "message": "a crash may have left a qBraid instance the broker cannot attribute; it was NOT "
                            "terminated. Check these and terminate yours (they bill per minute)."}
        self._alerts.append(alert)
        log.error("qbraid_lab: possible orphan for %s: %s (not terminated: not in sole-user mode)", tag, candidates)
        return []
