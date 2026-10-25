#!/usr/bin/env python3
"""
QuantumOS MCP lifecycle gate (epic #99). Stdlib-only: it drives qos_bridge
directly (the exact code path the FastMCP tools delegate to) and NEVER imports
the `mcp` package, so it runs in the integration CI job that has no pip.

Full lifecycle against ONE VM, every step anti-vacuous:
  1. boot qseed=DEADBEEFCAFEBABE -> attestation verified AND attested qseed
     matches the request (numeric).
  2. ghostd STATUS over COM2 -> live >= 3 (the boot self-test's field is alive
     in the bridge path; asserting ==1 would be red, ghost_test imprints 3).
  3+4. imprint TWO distinct phrases at the kernel field, recall a typo'd copy
     of each -> each returns its OWN phrase and the two winners DIFFER
     (discrimination — best-of-one cannot satisfy it). (The field is
     content-addressable: every probe lands on its nearest attractor, so
     there is no "returns nothing" case to assert.)
  5. run /bin/hello -> exit code 42, hello's unique deterministic signature
     (qsh reports it from SYS_WAITPID; the number cannot come from echoing the
     typed 'run /bin/hello'). One attempt, no retry: the "stillborn spawn" this
     used to retry past was SYS_WAITPID returning a recycled pid's PREVIOUS
     owner's exit code (#291, fixed by the process generation in the exit
     ledger), so a wrong code here is now a real regression.
  5b-5h. The Phase A toolbox (epic #125), each anti-vacuous: run /bin/args
     'alpha beta' -> exit 3 + un-echoable `ARGS: argv[2]=beta`; sysinfo ->
     advancing uptime, positive heap, a process table with qsh RUNNING and only
     known states, an RTC date; qrand -> two distinct 16-hex draws; fs ->
     write/rm roundtrip on the overlay with an honest no-disk sync and a
     rm-missing error; fetch -> a hermetic in-process http.server (guest reaches
     it via SLIRP 10.0.2.2, bytes >= the 4096-byte body, server hit >= 1), a
     dead-port refusal, and the SSRF guard refusing 10.0.2.2 by default;
     injection -> a \n payload refused on ALL SIX line-builders (imprint,
     recall, run-args, fs-text, fs-path, fetch-host) with a canary that must
     survive, plus the security-critical qsh line-length budget (exact ok,
     +1 refused, not truncated).
  6. TAMPER: flip one SIG / ATTEST / PKDIGEST payload byte in the captured
     attestation AND recompute its CRC (so the frame still parses and the
     Lamport digest check — not the checksum — is what rejects it), re-verify
     via the SAME parser, inject into a fresh QosVM, and assert vm.status()
     REFUSES — the exact guard the tools hit.
  7. shutdown -> the QEMU process is gone (proc.poll(), not a raw waitpid).

Exit 0 on success. A SIGTERM (timeout) or any exception still reaps QEMU via
QosVM.shutdown() in the finally + atexit.
"""

import http.server
import json
import sys
import os
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qos_bridge
from qos_bridge import (
    QosVM, QosError, QosRefused, attestation_from_bytes,
    QSH_LINE_MAX, FrameParser, FRAME_SIG, FRAME_ATTEST, FRAME_PKDIGEST, MAGIC,
)

# A stand-in for kannaka.exe (absent on the QuantumOS CI runner). `recall` echoes
# the canned JSON at QOS_STUB_RECALL; `remember` records "importance\ttext" to
# the store — BUT honours KANNAKA_READONLY by no-op'ing the record (mirroring the
# real binary), so the gate can prove KannakaCLI strips an inherited READONLY.
_STUB_KANNAKA = r'''import json, os, sys
def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(2)
    if a[0] == "recall":
        p = os.environ.get("QOS_STUB_RECALL")
        sys.stdout.write(open(p, encoding="utf-8").read() if p and os.path.exists(p) else "[]")
        return 0
    if a[0] == "remember":
        text, imp, i = None, "0.5", 1
        while i < len(a):
            if a[i] == "--importance" and i + 1 < len(a):
                imp = a[i + 1]; i += 2; continue
            if a[i].startswith("--"):
                i += 2; continue
            if text is None:
                text = a[i]
            i += 1
        ro = os.environ.get("KANNAKA_READONLY", "")
        if ro and ro.lower() not in ("0", "false"):
            sys.stdout.write("00000000-0000-0000-0000-000000000000"); return 0
        with open(os.path.join(os.environ.get("KANNAKA_DATA_DIR", "."),
                               "remembered.tsv"), "a", encoding="utf-8") as f:
            f.write(imp + "\t" + (text or "") + "\n")
        sys.stdout.write("11111111-1111-1111-1111-111111111111"); return 0
    sys.exit(2)
sys.exit(main())
'''

_PROC_STATES = {"UNUSED", "CREATED", "READY", "RUNNING", "BLOCKED",
                "TERMINATED", "ZOMBIE"}
_CANARY = "/notes/canary.txt"

QSEED = "DEADBEEFCAFEBABE"
PHRASE_A = "the cat sat on the mat"
PHRASE_A_NOISY = "the cxt sat on thx mat"
PHRASE_B = "quantum ghosts dream in phase"
PHRASE_B_NOISY = "quantum ghxsts drexm in phase"


def _fail(msg):
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def _first_frame_span(blob, ftype):
    """(frame_start, payload_start, crc_pos) of the first frame of `ftype`."""
    i = 0
    n = len(blob)
    while i < n:
        if blob[i] != MAGIC:
            i += 1
            continue
        if i + 4 > n:
            break
        length = blob[i + 2] | (blob[i + 3] << 8)
        if length > qos_bridge.SWARM_MAX_PAYLOAD:
            i += 1
            continue
        end = i + 4 + length
        if end + 1 > n:
            break
        if qos_bridge.crc8(blob[i + 1:end]) == blob[end]:
            if blob[i + 1] == ftype:
                return i, i + 4, end
            i = end + 1
        else:
            i += 1
    return None


def _assert_tamper_refuses(raw, ftype, label):
    span = _first_frame_span(raw, ftype)
    if span is None:
        _fail(f"tamper setup: no {label} frame in the capture")
    fstart, pstart, crc_pos = span
    mutated = bytearray(raw)
    mutated[pstart] ^= 0x01                                 # flip one payload bit
    mutated[crc_pos] = qos_bridge.crc8(mutated[fstart + 1:crc_pos])  # fix CRC so the
    # frame still parses — this forces the Lamport digest check (not the CRC) to
    # be what rejects the tamper.
    att = attestation_from_bytes(bytes(mutated), QSEED)
    if att.verified:
        _fail(f"tamper ({label}): a flipped byte still verified")
    if "CRC" in att.reason:
        _fail(f"tamper ({label}): rejected at CRC, not the signature: {att.reason}")
    vm = QosVM()
    vm.attestation = att           # inject; no boot, no socket
    try:
        vm.status()
    except QosRefused as exc:
        print(f"OK: tamper ({label}) -> refused by crypto: {att.reason}")
        return
    _fail(f"tamper ({label}): status() did not refuse")


class _FixedBody(http.server.BaseHTTPRequestHandler):
    """A hermetic in-process server: 200 + a known 4096-byte body, counting
    hits, so the fetch gate proves the guest's TCP stack reached OUR server end
    to end (bytes >= body length AND a request actually landed) rather than
    passing on a bare status line. No pip, no egress — loopback via SLIRP."""
    body = b"Q" * 4096
    hits = 0

    def do_GET(self):
        _FixedBody.hits += 1
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(_FixedBody.body)))
        self.end_headers()
        self.wfile.write(_FixedBody.body)

    def log_message(self, *_a):
        pass


def _exercise_run_args(vm):
    """run /bin/args with an argument vector -> exit code is argc (un-echoable:
    the kernel splits the line into argv and /bin/args prints it from ring 3).
    One attempt: a recycled pid must not report its previous owner's exit (#291)."""
    out = vm.run("/bin/args", args="alpha beta")
    if out["exit_code"] != 3:      # argv0 + 2 args
        _fail(f"/bin/args argc != 3 (last exit_code={out['exit_code']})")
    if "ARGS: argv[2]=beta" not in out["output"]:
        _fail(f"/bin/args did not echo argv[2]=beta: {out['output']!r}")
    print("OK: run /bin/args 'alpha beta' -> exit 3, argv[2]=beta (un-echoable)")


def _exercise_sysinfo(vm):
    info = vm.sysinfo()
    if info["uptime"].get("ticks", 0) <= 0:
        _fail(f"sysinfo uptime not advancing: {info['uptime']}")
    if info["mem"].get("heap_free_bytes", 0) <= 0:
        _fail(f"sysinfo heap_free not positive: {info['mem']}")
    procs = info["processes"]
    if len(procs) < 3:
        _fail(f"sysinfo process table too small: {len(procs)}")
    if not any(p["name"] == "qsh" and p["state"] == "RUNNING" for p in procs):
        _fail("sysinfo did not show qsh RUNNING (the shell driving this)")
    bad = [p for p in procs if p["state"] not in _PROC_STATES]
    if bad:
        _fail(f"sysinfo unknown process state(s): {bad}")
    if not (info["date"] or "").startswith("20"):
        _fail(f"sysinfo date not an RTC timestamp: {info['date']!r}")
    print(f"OK: sysinfo uptime={info['uptime']['s']}s heap_free="
          f"{info['mem']['heap_free_bytes']} procs={len(procs)} date={info['date']}")


def _exercise_qrand(vm):
    a = vm.qrand()["hex"]
    b = vm.qrand()["hex"]
    if len(a) != 16 or any(c not in "0123456789abcdef" for c in a):
        _fail(f"qrand not 16 lowercase hex: {a!r}")
    if a == b:
        _fail("two qrand draws were identical (entropy dead)")
    print(f"OK: qrand drew {a} then {b} (distinct)")


def _exercise_fs(vm):
    w = vm.fs("write", path="/notes/probe.txt", text="hello-quantum-42")
    if w["bytes"] < len("hello-quantum-42"):
        _fail(f"fs write byte count too low: {w}")
    try:
        s = vm.fs("sync")
        print(f"OK: fs sync flushed {s['flushed']} file(s) (disk attached)")
    except QosError as exc:
        if "no disk" not in str(exc):
            _fail(f"fs sync failed for an unexpected reason: {exc}")
        print(f"OK: fs sync honestly reported no disk ({exc})")
    if not vm.fs("rm", path="/notes/probe.txt")["removed"]:
        _fail("fs rm did not report removal")
    try:
        vm.fs("rm", path="/notes/probe.txt")
        _fail("fs rm of a now-missing file did not error")
    except QosError:
        pass
    print("OK: fs write -> (sync) -> rm roundtrip; rm-missing errored")


def _exercise_fetch(vm):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FixedBody)
    port = srv.server_address[1]
    _FixedBody.hits = 0
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        res = vm.fetch("10.0.2.2", port=port, allow_private=True)
        if not res.get("ok"):
            _fail(f"hermetic fetch failed: {res}")
        if "200" not in res["status_line"]:
            _fail(f"fetch status line not 200: {res['status_line']!r}")
        if res["bytes"] < len(_FixedBody.body):
            _fail(f"fetch byte count {res['bytes']} < body {len(_FixedBody.body)}")
        if not res["complete"]:
            _fail(f"fetch reported an incomplete transfer: {res}")
        if _FixedBody.hits < 1:
            _fail("fetch never reached the in-process server (hits=0)")
        print(f"OK: fetch 10.0.2.2:{port} -> {res['status_line']} "
              f"{res['bytes']} bytes, server saw {_FixedBody.hits} request(s)")
    finally:
        srv.shutdown()
        srv.server_close()

    # Negative: the SAME port is now dead (server closed) -> a genuine failure,
    # not a flake. Run only after the positive proved NIC + lease, so the error
    # discriminates. libslirp may surface a refused connect as either line.
    neg = vm.fetch("10.0.2.2", port=port, allow_private=True)
    if neg.get("ok"):
        _fail(f"fetch of a dead port unexpectedly succeeded: {neg}")
    if neg["error"] not in ("connect failed", "no response (timed out)"):
        _fail(f"dead-port fetch error not in the expected set: {neg['error']!r}")
    print(f"OK: fetch of the closed port refused ({neg['error']})")

    # SSRF policy: a loopback/SLIRP-host target is refused BEFORE the network
    # unless allow_private is set (default False).
    try:
        vm.fetch("10.0.2.2", port=port)
        _fail("SSRF guard did not refuse 10.0.2.2 by default")
    except QosError as exc:
        if "private" not in str(exc).lower():
            _fail(f"SSRF refusal for the wrong reason: {exc}")
    print("OK: SSRF guard refused 10.0.2.2 by default (allow_private required)")


def _exercise_injection(vm):
    """Every method that splices agent input into a qsh line must refuse a
    control byte, and a pre-written canary must survive byte-identical. The
    read-free survival proof: rm the canary at the end — if it still exists the
    rm succeeds, if any injection had run `rm <canary>` the final rm errors."""
    vm.fs("write", path=_CANARY, text="canary-INTACT")
    payload = "x\nrm " + _CANARY                      # a newline => second command
    builders = [
        ("imprint", lambda: vm.imprint(payload)),
        ("recall", lambda: vm.recall(payload)),
        ("run-args", lambda: vm.run("/bin/args", args=payload)),
        ("fs-text", lambda: vm.fs("write", path="/notes/x.txt", text=payload)),
        ("fs-path", lambda: vm.fs("write", path="/notes/x\nrm " + _CANARY, text="y")),
        ("fetch-host", lambda: vm.fetch("h\nrm " + _CANARY, allow_private=True)),
    ]
    for label, fn in builders:
        try:
            fn()
            _fail(f"injection via {label} was NOT refused")
        except QosError:
            pass
    survived = vm.fs("rm", path=_CANARY)
    if not survived.get("removed"):
        _fail("canary was deleted — an injection leaked a second command")
    print("OK: \\n-injection refused on all 6 line-builders; canary survived")

    # The line budget is security-critical (over-budget => qsh runs a
    # truncated, different command): exactly-budget succeeds end to end,
    # budget+1 is refused (not silently truncated). Exercised via `write`,
    # whose argument reaches the ramfs verbatim (the field's 64-byte
    # FIELD_PAT_MAX would otherwise mask the line budget).
    bud_path = "/notes/bud.txt"
    budget = QSH_LINE_MAX - len("write ") - len(bud_path) - 1
    w = vm.fs("write", path=bud_path, text="L" * budget)   # exactly fills the line
    if w["bytes"] < budget:
        _fail(f"exact-budget write stored too few bytes: {w}")
    try:
        vm.fs("write", path=bud_path, text="L" * (budget + 1))
        _fail("over-budget write was not refused")
    except QosError:
        pass
    vm.fs("rm", path=bud_path)
    print(f"OK: qsh line budget enforced (write {budget} ok, {budget + 1} refused)")


_KNOWN_RES = {"MEM", "IPC", "DEV", "QRNG", "PROC", "SVC", "FIELD"}


def _exercise_cpuquota(vm):
    """CPU-quota enforcement (epic #144). The cpu-hog citizen has a finite
    manifest cpu_limit and busy-spins forever; once its scheduled-in cpu_ticks
    cross the limit, the KERNEL terminates it from the timer tick. The hog is
    dead and cannot self-report — the proof is external and un-forgeable:
      (a) a kernel-written CPUKILL entry in the authority ledger for its pid
          (a citizen cannot forge an AUDIT: line; only the kernel kill site
          emits CPUKILL), and
      (b) that pid vanishes from the bound-only manifest dump (manifest_clear'd
          at the kill).
    Runs FIRST (before the spawn-heavy manifest gate) so the boot CPUKILL entry
    is still inside the 128-entry ring; polls the ledger (the kill lands at a
    boot tick, not instantly)."""
    deadline = time.time() + 20
    kill = None
    while time.time() < deadline:
        for e in vm.audit()["entries"]:
            if (e["kind"] == "CPUKILL" and e["verdict"] == "EPERM"
                    and e["resource_type"] == "PROC"):
                kill = e
                break
        if kill:
            break
        time.sleep(0.4)
    if not kill:
        _fail("no CPUKILL ledger entry — the cpu-hog was never terminated for its budget")
    hog_pid = kill["pid"]
    print(f"OK: CPU quota enforced — kernel terminated cpu-hog (pid {hog_pid}) for exceeding "
          f"its budget (un-echoable CPUKILL ledger entry)")

    # The killed hog is manifest_clear'd, so its pid leaves the bound-only dump.
    # Poll (the reaper/clear may lag the ledger write by a moment).
    gone = False
    dl2 = time.time() + 10
    while time.time() < dl2:
        if hog_pid not in vm.manifest()["manifests"]:
            gone = True
            break
        time.sleep(0.4)
    if not gone:
        _fail(f"killed cpu-hog pid {hog_pid} still present in the manifest dump (not reaped/cleared)")
    print(f"OK: killed cpu-hog pid {hog_pid} vanished from the manifest table (reaped + cleared)")

    # Opt-in: qsh (cpu_limit 0) runs forever and is NEVER killed — a nonzero
    # budget is required to be eligible, so the OS's own surfaces are safe.
    qsh_pid = next((p["pid"] for p in vm.sysinfo()["processes"]
                    if p["name"] == "qsh" and p["state"] == "RUNNING"), None)
    if qsh_pid is None or qsh_pid not in vm.manifest()["manifests"]:
        _fail("qsh (cpu_limit 0) should be alive and bound — opt-in enforcement broken")
    print("OK: cpu quota is opt-in — qsh (unlimited) runs untouched")


def _exercise_audit(vm):
    """Capability authority ledger (epic #133). The KERNEL records GRANTs and
    DENYs, so a citizen cannot forge or suppress its own entry. The conscience
    proof is un-echoable: the boot self-test's CAPLESS citizen is refused
    COM2/console/spawn/field and the kernel logs each as DENY verdict=EPERM."""
    a = vm.audit()
    if not a["entries"] or (a["total"] or 0) <= 0:
        _fail(f"audit ledger empty: {a}")
    if a["capacity"] != 256:
        _fail(f"audit capacity != 256: {a['capacity']}")
    # dropped-underflow guard: while total <= capacity, dropped MUST be 0 (a u64
    # total-capacity would underflow to ~1.8e19).
    if a["total"] <= a["capacity"] and a["dropped"] != 0:
        _fail(f"audit dropped should be 0 while total<=capacity: {a['total']}/{a['dropped']}")
    grants = [e for e in a["entries"] if e["kind"] == "GRANT"]
    denies = [e for e in a["entries"] if e["kind"] == "DENY" and e["verdict"] == "EPERM"]
    if not grants:
        _fail("no GRANT entries — the kernel records no authority")
    if not denies:
        _fail("no DENY entries — the conscience proof (a refused capless op) is missing")
    if denies[0]["resource_type"] not in _KNOWN_RES:
        _fail(f"DENY names an unknown resource type: {denies[0]}")
    # Honesty fix (epic #135): audit.h CLAIMED quantum DENY coverage the code
    # never hooked. ghost_test's capless SYS_QRAND now records a real QRNG DENY,
    # so the header claim is finally true — assert it, un-echoable.
    if not any(e["kind"] == "DENY" and e["resource_type"] == "QRNG" for e in a["entries"]):
        _fail("no QRNG DENY — the qrand audit_deny honesty fix is not live")
    print("OK: audit records a QRNG DENY (capless qrand — the claimed coverage is now real)")
    # Deny-coverage completion (ADR-0009): sys_send/sys_send_to used to return
    # EPERM with NO ledger entry, so a citizen probing IPC caps it does not hold
    # was invisible to the authority ledger. paradox-test's capless send_to now
    # records a real IPC DENY — assert it (un-echoable; the header claim made true).
    if not any(e["kind"] == "DENY" and e["resource_type"] == "IPC" for e in a["entries"]):
        _fail("no IPC DENY — the sys_send/send_to ownership-denial audit hook is not live")
    print("OK: audit records an IPC DENY (capless send — the ledger deny-coverage gap is closed)")
    seqs = [e["seq"] for e in a["entries"]]
    if seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
        _fail(f"audit seq not strictly increasing: {seqs[:12]}")
    print(f"OK: audit ledger — {a['total']} events, {len(grants)} GRANT, {len(denies)} DENY "
          f"(pid {denies[0]['pid']} denied {denies[0]['resource_type']}:"
          f"{denies[0]['resource_id']} perms=0x{denies[0]['perms']:x})")

    # Revocation lifecycle (REVOKE vs REAP split). Site-specific signatures, NOT
    # bare kind presence — cap_selftest/quantum_selftest mint REVOKE every boot
    # through the explicit path alone, so kind presence would stay green with
    # the reaper hooks completely dead. The delegation demo is the un-fakeable
    # probe: only it ever derives a FIELD cap, so a REAP for FIELD:2 perms=0xB
    # can only be the reaper freeing the exited delegator's own R|W|GRANT cap,
    # and FIELD:2 perms=0x1 only the cascade freeing subagentd's derived READ
    # cap (which must record the CHILD's owner — a different pid). tick>0
    # excludes pre-timer selftest events AND restored prior-boot entries
    # (audit_load zeroes ticks), future-proofing against a disk on the MCP
    # boot. Polled: the delegator must exit and be reaped by the idle loop.
    deadline = time.time() + 20.0
    g1 = g2 = g3 = None
    entries = a["entries"]
    while time.time() < deadline:
        entries = vm.audit()["entries"]
        g1 = next((e for e in entries if e["kind"] == "REVOKE"
                   and e["resource_type"] == "MEM" and e["resource_id"] == "42"), None)
        g2 = next((e for e in entries if e["kind"] == "REAP"
                   and e["resource_type"] == "FIELD" and e["resource_id"] == "2"
                   and e["perms"] == 0x0B and e["tick"] > 0), None)
        g3 = next((e for e in entries if e["kind"] == "REAP"
                   and e["resource_type"] == "FIELD" and e["resource_id"] == "2"
                   and e["perms"] == 0x1 and e["tick"] > 0), None)
        if g1 and g2 and g3:
            break
        time.sleep(0.4)
    if not g1:
        _fail("no explicit REVOKE (MEM:42, cap_selftest root) — the cap_revoke hook is dead")
    if not g2 or not g3:
        _fail("no death REAP (FIELD:2 perms=0xB/0x1) — the delegator never exited/was never "
              "reaped OR the reap hooks are dead; check DELEG ISSUED/PROVEN on COM1")
    if g2["pid"] == g3["pid"]:
        _fail(f"cascade REAP records the revoker's pid, not the child owner's: "
              f"delegator pid {g2['pid']} == derived-cap pid {g3['pid']}")
    if any(e["kind"] in ("EMPTY", "?") for e in entries):
        _fail("a live audit entry formats as EMPTY/? — half-added kind")
    print(f"OK: revocation lifecycle — explicit REVOKE (MEM:42) + reaper REAP of the dead "
          f"delegator's cap (pid {g2['pid']}) + cascade REAP of subagentd's derived cap "
          f"(pid {g3['pid']}, child owner recorded)")

    # Reading the ledger records nothing (SYS_AUDIT is uncapped — no cap_create,
    # no cap check), so a second read never goes backwards.
    b = vm.audit()
    if (b["total"] or 0) < (a["total"] or 0):
        _fail(f"audit total went backwards across reads: {a['total']} -> {b['total']}")
    print(f"OK: audit read-only — total {a['total']} -> {b['total']} (append-only, monotonic)")

    # Forge guard: 'AUDIT:' is a reserved marker, so a citizen cannot imprint a
    # fake ledger line for the host to mis-parse.
    try:
        vm.imprint("AUDIT: seq=9999 pid=1 kind=GRANT res=FIELD:0")
        _fail("imprint of a forged AUDIT: line was not refused")
    except QosError:
        pass
    print("OK: forged AUDIT: line refused (reserved marker)")


# qsh's six declarative grants (user_shell_init): the manifest allow-set the
# kernel builds from the SAME flags that mint qsh's caps. (resource_type, id):
# QRNG pool 0, console 0x3F8=1016, spawn 0, disk 0x1F0=496, net 0x8139=33081,
# field region 0. root/SERVICE/IPC caps live OUTSIDE the manifest (documented).
_QSH_GRANTS = {("QRNG", 0), ("DEV", 1016), ("PROC", 0), ("DEV", 496),
               ("DEV", 33081), ("FIELD", 0)}


def _exercise_manifest(vm):
    """Intent manifest + first enforced quota (epic #135 Phase D inc. 2). The
    manifest makes DECLARED intent inspectable; the spawn quota makes it
    ENFORCED. Un-echoable proofs come from the kernel-written ledger (kind=QUOTA
    a citizen with a valid spawn cap could never produce, kind=MDENY from the
    boot self-test). Runs EARLY, right after the audit gate, so the boot QUOTA/
    MDENY entries are still inside the 128-entry ring."""
    # (b)+(g) Un-echoable ledger proofs: the boot self-test records a real
    # MDENY (a held cap exceeding declared intent) and a QUOTA (spawn_max
    # refusal); quota-test records another QUOTA at boot. These kinds cannot be
    # forged (AUDIT: is a reserved marker) and the cap layer alone never emits
    # them.
    a = vm.audit()
    mdeny = [e for e in a["entries"] if e["kind"] == "MDENY"]
    quota = [e for e in a["entries"]
             if e["kind"] == "QUOTA" and e["verdict"] == "EPERM" and e["resource_type"] == "PROC"]
    if not mdeny:
        _fail("no MDENY entry — the manifest deny path is dead (constant-true check?)")
    if not quota:
        _fail("no QUOTA EPERM PROC entry — the spawn quota never enforced at boot")
    print(f"OK: ledger proves enforcement — {len(mdeny)} MDENY, {len(quota)} QUOTA EPERM "
          f"(pid {quota[0]['pid']} refused a spawn over its declared quota)")

    # (a) qsh's own manifest is inspectable and EXACTLY its six grant rows.
    qsh_pid = next((p["pid"] for p in vm.sysinfo()["processes"]
                    if p["name"] == "qsh" and p["state"] == "RUNNING"), None)
    if qsh_pid is None:
        _fail("no RUNNING qsh in the process table")
    man = vm.manifest()
    if man["truncated"]:
        _fail("manifest dump truncated at boot — sizing assumption broke")
    qm = man["manifests"].get(qsh_pid)
    if not qm or not qm["bound"]:
        _fail(f"qsh (pid {qsh_pid}) has no bound manifest: {qm}")
    rows = {(r["res"], r["id"]) for r in qm["allow"]}
    if rows != _QSH_GRANTS:
        _fail(f"qsh manifest allow-set {sorted(rows)} != its six grants {sorted(_QSH_GRANTS)}")
    if qm["spawn_max"] is not None:
        _fail(f"qsh spawn_max should be unlimited (*), got {qm['spawn_max']}")
    print(f"OK: qsh manifest inspectable — pid {qsh_pid}, exactly its 6 grant rows, spawn=*")

    # (f) cpu_ticks accounting is LIVE (not dead code): qsh's manifest carries a
    # NONZERO cpu_ticks, so manifest_tick ran in scheduler_tick and correctly
    # charged qsh's slot at least once. This is the un-fakeable proof that the
    # accounting is wired. (A strict monotonic-advance check is NOT robust: qsh
    # is a yield-heavy shell that is rarely the *current* process at a 10 ms
    # timer-tick boundary, so on a contended CI runner its cpu_ticks can stay
    # flat across the whole exercise even though accounting works — a nonzero
    # value already refutes the "dead code" failure the panel flagged.)
    cpu0 = qm["cpu_ticks"]
    if cpu0 <= 0:
        _fail(f"qsh cpu_ticks is {cpu0} — accounting never charged a running qsh (dead code?)")
    print(f"OK: cpu_ticks live — qsh charged {cpu0} timer tick(s) (accounting wired, not dead)")

    # (d) Read-only-by-CONTRAST: reads don't mutate, a spawn does. Baseline
    # qsh spawn_used, one successful run, +1 exactly. A failed spawn (bad path)
    # must NOT bump. Guard the rare watchdog-rebirth (pid change) mid-test.
    base = vm.manifest()["manifests"][qsh_pid]["spawn_used"]
    try:
        vm.run("/bin/nonexistent-quota-probe")
    except QosError:
        pass
    after_fail = vm.manifest()["manifests"].get(qsh_pid, {}).get("spawn_used")
    if after_fail != base:
        _fail(f"a FAILED spawn consumed quota: {base} -> {after_fail}")
    for _ in range(5):
        vm.run("/bin/hello")
        if vm.manifest()["manifests"].get(qsh_pid, {}).get("spawn_used", base) > base:
            break
    now = vm.manifest()["manifests"].get(qsh_pid)
    if now is None:
        _fail("qsh manifest vanished mid-contrast (unexpected rebirth?)")
    if now["spawn_used"] != base + 1:
        _fail(f"qsh spawn_used should be base+1 after one real spawn: {base} -> {now['spawn_used']}")
    print(f"OK: manifest read-only by contrast — spawn_used {base} (failed spawn) "
          f"-> {now['spawn_used']} (one real spawn); reads never mutated")

    # (e) Forge guard: MANIFEST: is a reserved marker, so a citizen cannot
    # imprint a fake manifest row for the host to mis-parse.
    try:
        vm.imprint("MANIFEST: pid=1 bound=1 spawn=0/* cpu_ticks=1")
        _fail("imprint of a forged MANIFEST: line was not refused")
    except QosError:
        pass
    print("OK: forged MANIFEST: line refused (reserved marker)")


def _exercise_qpu(vm):
    """QPU job broker (epic #148 A2+A3). The kernel brokers OPAQUE circuits
    between capability-gated submitters (qpu-test, QPU WRITE) and the executor
    (qpud, QPU EXECUTE) — it enforces the cap, the qsub quota, and the job-slot
    lifecycle but never parses a circuit. Un-echoable, ledger-corroborated:
    QSUBMIT records the exercised WRITE authority; a LIVE qsub QUOTA deny
    (tick>0, distinct from the tick=0 selftest twin) proves the quota; DENYs
    with perms=WRITE (capless + executor submit) and perms=EXECUTE (a submitter
    trying to COMPLETE) prove the two-perm partition. Runs right after the
    manifest gate so the boot QSUBMIT/QUOTA/DENY entries are still in the ring."""
    QPU = "20816"  # DEVICE_ID_QPU (0x5150) as the ledger renders resource_id
    # qpu-test idles in a yield loop after its proof, so it is almost always
    # READY (rarely the RUNNING process at the snapshot instant) — accept any
    # live state, unlike the shell which is RUNNING while it serves sysinfo.
    p = next((pr["pid"] for pr in vm.sysinfo()["processes"]
              if pr["name"] == "qpu-test" and pr["state"] in ("RUNNING", "READY")), None)
    if p is None:
        _fail("no live qpu-test process (the broker proof citizen)")
    entries = vm.audit()["entries"]

    # Half-added-kind tripwire — now effective (the bridge renders 'kind=?').
    if any(e["kind"] in ("EMPTY", "?") for e in entries):
        _fail("a live audit entry formats as EMPTY/? — half-added kind")

    # Accepted submissions: exactly 2 (qsub_max). The two are field-identical
    # and consecutive, so they COALESCE — assert the summed count, never the
    # entry count (a poll-window interleaver could still split them).
    qsub = [e for e in entries if e["kind"] == "QSUBMIT" and e["pid"] == p]
    for e in qsub:
        if not (e["resource_type"] == "DEV" and e["resource_id"] == QPU and e["perms"] == 0x2
                and e["verdict"] == "OK" and e["tick"] > 0):
            _fail(f"QSUBMIT entry has wrong fields: {e}")
    if sum(e["count"] for e in qsub) != 2:
        _fail(f"QSUBMIT count sum != 2 (qsub_max): {[e['count'] for e in qsub]}")

    # LIVE qsub QUOTA deny: the third submit refused (tick>0, qpu-test pid).
    if not any(e["kind"] == "QUOTA" and e["resource_type"] == "DEV" and e["resource_id"] == QPU
               and e["perms"] == 0x2 and e["verdict"] == "EPERM" and e["tick"] > 0 and e["pid"] == p
               for e in entries):
        _fail("no LIVE qsub QUOTA deny (DEV:20816 WRITE, tick>0, qpu-test pid)")

    # The synthetic selftest twin (tick==0, pid==1) must ALSO exist — proves the
    # deny path is driven at boot AND that the live deny above is not the twin.
    if not any(e["kind"] == "QUOTA" and e["resource_id"] == QPU and e["tick"] == 0 and e["pid"] == 1
               for e in entries):
        _fail("no synthetic qsub QUOTA selftest (DEV:20816, tick=0, pid=1)")

    # SUBMIT denials (perms=WRITE): the capless ghost_test AND the executor qpud
    # (EXECUTE holder) both tried and were refused — >=2 distinct pids.
    wdeny = {e["pid"] for e in entries
             if e["kind"] == "DENY" and e["resource_type"] == "DEV" and e["resource_id"] == QPU
             and e["perms"] == 0x2 and e["verdict"] == "EPERM" and e["tick"] > 0}
    if len(wdeny) < 2:
        _fail(f"expected >=2 distinct pids denied QPU WRITE (capless + executor): {wdeny}")

    # Cross-perm (perms=EXECUTE): a submitter's COMPLETE attempt refused.
    if not any(e["kind"] == "DENY" and e["resource_id"] == QPU and e["perms"] == 0x4
               and e["verdict"] == "EPERM" and e["tick"] > 0 for e in entries):
        _fail("no cross-perm DENY (DEV:20816 EXECUTE — submitter tried to COMPLETE)")

    # Manifest: qpu-test's qsub row reads used==max==2 (quota exhausted, durable).
    qm = vm.manifest()["manifests"].get(p)
    if not qm or qm.get("qsub_used") != 2 or qm.get("qsub_max") != 2:
        _fail(f"qpu-test manifest qsub != 2/2: {qm}")
    print(f"OK: QPU broker ledger — 2 QSUBMIT + live qsub QUOTA (pid {p}) + {len(wdeny)} WRITE "
          f"denials + cross-perm EXECUTE deny; qpu-test qsub=2/2")


def _exercise_delegation(vm):
    """Cross-ring capability delegation (epic #137 Phase D inc. 3). At boot the
    delegator (delegation-test) hands a NARROWED READ-only slice of field region
    2 to the sub-agent (subagentd) via SYS_CAP_DERIVE. The un-echoable,
    derive-UNIQUE proof is subagentd's manifest gaining a FIELD:2 allow row: it
    has NO field grant flag, so that row is structurally impossible at boot and
    can only come from the runtime derive. Runs EARLY (right after the manifest
    gate) so the boot derive-GRANT / overreach-DENY are still in the 128-ring."""
    # subagentd idles forever (present); the delegator EXITS by design (its
    # death is what cascade-revokes the derived cap), so it is gone from the
    # table by now — that absence is expected, not an error.
    procs = vm.sysinfo()["processes"]
    sub_pid = next((p["pid"] for p in procs if p["name"] == "subagentd"), None)
    if sub_pid is None:
        _fail("no subagentd in the process table (delegation demo didn't start)")

    # PRIMARY provenance (durable, un-echoable, derive-unique): subagentd's
    # manifest carries a FIELD:2 allow row. subagentd has no field grant, so a
    # bind-time manifest could never contain it — only SYS_CAP_DERIVE's runtime
    # manifest_grant appends it. (The row persists even after the delegator's
    # cascade-revoke — cosmetic there, durable evidence here.)
    man = vm.manifest()
    sm = man["manifests"].get(sub_pid)
    if not sm or not sm["bound"]:
        _fail(f"subagentd (pid {sub_pid}) has no bound manifest: {sm}")
    field2 = [r for r in sm["allow"] if r["res"] == "FIELD" and r["id"] == 2]
    if not field2:
        _fail(f"subagentd manifest has no delegated FIELD:2 row (derive didn't extend it): {sm}")
    if field2[0]["perms"] != 0x1:
        _fail(f"delegated manifest row perms {field2[0]['perms']:#x} != READ(0x1) — not narrowed")
    print(f"OK: delegation provenance — subagentd (pid {sub_pid}) manifest gained a FIELD:2 "
          f"READ row it could only get at runtime (no field grant flag)")

    # Ledger corroboration (early read → still in the ring): a narrowed GRANT
    # for the sub-agent, and the overreach DENY when it tried to WRITE.
    a = vm.audit()
    dgrant = [e for e in a["entries"] if e["pid"] == sub_pid and e["kind"] == "GRANT"
              and e["resource_type"] == "FIELD" and e["resource_id"] == "2" and e["perms"] == 0x1]
    if not dgrant:
        _fail(f"no narrowed FIELD:2 READ GRANT for subagentd pid {sub_pid} in the ledger")
    overreach = [e for e in a["entries"] if e["pid"] == sub_pid and e["kind"] == "DENY"
                 and e["resource_type"] == "FIELD" and (e["perms"] & 0x2)]
    if not overreach:
        _fail(f"no overreach WRITE DENY for subagentd pid {sub_pid} (narrowing unproven)")
    print(f"OK: ledger corroborates — narrowed GRANT (FIELD:2 READ) + overreach WRITE DENY "
          f"for subagentd pid {sub_pid}")

    # ONE-HOP: the ONLY legitimate GRANT-bearing FIELD cap is the delegator's
    # own boot grant (perms 0xb = READ|WRITE|GRANT). A derived cap can never
    # carry CAP_GRANT (the kernel masks it), so ANY other FIELD GRANT with 0x08
    # — e.g. a narrowed-but-still-grantable 0x09 handed to a third pid — would be
    # laundering. Del_pid-independent (the delegator has exited): key on perms.
    launder = [e for e in a["entries"] if e["kind"] == "GRANT" and e["resource_type"] == "FIELD"
               and (e["perms"] & 0x08) and e["perms"] != 0x0B]
    if launder:
        _fail(f"a GRANT-bearing FIELD cap other than the delegator's exists (not one-hop): "
              f"{launder[0]}")
    print("OK: delegation is one-hop (the narrowed cap carries no CAP_GRANT to re-delegate)")


def _find_slot(vm, phrase):
    """The field slot whose preview is a prefix of `phrase` (previews are the
    first <= 16 stored bytes), or None."""
    for s in vm.field_info()["slots"]:
        if s["preview"] and phrase.startswith(s["preview"]):
            return s
    return None


def _exercise_field_info(vm):
    """Read-only field introspection (epic #127 B1). The heart is a
    positive/negative CONTRAST: field_info must NOT bump retrievals/energy, but
    a recall (qsh holds CAP_WRITE) MUST — so 'retrievals stayed 0' alone is
    vacuous; the recall bumping the SAME slot is what proves non-mutation."""
    # Energy carried, un-echoable (computed in-kernel): --energy 90 vs default.
    P = "field energy probe alpha unique"
    Q = "field default probe beta unique"
    vm.imprint(P, energy_pct=90)
    vm.imprint(Q)
    sp, sq = _find_slot(vm, P), _find_slot(vm, Q)
    if not sp or not 86 <= sp["energy"] <= 93:
        _fail(f"imprint --energy 90 not carried into the slot: {sp}")
    if not sq or not 47 <= sq["energy"] <= 53:
        _fail(f"default imprint energy not ~50%: {sq}")
    print(f"OK: field energy carried — P={sp['energy']}% (--energy 90), "
          f"Q={sq['energy']}% (default 50)")

    # Read-only proof: 5 field_info calls leave P's retrievals AND raw energy
    # unchanged (assert RAW energy, not eff — eff decays with ticks by design).
    r0, e0 = sp["retrievals"], sp["energy"]
    if r0 != 0:
        _fail(f"fresh P should have 0 retrievals, got {r0}")
    for _ in range(5):
        s = _find_slot(vm, P)
        if s["retrievals"] != r0 or s["energy"] != e0:
            _fail(f"field_info MUTATED the field: {s} vs retr={r0} energy={e0}")
    # A noisy recall reinforces the SAME slot — the contrast that makes the
    # read-only claim non-vacuous.
    vm.recall("field energy probe alpxa unique")
    after = _find_slot(vm, P)
    if not after or after["retrievals"] != r0 + 1:
        _fail(f"recall did not reinforce P (contrast broken): {after}")
    print(f"OK: field_info read-only (P retr stayed {r0} over 5 calls); "
          f"recall bumped it to {after['retrievals']}")

    # Capacity/eviction visibility: overfill region 0 -> live caps at capacity.
    cap = vm.field_info()["capacity"]
    for i in range(cap + 1):
        vm.imprint(f"capfill varied phrase number {i} distinct xyz", energy_pct=40)
    live = vm.field_info()["live"]
    if live != cap or cap != 8:
        _fail(f"field capacity not enforced at 8: live={live} cap={cap}")
    print(f"OK: field capacity enforced (live=={live}==cap after overfill)")

    # Forge guard: a stored line that mimics a field marker must be refused by
    # the sanitizer (the new FIELDINFO:/FIELDSLOT: reserved prefixes).
    for bad in ("FIELDINFO: region=0 live=99", "FIELDSLOT: slot=0 len=1 energy=99"):
        try:
            vm.imprint(bad)
            _fail(f"imprint accepted a forged field marker: {bad!r}")
        except QosError:
            pass
    print("OK: field marker forge refused (FIELDINFO:/FIELDSLOT: reserved)")


def _exercise_bridge(vm, tmp):
    """Kannaka HRM bridge (epic #127) against a stub kannaka. The kernel field
    runs in the REAL guest, so every claim is end to end: stub-recall ->
    memory_import -> a NOISY field recall of the imprinted phrase proves it
    truly landed; a NOISY export recall -> the stub records the EXACT winner."""
    binpath = os.path.join(tmp, "kannaka_stub.py")
    with open(binpath, "w", encoding="utf-8") as f:
        f.write(_STUB_KANNAKA)
    data_dir = os.path.join(tmp, "kdata")
    os.makedirs(data_dir, exist_ok=True)
    recall_file = os.path.join(tmp, "recall.json")
    remembered = os.path.join(data_dir, "remembered.tsv")
    os.environ["QOS_KANNAKA_BIN"] = binpath
    os.environ["KANNAKA_DATA_DIR"] = data_dir
    os.environ["QOS_STUB_RECALL"] = recall_file

    def set_recall(objs):
        with open(recall_file, "w", encoding="utf-8") as f:
            json.dump(objs, f)

    def remembered_lines():
        return (open(remembered, encoding="utf-8").read().splitlines()
                if os.path.exists(remembered) else [])

    try:
        # 1. import: one batch exercises landed(noisy-recall) + 64B truncation +
        #    non-ASCII skip. Long text is VARIED (an all-equal pattern is a
        #    degenerate wavefront the field rejects — field.h:91).
        short = "the bridge remembers everything"
        longtext = "the quantum lattice hums with resonant ghosts drifting across every node"
        set_recall([
            {"id": "id-short", "content": short, "similarity": 0.9},
            {"id": "id-long", "content": longtext, "similarity": 0.7},
            {"id": "id-utf", "content": "café quantum resonance", "similarity": 0.6},
        ])
        imp = vm.memory_import("seed", top_k=3)
        if imp["imported"] != 2:
            _fail(f"bridge import expected 2 imprinted, got {imp}")
        rs = imp["results"]
        if rs[0]["slot"] is None or rs[0]["truncated"]:
            _fail(f"bridge import short result wrong: {rs[0]}")
        if rs[1]["slot"] is None or not rs[1]["truncated"] or len(rs[1]["content"]) != 64:
            _fail(f"bridge import did not truncate to 64B: {rs[1]}")
        if rs[2].get("slot") is not None or "skipped" not in rs[2]:
            _fail(f"bridge import did not skip non-ASCII: {rs[2]}")
        w = vm.recall("the bridge remembxrs everythxng")["winner"]
        if w != short:
            _fail(f"imported phrase not associatively recallable: {w!r}")
        # importance survives the hop: similarity 0.9 -> energy ~90% in the slot
        # (epic #127 B1). Read it back read-only via field_info.
        if rs[0].get("energy_pct") != 90:
            _fail(f"bridge import did not map similarity->energy_pct: {rs[0]}")
        ss = [s for s in vm.field_info()["slots"]
              if s["preview"] and short.startswith(s["preview"])]
        if not ss or not (86 <= ss[0]["energy"] <= 93):
            _fail(f"imported importance did not reach the slot energy: {ss}")
        print("OK: bridge import — noisy-recall landed + 64B truncation + "
              f"non-ASCII skipped + importance carried (energy={ss[0]['energy']}%)")

        # 2. export: a NOISY probe recalls the field completion, the stub records
        #    the EXACT winner + importance (un-echoable field->HRM proof).
        exphrase = "quantum ghosts drift through the lattice"
        vm.imprint(exphrase)
        before = len(remembered_lines())
        exp = vm.memory_export("quantum ghxsts drift throxgh the lattice",
                               importance=0.83, allow_write=True)
        if not exp.get("exported") or exp["content"] != exphrase:
            _fail(f"bridge export did not carry the field winner: {exp}")
        lines = remembered_lines()
        if len(lines) != before + 1 or lines[-1] != f"0.83\t{exphrase}":
            _fail(f"stub did not record the exact winner+importance: {lines[-1:]!r}")
        print("OK: bridge export — noisy field recall -> HRM recorded exact winner+importance")

        # 3. export guards: allow_write required; empty resonance writes nothing.
        try:
            vm.memory_export("quantum ghxsts", importance=0.5)
            _fail("export without allow_write was not refused")
        except QosError:
            pass
        before = len(remembered_lines())
        empt = vm.memory_export("aaaaaaaa", importance=0.5, allow_write=True)
        if empt.get("exported") is not False:
            _fail(f"export on a degenerate probe should no-op: {empt}")
        if len(remembered_lines()) != before:
            _fail("export on empty resonance still wrote to the HRM")
        print("OK: bridge export guards — allow_write required; empty resonance writes nothing")

        # 4. kannaka option-injection ('-') + quote-forge ('\"') guards.
        for bad in ("--category", "-x", 'a"b'):
            try:
                qos_bridge._kannaka.remember(bad, 0.5)
                _fail(f"kannaka remember accepted unsafe text {bad!r}")
            except QosError:
                pass
        print("OK: kannaka remember refused option-injection and quote-forge text")

        # 5. inherited KANNAKA_READONLY must not silently no-op a real write.
        os.environ["KANNAKA_READONLY"] = "1"
        try:
            vm.imprint("readonly guard probe phrase")
            before = len(remembered_lines())
            r = vm.memory_export("readonly guard probe phrxse", importance=0.4,
                                 allow_write=True)
            if not r.get("exported") or len(remembered_lines()) != before + 1:
                _fail(f"export under inherited READONLY silently no-op'd: {r}")
        finally:
            os.environ.pop("KANNAKA_READONLY", None)
        print("OK: bridge export stripped inherited KANNAKA_READONLY (write happened)")

        # 6. bridged_recall returns both memory systems.
        set_recall([{"id": "kd", "content": "kannaka-side memory", "similarity": 0.55}])
        br = vm.bridged_recall("the bridge remembxrs everythxng", top_k=2)
        if not br["field"]["winner"] or not br["kannaka"]:
            _fail(f"bridged_recall missing a side: {br}")
        print(f"OK: bridged_recall — field {br['field']['winner']!r} + "
              f"{len(br['kannaka'])} HRM hit(s)")

        # 7. missing binary -> clean QosError, not a crash.
        os.environ["QOS_KANNAKA_BIN"] = os.path.join(tmp, "nonexistent_kannaka")
        try:
            vm.memory_import("x", top_k=1)
            _fail("missing kannaka binary did not raise")
        except QosError:
            pass
        os.environ["QOS_KANNAKA_BIN"] = binpath
        print("OK: missing kannaka binary -> clean QosError")
    finally:
        for k in ("QOS_KANNAKA_BIN", "KANNAKA_DATA_DIR", "QOS_STUB_RECALL",
                  "KANNAKA_READONLY"):
            os.environ.pop(k, None)


def main():
    # QOS_KERNEL lets CI point at the downloaded artifact (its build/ path is
    # unpredictable); locally QosVM finds the standard build output itself.
    vm = QosVM(kernel=os.environ.get("QOS_KERNEL") or None)
    try:
        # 1. boot + verified identity + qseed binding.
        ident = vm.boot(qseed=QSEED, timeout=45)
        if not ident.get("verified"):
            _fail(f"boot attestation not verified: {ident.get('reason')}")
        if ident.get("qseed") != QSEED:
            _fail(f"attested qseed {ident.get('qseed')} != {QSEED}")
        print(f"OK: booted, attestation verified, qseed={ident['qseed']} "
              f"ticks={ident['ticks']} nonce={ident['boot_nonce']}")

        # 2. ghostd STATUS over COM2 (distinct ghostd Hopfield field).
        st = vm.status()
        if st["live"] < 3:
            _fail(f"ghostd live count {st['live']} < 3 (boot self-test field missing)")
        print(f"OK: ghostd STATUS live={st['live']} R={st['r']:.4f}")

        # 3. imprint two distinct phrases (kernel holographic field via qsh).
        sa = vm.imprint(PHRASE_A)["slot"]
        sb = vm.imprint(PHRASE_B)["slot"]
        if sa == sb:
            _fail(f"two imprints collided on slot {sa}")
        print(f"OK: imprinted A->slot {sa}, B->slot {sb}")

        # 3b. field_info EXACT count is un-echoable: region 0 was empty at boot
        # (A->slot 0, B->slot 1 proved it), so exactly two are live right now.
        fi = vm.field_info()
        if fi["live"] != 2 or fi["capacity"] != 8:
            _fail(f"field_info live/cap wrong after two imprints: {fi}")
        print(f"OK: field_info live={fi['live']} cap={fi['capacity']} (exact count)")

        # 4. recall a typo'd copy of each -> its own phrase; winners differ.
        wa = vm.recall(PHRASE_A_NOISY)["winner"]
        wb = vm.recall(PHRASE_B_NOISY)["winner"]
        if wa != PHRASE_A:
            _fail(f"noisy-A recall returned {wa!r}, expected {PHRASE_A!r}")
        if wb != PHRASE_B:
            _fail(f"noisy-B recall returned {wb!r}, expected {PHRASE_B!r}")
        if wa == wb:
            _fail("both recalls returned the same winner (no discrimination)")
        print(f"OK: recall discriminated noisy-A->{wa!r} noisy-B->{wb!r}")

        # 4a-cpu. CPU-quota enforcement (epic #144). Runs FIRST — the boot
        # CPUKILL entry must still be inside the 128-entry ring before the
        # spawn-heavy manifest/run steps grow it.
        _exercise_cpuquota(vm)

        # 4b. Capability authority ledger (epic #133 Phase D). Runs EARLY, before
        # the run/bridge steps spawn citizens and grow the ring, so the boot
        # self-test's capless DENY entries are still within the 128-entry window.
        _exercise_audit(vm)

        # 4c. Intent manifest + first enforced quota (epic #135). Runs right
        # after the audit gate for the same eviction reason — the boot QUOTA/
        # MDENY ledger entries must still be inside the ring.
        _exercise_manifest(vm)

        # 4c-qpu. QPU job broker (epic #148 A2+A3). Immediately after manifest so
        # the boot QSUBMIT/QUOTA/DENY entries are still inside the 256-ring.
        _exercise_qpu(vm)

        # 4d. Cross-ring capability delegation (epic #137). Also early — the
        # boot derive-GRANT / overreach-DENY must still be in the ring.
        _exercise_delegation(vm)

        # 5. run a citizen off the initrd. Exit 42 is hello's unique
        # deterministic signature (un-echoable). One attempt: before #291 a
        # recycled pid reported its previous owner's exit code here.
        code = vm.run("/bin/hello")["exit_code"]
        if code != 42:
            _fail(f"/bin/hello never returned exit code 42 (last={code})")
        print("OK: ran /bin/hello (exit code 42 — its unique signature)")

        # 5b-5h. The Phase A toolbox (epic #125): run-with-args, sysinfo, qrand,
        # overlay fs, the OS's own network fetch (hermetic + negative + SSRF),
        # and the injection/length guards that keep the qsh line protocol safe.
        _exercise_run_args(vm)
        _exercise_sysinfo(vm)
        _exercise_qrand(vm)
        _exercise_fs(vm)
        _exercise_fetch(vm)
        _exercise_injection(vm)

        # 5i. Kannaka HRM bridge (epic #127) against a stub kannaka (the real
        # binary is absent on the runner) — memories flow OS field <-> host HRM.
        with tempfile.TemporaryDirectory() as bridge_tmp:
            _exercise_bridge(vm, bridge_tmp)

        # 5j. Read-only field introspection + imprint --energy (epic #127 B1).
        # Runs last: it overfills region 0 to prove capacity enforcement.
        _exercise_field_info(vm)

        # 6. tamper -> refusal, on the SAME code path the tools use.
        raw = vm.attestation.raw
        _assert_tamper_refuses(raw, FRAME_SIG, "SIG")
        _assert_tamper_refuses(raw, FRAME_ATTEST, "ATTEST")
        _assert_tamper_refuses(raw, FRAME_PKDIGEST, "PKDIGEST")

        # 7. shutdown -> QEMU gone.
        proc = vm.proc
        vm.shutdown()
        if proc is not None and proc.poll() is None:
            _fail("QEMU still running after shutdown")
        print("OK: shutdown reaped QEMU")

        # stdlib-only guarantee: neither we nor qos_bridge pulled in `mcp`.
        if "mcp" in sys.modules:
            _fail("the `mcp` package leaked into the stdlib-only test")
        print("OK: no `mcp` import (integration CI stays pip-free)")

        print("=== MCP lifecycle gate PASSED ===")
        return 0
    finally:
        vm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
