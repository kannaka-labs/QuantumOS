#!/usr/bin/env python3
"""
QuantumOS MCP server (epics #99, #125): expose a running QuantumOS to agents
as tools. Any MCP-speaking agent can boot a VM; query its ghostd field; imprint
and recall associative memories at the kernel field; inspect that field
read-only (live slots, energy, eviction); run a citizen off the initrd (with
arguments); read a structured machine snapshot (uptime, memory, process, date); draw quantum-seeded entropy; write/rm/sync overlay files;
have the OS fetch a URL over its own TCP stack; bridge memories to/from the host
Kannaka HRM (epic #127); and shut it down — every result carrying the boot's
Lamport-verified identity.

This is a THIN wrapper: all logic (framing, attestation, the QEMU lifecycle,
the verified-gate) lives in qos_bridge.QosVM. The tools here are one-line
delegations, so the CI test in test_qos_mcp.py exercises the very same code
path these tools take. Only this file imports `mcp`; qos_bridge stays
stdlib-only so the integration CI (no mcp package) can test the bridge.

HONESTY: `identity()` in every result documents that `verified` means the
boot attestation's frames + Lamport signature + qseed are consistent — NOT
that the VM is live-now (a captured attestation replays). Individual DATA
replies are unsigned by default, EXCEPT when a session key is admitted
(admit_key): the STATUS reply is then nonce+HMAC authenticated per request and
verified fresh (QosVM.status_authenticated, ADR-0019 Extension) — RECALL/QSUBMIT
remain unsigned (deferred). Absent that, trust equals control of the VM's serial
channel, the same class as holding the console — do not present an unauthenticated
tool result as cryptographically attested data.

Run:  python3 scripts/qos_mcp.py       (stdio transport)
Dep:  mcp>=1.0  (Python >=3.10)
"""

from mcp.server.fastmcp import FastMCP

from qos_bridge import QosVM, QosSociety, QosError

mcp = FastMCP("quantumos")
_vm = QosVM()
_society = QosSociety()


def _err(exc):
    """Structured error for the single-VM tools, carrying that VM's attested
    identity (see the module docstring for what 'verified' honestly means)."""
    return {"error": str(exc), "identity": _vm.identity()}


def _society_err(exc):
    """Structured error for the SOCIETY tools. A society has no single attested
    identity (it is N members), so — unlike _err — this does NOT borrow the
    standalone _vm's identity, which would misattribute a society failure to an
    unrelated (often unbooted) single VM."""
    return {"error": str(exc), "society": True}


@mcp.tool()
def qos_boot(qseed: str = "", kernel: str = "") -> dict:
    """Boot a QuantumOS VM under QEMU and capture its Lamport boot attestation.

    qseed: optional hex quantum seed folded into the attested identity (the
    boot is deterministic under a fixed qseed). kernel: optional path to a
    kernel.elf32 (defaults to the repo build). One VM at a time — call
    qos_shutdown first if one is already running. Returns the verified boot
    identity (see the module docstring for what 'verified' honestly means)."""
    global _vm
    if _vm.is_running():
        return {"error": "a VM is already running — call qos_shutdown first",
                "identity": _vm.identity()}
    _vm = QosVM(kernel=kernel or None)
    try:
        ident = _vm.boot(qseed=qseed or None)
    except QosError as exc:
        return _err(exc)
    # Attest by DEFAULT (ADR-0019 Extension): admit a fresh host session key so
    # every subsequent tool RESULT is nonce+HMAC fresh, not merely boot-verified.
    # Degraded (not fatal) if it can't go live — the plain tools still work, but
    # the flag stays honest rather than claiming attestation we don't have.
    try:
        _vm.attest()
        attested = True
    except QosError:
        attested = False
    return {"booted": True, "identity": ident, "attested": attested}


@mcp.tool()
def qos_status() -> dict:
    """Query ghostd's field over the attested COM2 bridge: the order parameter
    R and the count of live imprinted patterns. Refuses if the boot attestation
    is not verified. When the session is attested (qos_boot admits a key by
    default), the reply is nonce+HMAC verified FRESH per request (ADR-0019
    Extension), not merely boot-verified — surfaced as `attested`. Returns a
    uniform {r, live, attested, identity}."""
    try:
        if _vm._session_key is not None:
            s = _vm.status_authenticated()
            s["attested"] = s.pop("authenticated", True)
            return s
        s = _vm.status()
        s["attested"] = False
        return s
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_imprint(text: str) -> dict:
    """Imprint `text` into the kernel holographic field (the same associative
    memory a human reaches with the qsh `imprint` command). Returns the
    assigned slot. Refuses if the attestation is not verified."""
    try:
        return _vm.imprint(text)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_recall(text: str) -> dict:
    """Recall from the kernel holographic field with a (possibly corrupted)
    probe `text`; returns the winning stored text, its slot and score. Refuses
    if the attestation is not verified."""
    try:
        return _vm.recall(text)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_field_status() -> dict:
    """Inspect the kernel holographic field READ-ONLY (epic #127 B1): live slot
    count, capacity, and per-slot metadata — slot index, length, energy (% of
    max, the stored importance), effective energy (% after age decay),
    retrievals, age, and a bounded preview. Unlike a recall this does NOT
    reinforce the field, so repeated calls do not perturb it. This is a real
    live count (not saturated) and content-addressable capacity/eviction
    visibility. Refuses if the attestation is not verified."""
    try:
        return _vm.field_info()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_audit() -> dict:
    """Read the kernel capability AUTHORITY LEDGER (epic #133): the
    kernel-recorded GRANT / DENY / SPAWN events — what authority each citizen was
    granted and every capability-gated syscall it was refused. The KERNEL records
    it, so a citizen cannot forge or suppress its own entry: this makes 'authority
    IS the capability set, and every attempt to exceed it is provable' observable
    (conscience before wallet). Returns {total, dropped, capacity, entries}. It is
    an authority ledger, NOT a full action trace. Refuses if not verified."""
    try:
        return _vm.audit()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_manifest() -> dict:
    """Read the per-pid INTENT MANIFESTS (epic #135 Phase D increment 2): every
    bound citizen's DECLARED intent — the allow-set of resources it may touch, its
    spawn quota (used/max, where * = unlimited), and CPU ticks consumed. This is a
    policy layer ABOVE raw capabilities: the KERNEL builds each manifest from the
    same grants that mint the caps and enforces it at the capability-gated syscall
    sites plus the first real quota (spawn), so a citizen cannot forge its own row.
    Returns {truncated, manifests:{pid:{bound, spawn_used, spawn_max, cpu_ticks,
    allow:[{res,id,perms}]}}}. Makes declared intent inspectable and the spawn
    quota's enforcement observable. Refuses if the attestation is not verified."""
    try:
        return _vm.manifest()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_run(program: str, args: str = "") -> dict:
    """Spawn a citizen off the initrd (e.g. '/bin/hello' or '/bin/args a b c')
    via qsh and return its console output and exit code. `program` must be
    /bin/<name>; `args` is an optional argument string (validated — no control
    bytes, bounded to the qsh line, no result-marker impersonation). Refuses if
    the attestation is not verified."""
    try:
        return _vm.run(program, args=args)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_sysinfo() -> dict:
    """One structured snapshot of the running machine: uptime (ticks + seconds),
    memory (heap-free bytes, free/total frames), the live process table
    (pid/name/state), and the RTC date. Refuses if not verified."""
    try:
        return _vm.sysinfo()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_qrand() -> dict:
    """Draw 64 bits of quantum-seeded entropy from the guest (SYS_QRAND,
    capability-gated in-kernel). Returns lowercase hex. Refuses if not
    verified."""
    try:
        return _vm.qrand()
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_qpu_submit(kind: str = "bell", n_qubits: int = 3, target: int = 0,
                   iters: int = 1, probe: int = 0) -> dict:
    """Submit a quantum circuit to the in-OS QPU broker (SYS_QPU, epic #148) over
    the attested COM2 bridge and get the EXACT integer result back. `kind` is
    'bell' | 'ghz' | 'grover' (grover uses n_qubits/target/iters; bell/ghz use
    n_qubits/probe). This is the conscience-before-wallet path made agent-facing:
    the broker is capability-gated (only swarm_svc holds the submit cap; a capless
    submit is EPERM), quota-bounded (qsub_max in swarm_svc's intent manifest, e.g.
    a 6th submit is refused), and every accepted job is recorded in the kernel
    authority ledger (AUDIT_QSUBMIT) — a capability-gated, quota-metered, provable
    quantum job. The kernel NEVER parses the circuit (opaque payload); qpud runs it
    on the exact Gaussian-integer engine. Returns {kind, status, ok, probability,
    prob_num, prob_den, n_qubits, ...}; status 0=OK / 1=quota-refused /
    2=broker-rejected(EINVAL). Refuses if the boot attestation is not verified."""
    try:
        return _vm.qpu_run(kind=kind, n_qubits=n_qubits, target=target,
                           iters=iters, probe=probe)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_fetch(host: str, port: int = 80, allow_private: bool = False) -> dict:
    """Have QuantumOS fetch http://host:port/ over its own TCP stack; returns
    the status line, byte count, and whether the transfer completed.

    This reaches the HOST network namespace via SLIRP NAT (loopback, DNS, and
    outbound internet are live), NOT a sandbox — so loopback/RFC1918 targets
    are refused by default (SSRF guard); set allow_private=True to override.
    The response BODY is not returned (qsh's `http` prints only the status line
    and byte count). Refuses if the attestation is not verified."""
    try:
        return _vm.fetch(host, port=port, allow_private=allow_private)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_fs(op: str, path: str = "", text: str = "") -> dict:
    """Overlay-filesystem operation over qsh. op is 'write' (path + text),
    'rm' (path), or 'sync' (flush the RAM overlay to disk). Paths and text are
    validated before they reach the qsh line. read/ls are not offered yet —
    their console output is not machine-framed (a filed guest follow-up).
    Refuses if the attestation is not verified."""
    try:
        return _vm.fs(op, path=path, text=text)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_memory_import(query: str, top_k: int = 5) -> dict:
    """Recall the top-k memories for `query` from the host Kannaka HRM and
    imprint each into the QuantumOS kernel field — seeding the OS's associative
    memory from the host memory system. Content is truncated to the 64-byte
    field slot (reported) and non-ASCII memories are skipped (the field is
    ASCII-only); each memory's Kannaka similarity IS carried into its slot energy
    (mapped to `imprint --energy` 1..100), and each result reports its
    `energy_pct`. Requires kannaka.exe (QOS_KANNAKA_BIN). Refuses if the
    attestation is not verified."""
    try:
        return _vm.memory_import(query, top_k=top_k)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_memory_export(probe: str, importance: float = 0.6,
                      allow_write: bool = False) -> dict:
    """Recall the field completion for `probe` and persist it to the host
    Kannaka HRM. REFUSES unless allow_write=True: this WRITES the user's real
    ~/.kannaka memory (or KANNAKA_DATA_DIR) with field-derived content, and it
    REINFORCES the recalled field slot (not a pure read). `importance` is a
    caller constant. Refuses if the attestation is not verified."""
    try:
        return _vm.memory_export(probe, importance=importance, allow_write=allow_write)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_memory_bridged_recall(query: str, top_k: int = 3) -> dict:
    """Query BOTH memory systems on the same cue: the QuantumOS field completion
    (resonance = cosine x energy) and the Kannaka HRM's top-k. The field recall
    REINFORCES its winner, and field_score_q15 is cosine x energy in Q15 (NOT a
    bare similarity). Requires kannaka.exe. Refuses if not verified."""
    try:
        return _vm.bridged_recall(query, top_k=top_k)
    except QosError as exc:
        return _err(exc)


@mcp.tool()
def qos_society_boot(qseed_a: str, qseed_b: str) -> dict:
    """Boot a SOCIETY of two attested QuantumOS VMs coupled into one holographic
    field (epic #131). Each member boots with a static IP and a peer link; their
    ghostd services couple their fields over UDP and the R_x cross-order
    parameter climbs to synchronization. qseed_a must differ from qseed_b.
    Returns each member's attested identity + status.

    HONEST TRUST NOTE: attestation and coupling are cryptographically UNLINKED —
    a verified 'synchronized' proves two fields coupled on the loopback L2, NOT
    that the two attested identities coupled with each other. Trust == control of
    the host. One society at a time — shut it down first."""
    global _society
    if _society.is_running():
        return {"error": "a society is already running — call qos_society_shutdown first"}
    _society = QosSociety()
    try:
        return {"booted": True, "status": _society.boot(qseed_a, qseed_b)}
    except QosError as exc:
        return _society_err(exc)


@mcp.tool()
def qos_society_boot_n(qseeds: list[str]) -> dict:
    """Boot an N-WAY society (3..4 members) into ONE mean-field coupled over a
    shared multicast L2 (epic #139). Every qseed must be DISTINCT — each member
    seeds its ghostd field divergently, so the cross-node order parameter starts
    low and can only climb because the wire carries the peers' phases. Each node
    folds the circular MEAN field over its N-1 peers; the sync verdict is the
    MINIMUM pairwise R_x, so a partial lock cannot masquerade as full. All N
    reaching that threshold is mean-field convergence a 2-VM society (which can
    only pairwise-lock) structurally cannot fake. Returns each member's attested
    identity + status.

    Same HONEST TRUST NOTE as the 2-VM society: coupling and attestation are
    cryptographically UNLINKED — a synchronized society proves N fields coupled
    on the shared loopback L2, not that N attested identities coupled; a forged
    source can skew (never starve) the mean. Trust == control of the host. One
    society at a time — shut it down first."""
    global _society
    if _society.is_running():
        return {"error": "a society is already running — call qos_society_shutdown first"}
    _society = QosSociety()
    try:
        return {"booted": True, "status": _society.boot_n(list(qseeds))}
    except QosError as exc:
        return _society_err(exc)


@mcp.tool()
def qos_society_status() -> dict:
    """Per-member status of the running society: each node's verified identity,
    latest and minimum R_x, and whether it has synchronized. Works for both the
    2-VM society and an N-way society (epic #139)."""
    try:
        return _society.status_n() if _society.members else _society.status()
    except QosError as exc:
        return _society_err(exc)


@mcp.tool()
def qos_society_await_sync(threshold: float = 0.80) -> dict:
    """Block until EVERY society member's field synchronizes (R_x >= threshold),
    or a member dies, or a deadline passes. Returns the final per-member status
    (with the minimum R_x seen, showing the divergent start). Works for both the
    2-VM society and an N-way society (epic #139: the N-way verdict is the
    minimum pairwise R_x per node)."""
    try:
        if _society.members:
            return _society.await_sync_n(threshold=threshold)
        return _society.await_sync(threshold=threshold)
    except QosError as exc:
        return _society_err(exc)


@mcp.tool()
def qos_society_shutdown() -> dict:
    """Power off both society members. Idempotent."""
    return _society.shutdown()


@mcp.tool()
def qos_shutdown() -> dict:
    """Power off the running VM (SIGTERM -> SIGKILL of QEMU; qsh `exit` only
    reboots the shell, never the machine). Idempotent."""
    return _vm.shutdown()


def main(argv=None) -> None:
    """Console-script entry point (``qos-mcp``): serve the MCP tools over stdio.

    With no arguments it speaks MCP on stdin/stdout, which is what a client's
    ``command`` entry expects. ``--help`` and ``--version`` answer and exit, so
    typing the command by hand does not leave a silent process waiting on stdin.
    """
    import argparse
    import sys as _sys
    ap = argparse.ArgumentParser(prog="qos-mcp", description="QuantumOS as MCP tools: boot the kernel under QEMU and drive its shell, field, ledgers and society from an agent. Environment: QOS_KERNEL (kernel.elf), QOS_KANNAKA_BIN (the kannaka CLI for the Kannaka tools). No arguments: serve over stdio.")
    ap.add_argument("--version", action="store_true", help="print the quantumos-host-tools version and exit")
    a = ap.parse_args(argv if argv is not None else _sys.argv[1:])
    if a.version:
        try:
            from importlib.metadata import version as _v
            print(_v("quantumos-host-tools"))
        except Exception:  # noqa: BLE001 - a checkout without the package installed
            print("dev")
        return
    mcp.run()


if __name__ == "__main__":
    main()
