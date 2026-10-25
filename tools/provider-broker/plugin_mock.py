"""A provider that provisions nothing real: the broker's tests, and a way to try the
lifecycle without an account. Prices are fixed and labelled exact."""
from __future__ import annotations

import secrets

try:                                   # installed: quantumos_agent.broker
    from .provider_broker import Quote, Refused
except ImportError:                    # run from a checkout (tools/provider-broker on sys.path)
    from provider_broker import Quote, Refused

OFFERINGS = {
    "bucket": {"class": "storage", "unit": "GB-month", "price_usd": 0.0},
    "vm": {"class": "compute", "unit": "hour", "price_usd": 0.01},
    "gpu": {"class": "gpu", "unit": "hour", "price_usd": 1.20},
}


class MockProvider:
    name = "mock"

    def __init__(self):
        self.resources: dict[str, dict] = {}     # rid -> {"kind", "tag", "state"}
        self.fail_next = False                   # the next provision raises
        self.crash_after_create = False          # the next provision creates, then raises
        self.torn_down: list[str] = []

    def offerings(self) -> dict:
        return {k: dict(v) for k, v in OFFERINGS.items()}

    def quote(self, kind: str, spec: dict, duration_s: int) -> Quote:
        o = OFFERINGS.get(kind)
        if o is None:
            raise Refused("bad_kind", f"mock has no {kind!r}; it offers {', '.join(OFFERINGS)}")
        hours = -(-duration_s // 3600)
        units = hours if o["unit"] == "hour" else 1
        return Quote(o["class"], o["unit"], o["price_usd"], round(o["price_usd"] * units, 6), exact=True,
                     free_tier="always free" if o["price_usd"] == 0 else None,
                     notes="mock provider: nothing real is created")

    def provision(self, kind: str, spec: dict, tag: str) -> str:
        if self.fail_next:
            self.fail_next = False
            raise RuntimeError("mock provider refused")
        rid = f"mock-{kind}-{secrets.token_hex(3)}"
        self.resources[rid] = {"kind": kind, "tag": tag, "state": "running"}
        if self.crash_after_create:
            self.crash_after_create = False
            raise RuntimeError("mock crash after create")
        return rid

    def status(self, rid: str) -> dict:
        r = self.resources.get(rid)
        return {"state": r["state"] if r else "gone"}

    def teardown(self, rid: str) -> None:
        if rid in self.resources:
            del self.resources[rid]
            self.torn_down.append(rid)

    def billed(self, rid: str) -> float | None:
        return None          # the mock has no bill; the ledger shows billed as unknown

    def find(self, tag: str) -> list[str]:
        return [rid for rid, r in self.resources.items() if r["tag"] == tag]
