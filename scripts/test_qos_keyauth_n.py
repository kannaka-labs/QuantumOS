#!/usr/bin/env python3
"""N-way authenticated swarm-plane gate (epic #139 + ADR-0019): prove a
host-admitted group session key gates the FIELD-COUPLING wire across a THREE-VM
mean-field society — the N>2 generalization of the 2-VM ci-smoke-keyauth.

Stdlib-only: drives QosSociety directly (never imports `mcp`), so it runs in the
integration CI job with no pip. Needs qemu + host multicast.

THE CENTRAL CORRECTION (why this gate does NOT assert on R_x / SYNCHRONIZED):
an R_x-based "the keyless node is excluded" claim is VACUOUS. A keyless receiver
skips the have_key MAC guard and ACCEPTS everything, so a keyless C in a
2-keyed+1-keyless mesh becomes a ONE-WAY Kuramoto follower — it phase-locks to
A/B's authenticated frames and prints R_x>=0.80 + SYNCHRONIZED while its OWN
zero-tag frames are rejected by A/B. All three print SYNCHRONIZED with the key
withheld. R_x discriminates nothing.

The REAL discriminator is frame-ADMISSION, offset-anchored at each member's FSKEY:
  leg (a) EXCLUSION — key A,B only. On A AND B: the FSAUTH bad-MAC line appears
    (C's zero-tag frames rejected) AND the per-source 'frame from C' count STOPS
    growing (delta==0 past the key boundary). POSITIVELY confirm C still receives
    fresh frames from A/B (the one-way lock — documented, not a failure).
  leg (b) ADMIT — key C too (SAME group key). Fresh 'frame from C' lines RESUME
    on A AND B (offset-anchored past C's FSKEY); THEN the whole society synchronizes
    (await_sync_n anchored past the keying boundary — a fresh sync, not a stale
    pre-key SYNCHRONIZED read off the sticky log).
  leg (c) REPLAY (real) — the host joins the society's mcast group, captures one
    member's live FSYN datagram verbatim, waits >1s, and re-sends it. The receiver
    sees a VALID MAC + a STALE seq and prints the per-reason replay line (needs
    increment 0's per-reason split — a shared flag would have swallowed it behind
    the bad-MAC line). Then a 2nd admit prints the rekey line.

Revert-confirms (documented in the Makefile gate):
  - skip the leg-(b) admit  -> frame-from-C never resumes + sync times out (red).
  - collapse the per-reason flag back to one shared latch -> leg (c) replay marker
    never prints (swallowed behind leg (a)'s bad-MAC line) (red).

Run: python3 scripts/test_qos_keyauth_n.py     (boots three VMs; needs qemu+mcast)
Gate: make ci-smoke-keyauth-n
"""

import atexit
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qos_bridge import QosSociety, QosDead  # noqa: E402

QSEEDS = ["1111111111111111", "8888888888888888", "cccccccccccccccc"]
KEY = bytes([0x11]) * 32          # ONE group key — the same 32 bytes to every member
A, B, C = 0, 1, 2
A_IP, B_IP, C_IP = "10.0.0.1", "10.0.0.2", "10.0.0.3"

BADMAC = "FSAUTH: forged FSYN frame rejected (bad MAC)"
REPLAY = "FSAUTH: stale FSYN frame rejected (replay)"
REKEY = "FSKEY: rekeyed (replay watermarks reset)"


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


def _wait_marker(soc, idx, marker, since, timeout, what):
    """Poll until `marker` appears in member idx's log at/after `since`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not soc.members[idx].is_running():
            raise QosDead(f"member {idx} exited before {what}")
        if marker in soc.members[idx]._log_text(since):
            return True
        time.sleep(0.3)
    return False


def _leg_a_exclusion(soc):
    print("--- leg (a): EXCLUSION — key A,B only, C withheld ---")
    c_off = soc.members[C]._log_len()          # anchor C's ONGOING reception check
    offs = soc.admit_key_n(KEY, indices=[A, B])  # SAME key to A and B; C keyless
    print(f"OK: group key admitted to A,B (FSKEY anchors {offs})")

    # A AND B must reject C's zero-tag frames — the bad-MAC admission signal.
    for idx, name in ((A, "A"), (B, "B")):
        if not _wait_marker(soc, idx, BADMAC, offs[idx], 30, "bad-MAC reject"):
            _fail(f"leg(a): {name} never rejected the keyless peer's frames "
                  f"({BADMAC!r} absent past the key boundary) — MAC not gating")
    print("OK: A and B both reject the keyless peer's forged-MAC frames")

    # The per-source discriminator: C's frames STOP being admitted on A/B.
    # Sample across a >=15s window (past the ~15s peer-stale drop) — the count
    # since the key boundary must stay 0 (no new 'frame from C' is admitted).
    print("    sampling frame-from-C admission on A,B for 16s (stale window)...")
    t_end = time.time() + 16
    while time.time() < t_end:
        for idx, name in ((A, "A"), (B, "B")):
            n = soc.frame_from_count(idx, C_IP, offs[idx])
            if n != 0:
                _fail(f"leg(a): {name} admitted {n} frame(s) from the keyless C "
                      f"past the key boundary — exclusion not enforced")
        time.sleep(1.0)
    print("OK: neither A nor B admits ANY frame from the keyless C (delta==0)")

    # POSITIVE one-way-lock assertion: C (keyless) still RECEIVES fresh frames
    # from A and B — documented Kuramoto follower behaviour, NOT a failure. This
    # is exactly why an R_x/SYNCHRONIZED assertion here would be vacuous.
    from_a = soc.frame_from_count(C, A_IP, c_off)
    from_b = soc.frame_from_count(C, B_IP, c_off)
    if from_a <= 0 or from_b <= 0:
        _fail(f"leg(a): the keyless C stopped receiving from A/B "
              f"(from_a={from_a} from_b={from_b}) — expected the one-way lock")
    print(f"OK: keyless C still receives fresh frames from A({from_a}) and "
          f"B({from_b}) — the one-way lock R_x cannot discriminate")


def _leg_b_admit(soc):
    print("--- leg (b): ADMIT — key C with the SAME group key ---")
    ab_off = {A: soc.members[A]._log_len(), B: soc.members[B]._log_len()}
    off = soc.admit_key_n(KEY, indices=[C])
    print(f"OK: group key admitted to C (FSKEY anchor {off})")

    # Fresh 'frame from C' must RESUME on A and B, anchored past their new offset.
    for idx, name in ((A, "A"), (B, "B")):
        deadline = time.time() + 45
        while time.time() < deadline:
            if soc.frame_from_count(idx, C_IP, ab_off[idx]) > 0:
                break
            if not soc.members[idx].is_running():
                raise QosDead(f"member {idx} exited before C's frames resumed")
            time.sleep(0.3)
        else:
            _fail(f"leg(b): {name} never admitted C's frames after C was keyed "
                  f"— authenticated coupling did not resume")
    print("OK: A and B admit C's authenticated frames again (coupling resumed)")

    # NOW the whole society synchronizes — anchored PAST the keying boundary so it
    # is a fresh, post-admission sync, not a stale pre-key SYNCHRONIZED line.
    since = [ab_off[A], ab_off[B], off[C]]
    st = soc.await_sync_n(threshold=0.80, timeout=180, since=since)
    rxs = [(m["r_x"], m["r_x_min"]) for m in st["members"]]
    for i, nd in enumerate(st["members"]):
        if not nd["synchronized"] or (nd["r_x"] or 0) < 0.80:
            _fail(f"leg(b): member {i} did not synchronize post-admission: {nd}")
    print(f"OK: all three authenticated members synchronized post-key (r_x,min) {rxs}")


def _capture_fsyn(mgroup, mport, timeout):
    """Join the society's mcast group and capture ONE live FSYN datagram
    verbatim (the whole encapsulated Ethernet frame QEMU puts on the wire). We
    filter for the FSYN magic so we never grab an FSYP aggregate."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", mport))
    mreq = struct.pack("4sl", socket.inet_aton(mgroup), socket.INADDR_ANY)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.settimeout(1.0)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            try:
                data, _ = s.recvfrom(2048)
            except socket.timeout:
                continue
            # 'FSYN' magic (little-endian uint32 -> bytes F,S,Y,N) inside the
            # encapsulated frame; require a full phase frame's worth of payload.
            if b"FSYN" in data and len(data) >= 296:
                return s, data
    except OSError:
        pass
    s.close()
    return None, None


def _leg_c_replay(soc):
    print("--- leg (c): REPLAY (real host injector) ---")
    mgroup, mport = soc._mcast_group, soc._mcast_port
    if not mgroup or not mport:
        _fail("leg(c): society exposed no mcast group/port for the injector")
    print(f"    joining mcast group {mgroup}:{mport} to capture a live FSYN...")
    sock, dgram = _capture_fsyn(mgroup, mport, timeout=20)
    if dgram is None:
        _fail("leg(c): host could NOT capture an FSYN datagram on the society "
              "mcast group — is host multicast reachable to a joiner on this "
              "runner? (the guests coupled, so the L2 delivers guest<->guest)")
    print(f"OK: captured a {len(dgram)}-byte FSYN datagram; waiting >1s then replaying")

    # Anchor the replay marker check on every member AT the injection boundary.
    pre = [m._log_len() for m in soc.members]
    time.sleep(1.5)   # let the real sender advance its seq past the captured one
    for _ in range(4):
        sock.sendto(dgram, (mgroup, mport))   # verbatim re-send — valid MAC, stale seq
        time.sleep(0.4)
    sock.close()

    # SOME keyed member (not the original sender — its own IP isn't in its peer
    # set) must print the per-reason replay line past its injection anchor.
    hit = None
    deadline = time.time() + 30
    while time.time() < deadline and hit is None:
        for i, m in enumerate(soc.members):
            if REPLAY in m._log_text(pre[i]):
                hit = i
                break
        time.sleep(0.3)
    if hit is None:
        _fail(f"leg(c): no member printed {REPLAY!r} after the verbatim replay "
              f"— the per-reason replay latch is not surfacing (or the frame was "
              f"not admitted as a valid-MAC stale-seq)")
    print(f"OK: member {hit} rejected the replayed frame (valid MAC + stale seq)")

    # A 2nd admit must print the rekey line (increment 0's re-install signal).
    rk = soc.admit_key_n(KEY)   # re-admit the SAME key to ALL members
    rekeyed = 0
    for i, off in rk.items():
        # admit_key_n already waited for an FSKEY line; confirm it was the REKEY
        # variant (watermarks reset), not a first-install.
        if REKEY in soc.members[i]._log_text(0):
            rekeyed += 1
    if rekeyed < len(soc.members):
        _fail(f"leg(c): only {rekeyed}/{len(soc.members)} members printed the "
              f"rekey line ({REKEY!r}) on re-admission")
    print(f"OK: all {rekeyed} members printed the rekey line on re-admission")


def main():
    soc = QosSociety(kernel=_kernel())
    global _SOC
    _SOC = soc
    atexit.register(soc.shutdown)
    try:
        st = soc.boot_n(QSEEDS, timeout=45)
        members = st["members"]
        if len(members) != 3:
            _fail(f"expected 3 members, got {len(members)}")
        if not all(m["verified"] for m in members):
            _fail(f"all three members must attest: {st}")
        print("OK: three attested members booted, full 3-cycle mesh established")

        _leg_a_exclusion(soc)
        _leg_b_admit(soc)
        _leg_c_replay(soc)
    finally:
        soc.shutdown()
    if soc.members:
        _fail("society did not clear its member handles on shutdown")
    if "mcp" in sys.modules:
        _fail("the `mcp` package leaked into the stdlib-only test")
    print("OK: society shutdown reaped all three members; no `mcp` import")
    print("=== N-WAY KEYAUTH gate PASSED — the group key gates the field wire "
          "for N>2 (exclusion + admit + real replay) ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
