#!/usr/bin/env python3
"""QuantumOS N-way society-of-societies gate (societies epic, increment 3;
epic #178 generalized to N>2). Stdlib-only: drives QosSociety directly and never
imports `mcp`, so it runs in CI with no pip.

Boots THREE attested QuantumOS VMs, each with BOTH the `agentdemo` token AND the
coupling wire, and proves — anti-vacuously — the useful cross-node WORK product
at N>2 (not just phase sync):
  1. each VM runs its OWN complete agent society locally (spawn-assembled
     specialists, content consensus, division of labor, and the qseed-salted
     AGGREGATE handoff — the full AGENTD: DEMO OK conjunction, per VM);
  2. the three societies EXCHANGE results over FSYP full-mesh: each VM's COM1
     shows the OTHER TWO members' aggregates (N-1 = 2 per node). Attribution is
     by VALUE — the host recomputes each member's expected aggregate from the
     compile-time phrase + workspace digits + that boot's qseed, and the FIXTURE
     pre-check requires all three expected values PAIRWISE-DISTINCT, so a
     forged/looped-back local value cannot satisfy a peer assertion (the FSYP
     print carries no source field; the distinct qseed-salted value IS the
     attribution — FSYP frames stay source-IP-filtered/unauthenticated per
     ADR-0019, and this host recomputation is their integrity check);
  3. every field still synchronizes under the demo load (the existing society
     property), on a GENEROUS budget — three agentdemo boots (a QPU job, four
     spawns, field ops each) ride the same VMs (the #171 timing lesson);
  4. shutdown reaps all three QEMU processes.

NEW gate rather than an extension of test_qos_society3.py, so the demo-free
society3/society4 gates keep their timing (the #178 design-review finding).
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosSociety  # noqa: E402

# Three distinct qseeds -> three divergent fields AND three distinct
# qseed-salted aggregates (the value-attribution premise). Deliberately not
# palindromic-collision-prone; the pairwise-distinct fixture check below is the
# hard guard regardless.
QSEEDS = ["1111111111111111", "8888888888888888", "cccccccccccccccc"]

# Mirrors user/agentd.c exactly (kept in lockstep with test_qos_society_agents.py):
# the aggregate is FNV-1a over the three specialist result strings ("w%08x" of
# the phrase||digit digest), salted with FNV-1a over the boot qseed's 8 LE bytes.
PHRASE = b"agentd end to end field phrase"
FNV_PRIME = 16777619
FNV_BASIS = 2166136261
M32 = 0xFFFFFFFF


def _fnv1a(data, h=FNV_BASIS):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & M32
    return h


def expected_aggregate(qseed_hex):
    agg = FNV_BASIS
    for ws in (4, 5, 6):
        rd = _fnv1a(PHRASE)
        rd = ((rd ^ (ord("0") + ws)) * FNV_PRIME) & M32
        for b in ("w%08x" % rd).encode():
            agg = ((agg ^ b) * FNV_PRIME) & M32
    q = int(qseed_hex, 16)
    return agg ^ _fnv1a(q.to_bytes(8, "little"))


_SOC = None  # the society under test, so a failure can print the evidence it needs


def _dump_members(lines=40):
    """On failure, print each member's log tail. The verdict alone has not been enough
    to tell a harness race from a guest fault (#361); the next failure carries its logs."""
    soc = _SOC
    if soc is None or not getattr(soc, "members", None):
        return
    for i, m in enumerate(soc.members):
        try:
            tail = m._log_text()[-6000:].splitlines()[-lines:]
        except Exception as e:  # noqa: BLE001 - never let the dump hide the verdict
            tail = [f"(log unavailable: {type(e).__name__})"]
        print(f"--- member {i} log tail ({len(tail)} lines) ---")
        for ln in tail:
            print("   ", ln)


def _fail(msg):
    print(f"FAIL: {msg}")
    _dump_members()
    raise SystemExit(1)


def _kernel():
    return os.environ.get("QOS_KERNEL") or None


def _await(pred, deadline, what):
    while time.time() < deadline:
        if pred():
            return
        time.sleep(0.5)
    _fail(f"timeout awaiting {what}")


def main():
    soc = QosSociety(kernel=_kernel())
    global _SOC
    _SOC = soc
    exp = [expected_aggregate(q) for q in QSEEDS]
    # FIXTURE anti-vacuity: all N expected aggregates must be pairwise-distinct,
    # else value-attribution cannot tell peers apart (would let a relayed value
    # masquerade). Fail the FIXTURE, not the run.
    for i in range(len(exp)):
        for j in range(i + 1, len(exp)):
            if exp[i] == exp[j]:
                _fail(f"expected aggregates {i},{j} collide (a{exp[i]:08x}) — "
                      "pick different qseeds; value-attribution would be vacuous")

    own_re = re.compile(r"AGENT: aggregate a([0-9a-f]{8}) handed to fieldsyncd")
    peer_re = re.compile(r"FIELDSYNC: peer aggregate a([0-9a-f]{8})")
    try:
        st = soc.boot_n(QSEEDS, timeout=60, extra_tokens="agentdemo")
        # boot_n returns per-member STATUS DICTS (identity/verified); the live
        # QosVM handles (with _log_text) are soc.members — keep the two straight.
        st_members = st["members"]
        vms = soc.members
        if len(st_members) != 3 or len(vms) != 3:
            _fail(f"expected 3 members, got status={len(st_members)} vms={len(vms)}")
        print(f"OK: three attested members booted with agentdemo + coupling "
              f"(full 3-cycle mesh); expected aggregates "
              f"{['a%08x' % e for e in exp]}")

        # 1. Each VM completes its OWN society end to end (DEMO OK is a
        # conjunction over qpu + field + spawn + society + division + aggregate).
        deadline = time.time() + 150.0
        _await(lambda: all("AGENTD: DEMO OK" in vm._log_text() for vm in vms),
               deadline, "AGENTD: DEMO OK on all three members")
        for i, vm in enumerate(vms):
            text = vm._log_text()
            if "AGENT: division of labor 3/3" not in text:
                _fail(f"member {i}: no division-of-labor proof")
            om = own_re.search(text)
            if not om:
                _fail(f"member {i}: no aggregate handoff line")
            if int(om.group(1), 16) != exp[i]:
                _fail(f"member {i}: own aggregate {om.group(1)} != expected "
                      f"{exp[i]:08x} — recompute drifted from agentd")
        print("OK: all three societies complete; each own aggregate verified")

        # 2. EXCHANGE (the N-way useful-work product): each member's console must
        # show BOTH other members' aggregates (N-1 = 2), and NEVER its own.
        deadline = time.time() + 90.0

        def _has_all_peers():
            for i, vm in enumerate(vms):
                seen = {int(v, 16) for v in peer_re.findall(vm._log_text())}
                if not all(exp[j] in seen for j in range(3) if j != i):
                    return False
            return True

        _await(_has_all_peers, deadline,
               "every member receiving BOTH peers' aggregates (N-1 each)")
        # Anti-loopback (anti-vacuous): a self-frame leaking through peer
        # validation would print the member's OWN value as a peer aggregate.
        for i, vm in enumerate(vms):
            for v in peer_re.findall(vm._log_text()):
                if int(v, 16) == exp[i]:
                    _fail(f"member {i} printed its OWN aggregate as a peer's — "
                          "self-frame leaked through peer validation")
        print("OK: all three societies exchanged results — each holds both peers' "
              "aggregates (host-recomputed, value-attributed), none its own")

        # 3. The fields still synchronize under the demo load (generous budget).
        final = soc.await_sync_n(threshold=0.80, timeout=180)
        for i, node in enumerate(final["members"]):
            if not node["synchronized"] or (node["r_x"] or 0) < 0.80:
                _fail(f"member {i} did not synchronize under demo load: {node}")
        rxs = [m["r_x"] for m in final["members"]]
        print(f"OK: all three fields synchronized under agentdemo load (R_x {rxs})")
    finally:
        soc.shutdown()
    if soc.members:
        _fail("society did not clear its member handles on shutdown")
    print("OK: society shutdown reaped all three members")

    if "mcp" in sys.modules:
        _fail("the `mcp` package leaked into the stdlib-only test")
    print("=== SOCIETY-OF-SOCIETIES-N gate PASSED — three societies, one field, "
          "results exchanged N-way ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
