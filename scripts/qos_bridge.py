#!/usr/bin/env python3
"""
QuantumOS host-side bridge (epic #99): the ONE implementation of the COM2
swarm-bridge wire protocol, the Lamport boot-attestation verifier, and the
QEMU VM lifecycle manager that the MCP server (scripts/qos_mcp.py) and the
CI test (scripts/test_qos_mcp.py) are built on. verify_attestation.py and
swarm_pingpong.py import their framing/verify from here, so there is exactly
one CRC8/frame/verify implementation — no wire drift.

Standard library only (hashlib, socket, subprocess, threading, ...): the
integration-CI python has no `mcp` package, and this module must never import
it. `mcp` lives only in qos_mcp.py.

Wire framing (little-endian), mirrored from user/swarm.h:
    0xA5 | type:u8 | len:u16 | payload[len] | crc8
CRC-8/CCITT (poly 0x07, init 0x00, MSB-first) over type+len+payload.

TRUST BOUNDARY (read before believing `verified`): the Lamport signature
covers ONLY the boot ATTEST string. It proves "a QuantumOS boot with this
qseed once produced this attestation", NOT liveness (a captured attestation
replays — the wire protocol carries no host challenge/nonce) and NOT the
authenticity of any later STATUS/RECALL reply (DATA frames are unsigned).
Trust therefore equals control of the serial channel — the same class of
trust as holding the console. identity() says so; the MCP tool docstrings
repeat it.
"""

import atexit
import ctypes
import hashlib
import hmac
import json
import os
import platform
import random
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid

# ADR-0030: the grant every harness VM boots with. Wide (any destination) because the
# harness tests the stack and the citizens, not egress policy; still budgeted and receipted.
EGRESS_WIDE = "egress=ttl=86400;bytes=4294967296;allow=*;listen=4747,8080,502,4000"

# ---- wire constants (user/swarm.h) ----------------------------------------
# These module constants ARE the host ring of the ADR-0020 wire freeze:
# scripts/extract-wire.py imports this module and twins each value against the
# compiler-measured guest probe (user/wire_probe.c), so an edit on either side
# is a contracts/wire/v1.golden diff, never a silent protocol split.
MAGIC = 0xA5
HDR_LEN = 4                          # magic + type + len(2) (SWARM_HDR_LEN)
SWARM_MAX_PAYLOAD = 512

FRAME_HANDSHAKE = 0x01
FRAME_DATA = 0x02
FRAME_PING = 0x03
FRAME_PONG = 0x04
FRAME_DISCONNECT = 0x05
FRAME_PKDIGEST = 0x10
FRAME_ATTEST = 0x11
FRAME_SIG = 0x12

SWARM_OP_STATUS = 0x01
SWARM_OP_RECALL = 0x02
SWARM_OP_QSUBMIT = 0x03  # host submits an opaque circuit to the SYS_QPU broker (epic #149 B1)
SWARM_OP_KEY = 0x04      # host admits the swarm-plane group session key (ADR-0019)

CRC8_POLY = 0x07                     # CRC-8/CCITT polynomial (SWARM_CRC8_POLY)
CRC8_INIT = 0x00                     # CRC-8/CCITT init value (SWARM_CRC8_INIT)

# ---- attestation string pieces (user/swarm.h SWARM_ATTEST_*) ---------------
# parse_attestation() splits on these; the guest composes with the same bytes.
ATTEST_MAGIC = "QOS-BOOT"
ATTEST_QSEED_KEY = "qseed="
ATTEST_TICKS_KEY = "ticks="

# ---- reply-auth + DATA reply body geometry (user/swarm.h, ADR-0019) --------
REPLYAUTH_NONCE_LEN = 16             # per-request host nonce, echoed in the reply
REPLYAUTH_TAG_LEN = 32               # HMAC-SHA256 tag after the nonce echo
KEY_LEN = 32                         # swarm-plane group session key (SWARM_OP_KEY)
STATUS_BODY_LEN = 6                  # op + r_q16(4) + live
RECALL_BODY_LEN = 6                  # op + match + r_q16(4)
QSUBMIT_BODY_ERR = 2                 # op + status (error reply)
QSUBMIT_BODY_OK = 22                 # op + status + QC_RESULT_LEN(20) result
STATUS_R_SCALE = 65536               # r_q16 is a Q16 fraction

# ---- Lamport parameters (user/swarm.h) ------------------------------------
LAMPORT_BITS = 256
HASH_LEN = 32
SIG_ELEM = 64                        # revealed preimage (32) + comp pk hash (32)
SIG_LEN = LAMPORT_BITS * SIG_ELEM    # 16384


# ============================================================================
# Framing
# ============================================================================
def crc8(data: bytes) -> int:
    """CRC-8/CCITT (poly 0x07, init 0x00, MSB-first) — matches swarm_crc8()."""
    crc = CRC8_INIT
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ CRC8_POLY) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def frame(ftype: int, payload: bytes) -> bytes:
    """Build one wire frame."""
    hdr = bytes([ftype, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload
    return bytes([MAGIC]) + hdr + bytes([crc8(hdr)])


class FrameParser:
    """Streaming frame extractor for a long-lived COM2 stream.

    Resyncs on a non-magic byte or a bad CRC by dropping one byte (matching
    swarm_pingpong.parse and the guest's poll_com2). Two hardening rules the
    persistent-client case needs that the one-shot scripts lacked:

      * `bad_crc` is counted (verify_attestation's gate hard-fails on it), and
      * a length field > SWARM_MAX_PAYLOAD is rejected by dropping the magic
        byte and resyncing (the guest's rule, swarm_svc.c). Without it a single
        flipped length byte makes the parser wait forever for len+5 bytes that
        never come, wedging every subsequent reply until VM reboot.
    """

    def __init__(self):
        self.buf = bytearray()
        self.bad_crc = 0
        self.bad_len = 0

    def feed(self, data: bytes):
        """Append bytes; return a list of (type, payload) for complete frames."""
        self.buf.extend(data)
        out = []
        while True:
            while self.buf and self.buf[0] != MAGIC:
                del self.buf[0]
            if len(self.buf) < HDR_LEN:
                return out
            length = self.buf[2] | (self.buf[3] << 8)
            if length > SWARM_MAX_PAYLOAD:
                self.bad_len += 1
                del self.buf[0]      # impossible length: resync past this magic
                continue
            total = HDR_LEN + length + 1
            if len(self.buf) < total:
                return out           # frame not fully arrived yet
            if crc8(bytes(self.buf[1:HDR_LEN + length])) == self.buf[HDR_LEN + length]:
                out.append((self.buf[1], bytes(self.buf[HDR_LEN:HDR_LEN + length])))
                del self.buf[:total]
            else:
                self.bad_crc += 1
                del self.buf[0]      # resync past this magic


def parse_frames(blob: bytes):
    """Whole-blob variant used by verify_attestation's CLI.

    Returns (frames, bad_crc). Behaviour on a clean capture is identical to
    the historical parse_frames; it additionally resyncs past an impossible
    length instead of truncating the rest of the stream.
    """
    p = FrameParser()
    frames = p.feed(blob)
    return frames, p.bad_crc


# ============================================================================
# Lamport attestation verification
# ============================================================================
def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def verify_lamport(message: bytes, pkdigest: bytes, signature: bytes) -> bool:
    """Rebuild the public key from the signature and check it against the
    committed digest. One digest equation catches a tampered signature,
    message, or committed digest alike."""
    if len(signature) != SIG_LEN:
        return False
    if len(pkdigest) != HASH_LEN:
        return False
    md = sha256(message)
    pk_stream = bytearray()
    for i in range(LAMPORT_BITS):
        bit = (md[i >> 3] >> (i & 7)) & 1
        elem = signature[i * SIG_ELEM:(i + 1) * SIG_ELEM]
        preimage = elem[:HASH_LEN]
        comp = elem[HASH_LEN:]
        pk = [None, None]
        pk[bit] = sha256(preimage)   # pk[i][bit]
        pk[1 - bit] = comp           # pk[i][1-bit], revealed in the signature
        pk_stream += pk[0]
        pk_stream += pk[1]
    return sha256(bytes(pk_stream)) == pkdigest


def parse_attestation(msg: str):
    """Parse 'QOS-BOOT|qseed=<hex|none>|ticks=<n>' -> (qseed_or_None, ticks)."""
    parts = msg.split("|")
    if len(parts) != 3 or parts[0] != ATTEST_MAGIC:
        raise ValueError(f"malformed attestation: {msg!r}")
    if not parts[1].startswith(ATTEST_QSEED_KEY) or not parts[2].startswith(ATTEST_TICKS_KEY):
        raise ValueError(f"malformed attestation fields: {msg!r}")
    qseed = parts[1][len(ATTEST_QSEED_KEY):]
    ticks = parts[2][len(ATTEST_TICKS_KEY):]
    qseed_val = None if qseed == "none" else int(qseed, 16)
    return qseed_val, int(ticks)


def expected_qseed(arg):
    """Normalise a requested qseed (hex string, 'none', or None) to int|None."""
    if arg is None:
        return None
    if isinstance(arg, int):
        return arg
    return None if arg.lower() == "none" else int(arg, 16)


class Attestation:
    """The verified (or refused) boot identity of one VM."""

    def __init__(self, verified, reason, qseed, ticks, pkdigest, raw):
        self.verified = verified
        self.reason = reason
        self.qseed = qseed          # int | None
        self.ticks = ticks
        self.pkdigest = pkdigest    # bytes | None
        self.raw = raw              # the captured attestation bytes

    def qseed_hex(self):
        return "none" if self.qseed is None else format(self.qseed, "X")

    def to_dict(self, boot_nonce=None):
        return {
            "verified": self.verified,
            "reason": self.reason,
            "qseed": self.qseed_hex(),
            "ticks": self.ticks,
            "pkdigest": self.pkdigest.hex() if self.pkdigest else None,
            "boot_nonce": boot_nonce,
            "trust_note": (
                "verified == attestation frames intact + Lamport signature "
                "consistent + attested qseed matches request. NOT liveness "
                "(a captured attestation replays; the wire protocol carries no "
                "host nonce) and NOT per-operation authentication (STATUS/"
                "RECALL replies are unsigned). Trust == control of the serial "
                "channel, same class as the console."
            ),
        }


def attestation_from_bytes(blob: bytes, requested_qseed=None) -> Attestation:
    """Parse+verify a full captured attestation byte stream. This is the SAME
    code path boot() uses, so the CI tamper test can flip a byte in a captured
    blob and get the identical verdict the live tools would."""
    frames, bad_crc = parse_frames(blob)
    if bad_crc:
        return Attestation(False, f"{bad_crc} frame(s) with bad CRC8", None, None, None, blob)

    pkdigest = None
    attest_msg = None
    sig = bytearray()
    for ftype, payload in frames:
        if ftype == FRAME_PKDIGEST:
            pkdigest = bytes(payload)
        elif ftype == FRAME_ATTEST:
            attest_msg = payload.decode("ascii", errors="replace")
        elif ftype == FRAME_SIG:
            sig += payload

    if pkdigest is None or attest_msg is None or not sig:
        return Attestation(False, "missing pk-digest, attestation, or signature",
                           None, None, pkdigest, blob)
    try:
        qseed_val, ticks = parse_attestation(attest_msg)
    except ValueError as exc:
        return Attestation(False, str(exc), None, None, pkdigest, blob)

    if not verify_lamport(attest_msg.encode("ascii"), pkdigest, bytes(sig)):
        return Attestation(False, "Lamport signature invalid", qseed_val, ticks, pkdigest, blob)

    want = expected_qseed(requested_qseed)
    if qseed_val != want:
        got = "none" if qseed_val is None else format(qseed_val, "X")
        exp = "none" if want is None else format(want, "X")
        return Attestation(False, f"attested qseed {got} != requested {exp}",
                           qseed_val, ticks, pkdigest, blob)

    return Attestation(True, "ok", qseed_val, ticks, pkdigest, blob)


def read_boot_attestation(recv_fn, deadline, requested_qseed=None) -> Attestation:
    """Accumulate COM2 bytes via recv_fn() until the SIG stream is complete (or
    the deadline passes), then verify. recv_fn() returns bytes, None when
    nothing arrived before its own timeout, or b'' on EOF (VM gone)."""
    parser = FrameParser()
    raw = bytearray()
    have_pk = have_attest = False
    sig_len = 0
    while time.time() < deadline:
        chunk = recv_fn()
        if chunk is None:
            continue
        if chunk == b"":
            break
        raw.extend(chunk)
        for ftype, payload in parser.feed(chunk):
            if ftype == FRAME_PKDIGEST:
                have_pk = True
            elif ftype == FRAME_ATTEST:
                have_attest = True
            elif ftype == FRAME_SIG:
                sig_len += len(payload)
        if have_pk and have_attest and sig_len >= SIG_LEN:
            break
    return attestation_from_bytes(bytes(raw), requested_qseed)


# ============================================================================
# QosVM — the QEMU lifecycle + bridge client
# ============================================================================
class QosError(Exception):
    """Base for structured tool failures (serialized by qos_mcp)."""


class QosRefused(QosError):
    """The VM's attestation is not verified — field/run tools refuse."""


class QosDead(QosError):
    """The VM process has exited."""


class QosTimeout(QosError):
    """A bridge reply or qsh marker did not arrive in time."""


# ============================================================================
# qsh input validation — the console is a line protocol
# ============================================================================
# qsh's line editor (user/qsh.c:1392) accepts ONLY printable ASCII and silently
# DROPS bytes past LINE_MAX-1 = 119, then executes the truncated PREFIX on Enter
# (qsh.c:1377). So a control byte (\n / \r) in an argument would run a SECOND
# command, and an over-long argument would run a DIFFERENT command. Every value
# spliced into a qsh line therefore passes through _qsh_text/_qsh_path first.
# The byte budget is security-critical, not cosmetic. Rejecting the reserved
# marker prefixes as well stops agent-controlled text from impersonating a
# result line (the host parses those prefixes as ground truth).
QSH_LINE_MAX = 119
# 'FIELDINFO:'/'FIELDSLOT:' are NOT covered by 'FIELD:' — 'FIELDINFO:'.startswith
# ('FIELD:') is False — so they must be listed explicitly or stored content could
# forge a field-info line (epic #127 B1).
_QSH_MARKERS = ("qsh:", "FIELD:", "FIELDINFO:", "FIELDSLOT:", "AUDIT:", "MANIFEST:", "PS:",
                "MEM:", "TIME:")


def _qsh_text(value, budget, what="argument"):
    """Validate one argument destined for a qsh command line. Returns it
    unchanged or raises QosError. `budget` is the byte room left on the line
    after the fixed command prefix (spaces included). The charset check runs
    FIRST so the length check can encode as ASCII without failing, and so
    byte-length == char-length holds against qsh's 119-byte line cap."""
    if not isinstance(value, str):
        raise QosError(f"{what} must be a string")
    for ch in value:
        o = ord(ch)
        if o < 0x20 or o > 0x7E:
            raise QosError(
                f"{what} contains non-printable byte 0x{o:02X}; only ASCII "
                f"0x20-0x7E is legal on the qsh line (a control byte would "
                f"inject a second command)")
    n = len(value.encode("ascii"))
    if n > budget:
        raise QosError(
            f"{what} too long: {n} > {budget} bytes; qsh drops input past "
            f"{QSH_LINE_MAX} and would execute a truncated, different command")
    stripped = value.lstrip()
    for mk in _QSH_MARKERS:
        if stripped.startswith(mk):
            raise QosError(
                f"{what} may not begin with the reserved result marker {mk!r}")
    return value


_QSH_PATH = re.compile(r"^/?[A-Za-z0-9._][A-Za-z0-9._/\-]{0,62}$")


def _qsh_path(value):
    """Validate a filesystem path argument: printable, bounded, no space (qsh
    splits the command on the first space), no `..` traversal past the flat
    ramfs/initrd namespace (kernel/src/ramfs.c:30-37 strips ./ and leading /)."""
    _qsh_text(value, QSH_LINE_MAX, "path")
    if " " in value or ".." in value:
        raise QosError(f"invalid path {value!r} (no spaces or '..')")
    if not _QSH_PATH.match(value):
        raise QosError(f"invalid path {value!r}")
    return value


_FETCH_HOST = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.\-]{0,62}$")


def _is_private_target(host):
    """True for loopback / RFC1918 / link-local / SLIRP-host targets. qos_fetch
    refuses these by default: adding -netdev to every boot gives the guest the
    HOST network namespace via SLIRP NAT (10.0.2.2 == host 127.0.0.1), so an
    unguarded fetch is a blind-SSRF primitive into host-local services."""
    if host.lower() == "localhost":
        return True
    parts = host.split(".")
    if len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts):
        a, b = int(parts[0]), int(parts[1])
        return (a in (0, 10, 127)
                or (a == 172 and 16 <= b <= 31)
                or (a == 192 and b == 168)
                or (a == 169 and b == 254))
    return False


def _repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _default_kernel():
    """The standard build path, or — if the artifact landed elsewhere (the CI
    integration job unpacks it under an unpredictable build/ subdir) — the
    first kernel.elf32 found under build/. QOS_KERNEL names one explicitly, which an installed
    package needs: it has no build/ (download the kernel from a release and point this at it)."""
    if os.environ.get("QOS_KERNEL"):
        return os.path.expanduser(os.environ["QOS_KERNEL"])
    root = _repo_root()
    std = os.path.join(root, "build", "x86_64", "kernel.elf32")
    if os.path.exists(std):
        return std
    build = os.path.join(root, "build")
    for dirpath, _dirs, files in os.walk(build):
        if "kernel.elf32" in files:
            return os.path.join(dirpath, "kernel.elf32")
    return std  # not found — boot() raises a clear error naming this path


QSH_BANNER = "QSH: QuantumOS interactive shell ready"
_ALLOWED_RUN = re.compile(r"^/bin/[A-Za-z0-9_]+$")
_UNSET = object()  # boot(expect_qseed=...) sentinel: distinguish "default" from None


# ============================================================================
# Kannaka HRM bridge (epic #127): the QuantumOS kernel field is "kannaka-
# memory's essence, ported" (kernel/include/kernel/field.h). This shells out
# to the kannaka.exe CLI to move memories between the two — imprint a Kannaka
# recall's results into the field, or export a field completion back into the
# HRM. Kannaka has NO daemon; the CLI is the only interface.
# ============================================================================
FIELD_PAT_MAX = 64                     # kernel field slot capacity (field.h:36)
_UUID_RE = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                      r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")
_kannaka_lock = threading.Lock()       # serialize this process's kannaka calls


def _default_kannaka_bin():
    """QOS_KANNAKA_BIN overrides (the CI gate points it at a stub); otherwise `kannaka` on PATH,
    otherwise the standard release build (kannaka.exe on Windows, kannaka elsewhere)."""
    import shutil
    exe = "kannaka.exe" if os.name == "nt" else "kannaka"
    return os.environ.get("QOS_KANNAKA_BIN") or shutil.which("kannaka") or (
        os.path.join(os.path.expanduser("~"), "Source", "kannaka-memory",
                     "target", "release", exe))


class KannakaCLI:
    """Thin, safe wrapper over the kannaka.exe CLI.

    SECURITY (from the epic-#127 attack panel, verified against kannaka's
    src/bin/kannaka.rs):
      * kannaka's `remember` flag parser matches --flags at ANY argv position
        and has NO `--` end-of-options escape (a bare `--` exits 2), so a
        text argument that begins with '-' is OPTION injection even in
        list-form (it can substitute the stored content, force a --substrate
        NATS publish, or DoS the call). remember() therefore REFUSES any text
        starting with '-'.
      * KANNAKA_READONLY makes a write SILENTLY no-op while still printing a
        UUID and exiting 0. The child env is built EXPLICITLY: reads force
        READONLY=1, writes REMOVE it (a merely-unset var still inherits).
      * stdout is parsed in isolation (KANNAKA_QUIET=1; notices go to stderr).
      * subprocess failures map to QosError/QosTimeout, never an uncaught raise.
    """

    def __init__(self, binary=None, timeout=120):
        self._binary_override = binary       # None => resolve QOS_KANNAKA_BIN per call
        self.timeout = timeout

    def _bin(self):
        return self._binary_override or _default_kannaka_bin()

    def _argv(self, binary, args):
        # A .py stub (the CI gate) is run through the current interpreter so
        # the same wiring works on the Linux runner and on Windows locally.
        if binary.endswith(".py"):
            return [sys.executable, binary, *args]
        return [binary, *args]

    def _run(self, args, write):
        binary = self._bin()
        env = os.environ.copy()
        env["KANNAKA_QUIET"] = "1"
        if write:
            env.pop("KANNAKA_READONLY", None)   # a write under READONLY no-ops
        else:
            env["KANNAKA_READONLY"] = "1"        # a read must not mutate the HRM
        with _kannaka_lock:                      # single-writer within this proc
            try:
                proc = subprocess.run(self._argv(binary, args), capture_output=True,
                                      text=True, timeout=self.timeout, env=env)
            except subprocess.TimeoutExpired:
                raise QosTimeout(f"kannaka timed out after {self.timeout}s "
                                 f"(store cold-load? op={args[0] if args else '?'})")
            except (FileNotFoundError, OSError) as exc:
                raise QosError(f"kannaka not runnable ({binary}): {exc}")
        if proc.returncode != 0:
            raise QosError(f"kannaka {args[0] if args else '?'} failed "
                           f"(exit {proc.returncode}): {proc.stderr.strip()[:200]}")
        return proc.stdout

    def remember(self, text, importance):
        """Persist `text` to the HRM; returns the new memory id. Writes the real
        store (or KANNAKA_DATA_DIR). Refuses option-injection / quote-forge text."""
        if not text or text[0] == "-":
            raise QosError("refusing kannaka text starting with '-' (kannaka has "
                           "no '--' end-of-options escape — option injection)")
        if '"' in text:
            raise QosError('refusing kannaka text containing a quote (field '
                           'winner-line forge guard)')
        out = self._run(["remember", text, "--importance", str(float(importance))],
                        write=True).strip()
        if not _UUID_RE.match(out):
            raise QosError(f"kannaka remember did not return a UUID: {out[:80]!r}")
        return out

    def recall(self, query, top_k):
        """Resonance recall; returns a list of {id, content, similarity, ...}.
        Read-only (KANNAKA_READONLY forced)."""
        out = self._run(["recall", query, "--top-k", str(int(top_k))], write=False)
        try:
            data = json.loads(out)
        except json.JSONDecodeError as exc:
            raise QosError(f"unparseable kannaka recall JSON: {exc}")
        if isinstance(data, dict):        # --envelope form
            data = data.get("data", [])
        return data if isinstance(data, list) else []


_kannaka = KannakaCLI()


class QosVM:
    """One QuantumOS VM. COM1 (=stdio) carries the boot log and the scripted
    qsh session; COM2 (=a TCP socket QEMU connects OUT to, so byte 0 of the
    one-shot attestation is captured and there is no free-port TOCTOU race)
    carries the attested bridge. A daemon thread drains COM1 the instant QEMU
    starts (or a full stdout pipe deadlocks QEMU mid-attestation). One coarse
    lock serializes every bridge transaction and qsh cycle, because FastMCP
    dispatches tool calls on a threadpool."""

    def __init__(self, kernel=None):
        self.kernel = kernel or _default_kernel()
        self.proc = None
        self.attestation = None
        self.boot_nonce = None
        self._com2 = None
        self._listen = None
        self._parser = FrameParser()
        self._log = bytearray()
        self._log_lock = threading.Lock()
        self._io_lock = threading.RLock()
        self._session_key = None  # set by admit_key(); enables status_authenticated()
        self._drain = None
        self._atexit_armed = False

    # -- COM1 drain ---------------------------------------------------------
    def _drain_stdout(self):
        f = self.proc.stdout
        while True:
            chunk = f.read(4096)
            if not chunk:
                return
            with self._log_lock:
                self._log.extend(chunk)

    def _log_text(self, start=0):
        with self._log_lock:
            return self._log[start:].decode("utf-8", errors="replace")

    def _log_len(self):
        with self._log_lock:
            return len(self._log)

    # -- lifecycle ----------------------------------------------------------
    def boot(self, qseed=None, timeout=30, *, netdev=None, append_extra="",
             quiet=True, mac=None, expect_qseed=_UNSET, arm_signals=True):
        """Boot one QuantumOS VM. The keyword-only args exist so a QosSociety can
        boot a COUPLED member without changing any single-VM caller's behaviour
        (all defaults reproduce the original boot exactly):
          netdev       replace `-netdev user` with a spec (e.g. a socket pair);
          append_extra extra cmdline tokens (ip=/peer= for the coupling wire);
          quiet        society members boot NON-quiet so ghostd's FIELDSYNC R_x
                       telemetry (quiet-gated) reaches COM1;
          mac          per-node NIC MAC (ARP on a shared L2 needs distinct MACs);
          expect_qseed verify the attestation against a DIFFERENT qseed than the
                       one booted (the negative-admission path); default = qseed;
          arm_signals  False when composed under a society (single owner reaps)."""
        if self.proc is not None:
            raise QosError("a VM is already running — shut it down first")
        if not os.path.exists(self.kernel):
            raise QosError(f"kernel image not found: {self.kernel}")

        # Host listens; QEMU connects OUT to this port for COM2.
        self._listen = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listen.bind(("127.0.0.1", 0))
        self._listen.listen(1)
        self._listen.settimeout(timeout)
        port = self._listen.getsockname()[1]

        if qseed is not None and not re.match(r"^[0-9A-Fa-f]{1,16}$", str(qseed)):
            self._listen.close()
            self._listen = None
            raise QosError(f"qseed must be 1-16 hex digits (or None): {qseed!r}")
        # `quiet` silences the per-second Timer-tick line and the service-health
        # churn (interrupts.c:380, service.c:457) that would otherwise interleave
        # into a scripted-command window; the one-time [BOOT] milestones and the
        # net self-test's `NET: DHCP lease` line survive (ungated boot_log), so
        # the QSH banner, ISOLATION-VERIFIED settle marker, and net-readiness
        # gate all still work. -netdev gives the guest its NIC so qos_fetch can
        # reach the network (SSRF-guarded host-side in fetch()).
        tokens = [EGRESS_WIDE]   # ADR-0030: the harness's VMs are networked on purpose
        if append_extra:
            tokens.append(append_extra.strip())
        if qseed:
            tokens.append(f"qseed={qseed}")
        if quiet:
            tokens.append("quiet")
        append = " ".join(tokens)
        if netdev:
            dev = "rtl8139,netdev=n0" + (f",mac={mac}" if mac else "")
            net_flags = ["-netdev", netdev, "-device", dev]
        else:
            net_flags = ["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        cmd = [
            "qemu-system-x86_64", "-kernel", self.kernel,
            "-append", append,
            "-serial", "stdio",                          # COM1: console + qsh
            "-serial", f"tcp:127.0.0.1:{port}",          # COM2: attested bridge
            *net_flags,
            "-m", "128M", "-display", "none", "-no-reboot",
        ]

        preexec = _pdeathsig if platform.system() == "Linux" else None
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0, preexec_fn=preexec,
        )
        self.boot_nonce = f"{uuid.uuid4().hex}-{time.monotonic_ns()}"
        self._arm_atexit(arm_signals=arm_signals)

        # Drain COM1 immediately, before any blocking COM2 read.
        self._drain = threading.Thread(target=self._drain_stdout, daemon=True)
        self._drain.start()

        # Accept QEMU's COM2 connection (it connects during startup).
        try:
            self._com2, _ = self._listen.accept()
        except socket.timeout:
            self.shutdown()
            raise QosTimeout("QEMU never connected COM2 (boot failed?)")
        finally:
            self._listen.close()
            self._listen = None
        self._com2.settimeout(1.0)

        # Read + verify the boot attestation (independent deadline from COM1).
        # expect_qseed lets the negative-admission path verify against a qseed
        # OTHER than the one booted (default: the booted qseed).
        want = qseed if expect_qseed is _UNSET else expect_qseed
        att_deadline = time.time() + timeout
        self.attestation = read_boot_attestation(self._recv_com2, att_deadline, want)

        # Wait for qsh readiness on COM1 (its own deadline — the two services
        # are independent and arrive in either order).
        self._await_banner(time.time() + timeout)
        # Let the transient boot citizens (init, canaries) finish before any
        # `run` spawns a citizen: an early spawn races the initrd/scheduler
        # settling and can load a stillborn process. Observable, bounded.
        self._settle(time.time() + 5)
        return self.identity()

    def _recv_com2(self):
        """Bytes on data, None on a read timeout (nothing yet), b'' on EOF."""
        if self.proc and self.proc.poll() is not None:
            return b""
        try:
            return self._com2.recv(4096)
        except socket.timeout:
            return None
        except OSError:
            return b""

    def _drain_com2(self):
        """Discard any bytes already waiting unread in the COM2 socket — a stale
        COMPLETE frame from a PRIOR transact whose reply landed AFTER its deadline
        (e.g. a slow authenticated STATUS on a loaded runner). Clearing only the
        parser's reassembly buffer misses such a frame, so the next transact would
        read it and, because the wire has no request/reply id, mis-attribute it to
        the new request (a fresh-nonce STATUS then fails on the echo forever).
        Non-blocking: reads until the socket would block, then restores the normal
        timeout. Caller holds _io_lock."""
        if self._com2 is None:
            return
        prev = self._com2.gettimeout()
        try:
            self._com2.settimeout(0.0)
            while True:
                try:
                    if not self._com2.recv(4096):
                        break  # EOF
                except (BlockingIOError, socket.timeout):
                    break  # nothing more pending
                except OSError:
                    break
        finally:
            self._com2.settimeout(prev)

    def _await_banner(self, deadline):
        while time.time() < deadline:
            if QSH_BANNER in self._log_text():
                return
            if self.proc.poll() is not None:
                raise QosDead("VM exited before qsh came up")
            time.sleep(0.1)
        raise QosTimeout("qsh banner never appeared on COM1")

    def _settle(self, deadline):
        """Wait for the boot self-test citizens to finish (the canary
        'ISOLATION VERIFIED' line marks the transient spawns done), so a
        later `run` doesn't race early boot. Bounded — proceeds at the
        deadline regardless."""
        while time.time() < deadline:
            if "ISOLATION VERIFIED" in self._log_text():
                return
            if self.proc.poll() is not None:
                raise QosDead("VM exited during boot settle")
            time.sleep(0.1)

    def await_network(self, timeout_s=12.0):
        """Block until the boot log shows a DHCP lease. net_selftest logs
        `NET: DHCP lease <ip>` via UNGATED boot_log (net.c:999) even under
        `quiet`, so this both serializes a fetch after the lease (the first
        connect must not race DHCP) AND anti-vacuously proves -netdev took
        effect (a NIC-less boot logs `NET: no NIC` instead). Bounded."""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if "NET: DHCP lease" in self._log_text():
                return
            if self.proc is not None and self.proc.poll() is not None:
                raise QosDead("VM exited before a DHCP lease")
            time.sleep(0.1)
        raise QosTimeout("no DHCP lease (netdev missing or SLIRP DHCP failed)")

    def _arm_atexit(self, arm_signals=True):
        if self._atexit_armed:
            return
        atexit.register(self.shutdown)
        # arm_signals=False when a QosSociety composes this VM: a per-VM signal
        # handler is process-global (last-writer-wins) and its os._exit(1) skips
        # the other VMs' atexit reapers — the society installs ONE handler that
        # reaps every member instead.
        if arm_signals:
            for sig in (signal.SIGTERM, signal.SIGINT):
                try:
                    signal.signal(sig, self._signal_shutdown)
                except (ValueError, OSError):
                    pass  # not the main thread / unsupported — atexit still covers
        self._atexit_armed = True

    def _signal_shutdown(self, *_):
        self.shutdown()
        os._exit(1)

    def shutdown(self):
        """Idempotent: SIGTERM -> grace -> SIGKILL -> reap. qsh `exit` only
        triggers a watchdog rebirth, never a poweroff, so killing QEMU is the
        only clean stop."""
        proc, self.proc = self.proc, None
        if self._com2 is not None:
            try:
                self._com2.close()
            except OSError:
                pass
            self._com2 = None
        if self._listen is not None:
            try:
                self._listen.close()
            except OSError:
                pass
            self._listen = None
        if proc is None:
            return {"stopped": True}
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
        return {"stopped": True}

    def is_running(self):
        return self.proc is not None and self.proc.poll() is None

    # -- identity + guards --------------------------------------------------
    def identity(self):
        if self.attestation is None:
            return {"verified": False, "reason": "no VM booted", "boot_nonce": None}
        return self.attestation.to_dict(self.boot_nonce)

    def _ensure_verified(self):
        if self.attestation is None or not self.attestation.verified:
            reason = self.attestation.reason if self.attestation else "no VM booted"
            raise QosRefused(f"attestation not verified: {reason}")
        if self.proc is not None and self.proc.poll() is not None:
            raise QosDead("VM has exited")

    # -- COM2 bridge ops ----------------------------------------------------
    def _transact(self, req_frame, expect_op, deadline):
        """Send one DATA request and return the matching-opcode reply payload."""
        with self._io_lock:
            # Drop any bytes left in the reassembly buffer from a PRIOR transact
            # (e.g. a reply that arrived after that call's deadline). The wire
            # carries no request/reply correlation id, so a stale same-opcode
            # frame here would be mis-attributed to THIS request — flush both the
            # partial-reassembly bytes AND any stale COMPLETE frame still unread
            # in the socket (the latter bit rapid nonce'd retries on slow runners).
            self._parser.buf.clear()
            self._drain_com2()
            self._com2.sendall(req_frame)
            while time.time() < deadline:
                chunk = self._recv_com2()
                if chunk is None:
                    continue
                if chunk == b"":
                    raise QosDead("COM2 closed (VM gone)")
                for ftype, payload in self._parser.feed(chunk):
                    if ftype == FRAME_DATA and payload and payload[0] == expect_op:
                        return payload
                    # stray PONG / late attestation frame / other op: ignore
        raise QosTimeout(f"no DATA reply for op {expect_op}")

    def ping(self, deadline_s=5.0):
        """Round-trip a FRAME_PING and return the reply latency in SECONDS. The
        guest echoes it as FRAME_PONG directly in swarm_svc (no downstream IPC
        hop), so this measures the pure COM2 transport + scheduler-cadence
        latency — the one-hop floor under every tool call (ADR-0022). Raises
        QosTimeout if no PONG arrives within deadline_s."""
        self._ensure_verified()
        with self._io_lock:
            self._parser.buf.clear()
            self._drain_com2()
            t0 = time.time()
            self._com2.sendall(frame(FRAME_PING, b"\x50\x49"))  # 'PI'
            deadline = t0 + deadline_s
            while time.time() < deadline:
                chunk = self._recv_com2()
                if chunk is None:
                    continue
                if chunk == b"":
                    raise QosDead("COM2 closed (VM gone)")
                for ftype, _payload in self._parser.feed(chunk):
                    if ftype == FRAME_PONG:
                        return time.time() - t0
        raise QosTimeout("no PONG reply")

    def admit_key(self, key):
        """Admit the swarm-plane group session key (ADR-0019) to this VM over the
        attested COM2 channel: swarm_svc forwards it to fieldsyncd so the
        field-coupling wire can be HMAC-authenticated. `key` is 32 bytes.
        Fire-and-forget — the SWARM_OP_KEY guest handler sends no reply (the host
        is the key root; it does not depend on a guest ack). Trust reduces to
        control of this COM2 channel."""
        self._ensure_verified()
        key = bytes(key)
        if len(key) != KEY_LEN:
            raise QosError(f"session key must be {KEY_LEN} bytes, got {len(key)}")
        req = frame(FRAME_DATA, bytes([SWARM_OP_KEY]) + key)
        with self._io_lock:
            self._com2.sendall(req)
        # Retain the key so status_authenticated() can verify reply tags. The
        # guest (swarm_svc) keeps the same 32 bytes for the same purpose.
        self._session_key = key

    def attest(self, deadline_s=12.0):
        """Enable reply-auth for THIS session and confirm it is LIVE. Admits a
        FRESH host-generated 32-byte key, then retries an authenticated STATUS
        until the guest has consumed the key frame — after which every COM2 reply
        (STATUS via status_authenticated, QSUBMIT via qsubmit) is nonce+HMAC
        attested, so a tool RESULT is provably fresh + unforged, not merely that
        the boot attested (ADR-0015 'verified != live'). The host generating AND
        holding the key IS the trust model here: control of the serial channel ==
        control of the VM, so a session-scoped host key binds every reply to the
        VM this host booted. Returns the confirmed authenticated status dict.
        Fail-CLOSED: raises if attestation cannot go live in time (it never
        silently leaves the session on the plain path while claiming attested)."""
        self._ensure_verified()
        self.admit_key(os.urandom(KEY_LEN))
        deadline = time.time() + deadline_s
        last = None
        while time.time() < deadline:
            try:
                # Per-attempt deadline must comfortably EXCEED the guest's COM2
                # round-trip (~1.3s+, slower on a loaded runner) — 5s matches the
                # proven status() default. A tighter window would time out before
                # the reply lands, leaving it in flight for the NEXT attempt and
                # offsetting the nonce by one forever (the #201 CI failure).
                return self.status_authenticated(deadline_s=5.0)
            except (QosRefused, QosTimeout) as exc:
                # Not yet live: the guest may not have consumed the key frame yet
                # (a plain reply -> QosRefused) or not have replied in the window
                # (-> QosTimeout while it settles). Retry until the deadline; a
                # genuine VM death (QosDead) is NOT caught here and propagates.
                last = exc
                time.sleep(0.3)
        raise QosError(f"attestation did not go live within {deadline_s:.0f}s (last: {last})")

    def status_authenticated(self, deadline_s=5.0):
        """STATUS over COM2 with a per-request nonce + HMAC reply tag (ADR-0019
        Extension): unlike status(), this proves the reply is FRESH (nonce echo)
        and UNFORGED (tag), not merely that the boot attested. Requires a key
        admitted via admit_key(). Returns {r, live, authenticated: True,
        identity}. Raises QosRefused on a replayed (nonce mismatch) or forged
        (bad MAC) reply, and fails CLOSED — a keyed caller never accepts an
        unauthenticated (stripped-tag) reply."""
        self._ensure_verified()
        if self._session_key is None:
            raise QosError("no session key admitted — call admit_key() first")
        nonce = os.urandom(REPLYAUTH_NONCE_LEN)
        payload = self._transact(frame(FRAME_DATA, bytes([SWARM_OP_STATUS]) + nonce),
                                 SWARM_OP_STATUS, time.time() + deadline_s)
        return self._verify_status_reply(payload, nonce)

    def _verify_reply_auth(self, payload, expect_nonce, op, valid_body_lens):
        """One audited COM2 reply-auth verify for every op (ADR-0019 Extension).
        Reply layout: body(var) | nonce_echo(16) | tag(32), where body[0]==op and
        body_len is in valid_body_lens (fixed for STATUS, {2,22} for QSUBMIT). The
        MAC preimage op||nonce||body[1..] is identical across ops, so one function
        gets the crypto right for all callers. Freshness (echo) is checked FIRST —
        a replayed genuine reply carries an OLD nonce whose tag is still valid, so
        only the echo betrays it — then the MAC (unforgeability). Fails CLOSED: a
        stripped/short reply is rejected on the length floor, never mis-sliced.
        Returns the verified `body` bytes for the caller to parse."""
        if self._session_key is None:
            raise QosError("no session key admitted")
        total = len(payload)
        floor = min(valid_body_lens) + REPLYAUTH_NONCE_LEN + REPLYAUTH_TAG_LEN
        if total < floor:
            raise QosRefused(
                f"op {op} reply too short to authenticate ({total} < {floor}) — "
                "tag stripped or unkeyed guest")
        body_len = total - REPLYAUTH_NONCE_LEN - REPLYAUTH_TAG_LEN
        if body_len not in valid_body_lens:
            raise QosRefused(f"op {op} reply body_len {body_len} not in {valid_body_lens}")
        body = bytes(payload[:body_len])
        echo = bytes(payload[body_len:body_len + REPLYAUTH_NONCE_LEN])
        tag = bytes(payload[body_len + REPLYAUTH_NONCE_LEN:
                            body_len + REPLYAUTH_NONCE_LEN + REPLYAUTH_TAG_LEN])
        if body[0] != op:
            raise QosRefused(f"op {op} reply op mismatch (0x{body[0]:02x}) — cross-op frame")
        if echo != bytes(expect_nonce):
            raise QosRefused(f"nonce echo mismatch — replayed or stale op {op} reply")
        macin = bytes([op]) + bytes(expect_nonce) + body[1:]
        want = hmac.new(self._session_key, macin, hashlib.sha256).digest()
        if not hmac.compare_digest(want, tag):
            raise QosRefused(f"reply MAC failed — forged op {op} reply")
        return body

    def _verify_status_reply(self, payload, expect_nonce):
        """Verify an authenticated STATUS reply (body op|r_q16(4)|live = 6 bytes)."""
        body = self._verify_reply_auth(payload, expect_nonce, SWARM_OP_STATUS,
                                       (STATUS_BODY_LEN,))
        r_q16 = int.from_bytes(body[1:5], "little")
        return {"r": r_q16 / STATUS_R_SCALE, "live": body[5], "authenticated": True,
                "identity": self.identity()}

    def status(self, deadline_s=5.0):
        """ghostd field STATUS over COM2: {r, live}. (This is the ghostd
        Hopfield field, distinct from qsh's kernel holographic field.)"""
        self._ensure_verified()
        payload = self._transact(frame(FRAME_DATA, bytes([SWARM_OP_STATUS])),
                                 SWARM_OP_STATUS, time.time() + deadline_s)
        r_q16 = int.from_bytes(payload[1:5], "little") if len(payload) >= 5 else 0
        live = payload[5] if len(payload) > 5 else 0
        return {"r": r_q16 / STATUS_R_SCALE, "live": live, "identity": self.identity()}

    def qsubmit(self, circuit, deadline_s=10.0):
        """Submit an OPAQUE circuit (qpu_circuit.h bytes) to the in-OS QPU broker
        over the attested COM2 bridge (epic #149 B1): swarm_svc forwards it to
        SYS_QPU, qpud runs it on the exact integer engine, and the exact result
        comes back over the wire. Returns {status, result} where status is
        0=OK / 1=refused(quota) / 2=broker-or-EINVAL and result is the 20-byte
        QC result (present on status 0). The deadline is generous: swarm_svc
        never self-times-out on a live qpud (it polls to DONE, heartbeating), so
        a timeout here means the VM is wedged, not a slow circuit.

        Reply-auth (ADR-0019 Extension): when a session key has been admitted
        (admit_key), the request carries a fresh 16-byte nonce (op|nonce|circuit)
        and the reply is verified (nonce echo + HMAC) — a keyed submit whose
        reply is stripped, stale, or forged RAISES QosRefused (fail-closed), so
        an on-serial attacker cannot forge a job result. This is the SOLE COM2
        QSUBMIT emitter (qpu_run/the MCP tool delegate here), so the guest's
        enforce-when-keyed framing can never desync from a bypassing caller.
        Unkeyed submits keep the legacy op|circuit wire and plain reply."""
        self._ensure_verified()
        if self._session_key is not None:
            nonce = os.urandom(REPLYAUTH_NONCE_LEN)
            payload = self._transact(
                frame(FRAME_DATA, bytes([SWARM_OP_QSUBMIT]) + nonce + bytes(circuit)),
                SWARM_OP_QSUBMIT, time.time() + deadline_s)
            return self._verify_qsubmit_reply(payload, nonce)
        payload = self._transact(frame(FRAME_DATA, bytes([SWARM_OP_QSUBMIT]) + bytes(circuit)),
                                 SWARM_OP_QSUBMIT, time.time() + deadline_s)
        status = payload[1] if len(payload) >= 2 else 2
        result = bytes(payload[2:]) if len(payload) > 2 else b""
        return {"status": status, "result": result}

    def _verify_qsubmit_reply(self, payload, expect_nonce):
        """Verify an authenticated QSUBMIT reply. Body is op|status(1) for an
        error or op|status(1)|result(20) for DONE, so body_len is 2 or 22."""
        body = self._verify_reply_auth(payload, expect_nonce, SWARM_OP_QSUBMIT,
                                       (QSUBMIT_BODY_ERR, QSUBMIT_BODY_OK))
        return {"status": body[1], "result": body[2:] if len(body) > 2 else b"",
                "authenticated": True}

    def qpu_run(self, kind="bell", n_qubits=3, target=0, iters=1, probe=0,
                deadline_s=10.0):
        """Build a NAMED opaque circuit, submit it to the in-OS QPU broker over
        the attested bridge (see qsubmit), and decode the exact result. `kind` is
        'bell' | 'ghz' | 'grover' (grover uses n_qubits/target/iters). This is the
        agent-facing convenience over the raw-bytes qsubmit: the circuit build and
        the QC-result decode live here, stdlib-only, so the MCP tool stays a
        one-line delegation. Returns {kind, status, ok, ..., identity}; status
        0=OK / 1=quota-refused / 2=broker-or-EINVAL."""
        import qpu_circuit as qc
        if kind == "bell":
            circuit = qc.bell(probe=probe)
        elif kind == "ghz":
            circuit = qc.ghz(n_qubits, probe=probe)
        elif kind == "grover":
            circuit = qc.grover(n_qubits, target, iters)
        else:
            raise QosError(f"unknown circuit kind {kind!r} (want bell|ghz|grover)")
        res = self.qsubmit(circuit, deadline_s=deadline_s)
        out = {"kind": kind, "status": res["status"], "identity": self.identity(),
               "attested": bool(res.get("authenticated"))}
        if res["status"] == 0 and len(res["result"]) >= qc.QC_RESULT_LEN:
            d = qc.parse_result(res["result"])
            out.update({
                "ok": True,
                "n_qubits": d["n_qubits"],
                "probability": (d["num"] / d["den"]) if d["den"] else 0.0,
                "prob_num": d["num"],
                "prob_den": d["den"],
                "amp_re": d["amp_re"],
                "amp_im": d["amp_im"],
                "h": d["h"],
            })
        else:
            out["ok"] = False
            out["detail"] = ("quota refused" if res["status"] == 1
                             else "broker rejected (malformed circuit / EINVAL)")
        return out

    # -- COM1 scripted qsh --------------------------------------------------
    def _send_line(self, line):
        """Write one command to qsh; return the log offset just before it (so
        a subsequent wait only sees output this command produced). Caller
        holds _io_lock."""
        if not self.is_running():
            raise QosDead("VM has exited")
        start = self._log_len()
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        return start

    def _wait_markers(self, start, markers, deadline_s, what, max_s=None):
        """Poll the COM1 log from `start` until one of the regex `markers`
        matches; return (body, match). Caller holds _io_lock.

        With `max_s`, `deadline_s` is an IDLE bound instead: the wait restarts
        whenever new output arrives and gives up only after `deadline_s` with
        none, or after `max_s` overall. For long dumps (audit, #291) whose
        transfer time over COM1 scales with host speed, not with liveness.

        The echoed command line is dropped before matching: qsh echoes typed
        input verbatim (qsh.c:1392) and it lands in the log BEFORE the real
        output, so a marker must never be allowed to match the agent's own
        input (a recall probe or run args containing a `FIELD:`/`qsh:` line
        form would otherwise spoof the result). Real output always follows the
        CRLF after the echo, so search only past the first newline; markers are
        additionally line-anchored (re.M `^`) by their callers."""
        now = time.time()
        deadline = now + deadline_s
        hard = now + max_s if max_s is not None else deadline
        seen = -1
        while time.time() < min(deadline, hard):
            text = self._log_text(start)
            nl = text.find("\n")
            body = text[nl + 1:] if nl >= 0 else ""
            for pat in markers:
                m = pat.search(body)
                if m:
                    return body, m
            if self.proc.poll() is not None:
                raise QosDead("VM exited mid-command")
            if max_s is not None and len(text) != seen:
                seen = len(text)
                deadline = time.time() + deadline_s
            time.sleep(0.05)
        raise QosTimeout(f"{what}: no completion marker")

    def _qsh(self, line, markers, deadline_s):
        with self._io_lock:
            start = self._send_line(line)
            return self._wait_markers(start, markers, deadline_s, repr(line))

    def _collect(self, cmds, deadline_s, what, max_s=None):
        """Run one or more qsh commands under a single lock hold, then bracket
        their output with a fresh-nonce `echo` sentinel and return the window
        up to that sentinel. The caller extracts by known line prefix (^PS: /
        ^MEM: / ...), so interleaved [BOOT] noise is naturally excluded and the
        fresh nonce can appear in no stored or echoed data. One sentinel round
        trip covers all commands."""
        nonce = "QOS-EOF-" + uuid.uuid4().hex
        sentinel = re.compile(r"^qsh: " + re.escape(nonce) + r"\s*$", re.M)
        with self._io_lock:
            start = self._log_len()
            for c in cmds:
                self._send_line(c)
            self._send_line("echo " + nonce)
            body, _m = self._wait_markers(start, (sentinel,), deadline_s, what, max_s)
            return body

    def imprint(self, text, energy_pct=None, deadline_s=10.0):
        """Kernel holographic field imprint via qsh (same space as a human at
        the prompt). `energy_pct` (1..100) sets the slot's importance via the
        `imprint --energy` argument; omitted → the field default (~50%).
        Returns the assigned slot."""
        self._ensure_verified()
        if energy_pct is not None:
            energy_pct = int(energy_pct)
            if not 1 <= energy_pct <= 100:
                raise QosError(f"energy_pct must be 1..100: {energy_pct}")
            prefix = f"imprint --energy {energy_pct} "
        else:
            prefix = "imprint "
        _qsh_text(text, QSH_LINE_MAX - len(prefix), "imprint text")
        _ok = re.compile(r"^FIELD: imprinted slot (\d+)", re.M)
        _err = re.compile(r"^qsh: imprint(?: failed| : usage|:)", re.M)
        _txt, m = self._qsh(prefix + text, (_ok, _err), deadline_s)
        if m.re is _err:
            raise QosError("imprint rejected by qsh")
        return {"slot": int(m.group(1)), "identity": self.identity()}

    def field_info(self, region=0, deadline_s=10.0):
        """READ-ONLY field enumeration (epic #127 B1): live count, capacity, and
        per alive slot {slot, len, energy (% of max), eff (decayed %), retrievals,
        age, preview}. Unlike recall this does NOT reinforce — repeated calls do
        not perturb the field. (qsh reaches only its granted region 0.)"""
        self._ensure_verified()
        text = self._collect(["field"], deadline_s, "field")
        m = re.search(r"^FIELDINFO: region=(\d+) live=(\d+) cap=(\d+)", text, re.M)
        if not m:
            raise QosError("field: no FIELDINFO line")
        slots = []
        for s in re.finditer(
                r'^FIELDSLOT: slot=(\d+) len=(\d+) energy=(\d+) eff=(\d+) '
                r'retr=(\d+) age=(\d+) preview="(.*?)"', text, re.M):
            slots.append({"slot": int(s.group(1)), "len": int(s.group(2)),
                          "energy": int(s.group(3)), "eff": int(s.group(4)),
                          "retrievals": int(s.group(5)), "age": int(s.group(6)),
                          "preview": s.group(7)})
        return {"region": int(m.group(1)), "live": int(m.group(2)),
                "capacity": int(m.group(3)), "slots": slots,
                "identity": self.identity()}

    def recall(self, text, deadline_s=10.0):
        """Kernel holographic field recall via qsh. Returns the winner text."""
        self._ensure_verified()
        _qsh_text(text, QSH_LINE_MAX - len("recall "), "recall probe")
        _win = re.compile(r'^FIELD: winner="(.*?)" slot=(\d+) score=(-?\d+) n=(\d+)', re.M)
        _empty = re.compile(r"^FIELD: recall empty", re.M)
        _err = re.compile(r"^qsh: recall(?: failed|:)", re.M)
        _txt, m = self._qsh("recall " + text, (_win, _empty, _err), deadline_s)
        if m.re is _win:
            return {"winner": m.group(1), "slot": int(m.group(2)),
                    "score": int(m.group(3)), "n": int(m.group(4)),
                    "identity": self.identity()}
        return {"winner": None, "slot": None, "score": None, "n": 0,
                "identity": self.identity()}

    def audit(self, deadline_s=12.0, max_s=90.0):
        """Read the kernel capability AUTHORITY LEDGER read-only (epic #133): the
        kernel-recorded GRANT / DENY / SPAWN events. The KERNEL writes it, so a
        citizen cannot forge or suppress its own entry. Returns {total, dropped,
        capacity, entries:[{seq,tick,pid,kind,resource_type,resource_id,perms,
        verdict,count}]}. Honest scope: an authority ledger (grants+denials+
        spawns), NOT a full syscall trace; successful exercise of held authority
        and revoke/expire are not recorded. Read over COM1; a cryptographic
        export over the attested COM2 channel is a follow-up (a malicious citizen
        could newline-inject a forged line — the host anchors ^AUDIT: and the
        line-forge guard mitigates, but v1 trusts the shipped citizens)."""
        self._ensure_verified()
        # The whole ledger (~190 lines) streams over COM1, so deadline_s is an
        # idle bound under a max_s cap: a loaded host is slow, not dead (#291).
        text = self._collect(["audit"], deadline_s, "audit", max_s=max_s)
        st = re.search(r"^AUDIT: total=(\d+) dropped=(\d+) capacity=(\d+)", text, re.M)
        entries = []
        # kind group is [\w?] not \w: a half-added kind formats as "?" and MUST
        # be visible so the EMPTY/? tripwire fires (epic #148 gate-vacuity fix).
        for m in re.finditer(
                r"^AUDIT: seq=(\d+) tick=(\d+) pid=(\d+) kind=([\w?]+) res=(\w+):(\*|\d+) "
                r"perms=0x([0-9a-fA-F]+) verdict=(\w+) count=(\d+)", text, re.M):
            entries.append({"seq": int(m.group(1)), "tick": int(m.group(2)),
                            "pid": int(m.group(3)), "kind": m.group(4),
                            "resource_type": m.group(5), "resource_id": m.group(6),
                            "perms": int(m.group(7), 16), "verdict": m.group(8),
                            "count": int(m.group(9))})
        return {"total": int(st.group(1)) if st else None,
                "dropped": int(st.group(2)) if st else None,
                "capacity": int(st.group(3)) if st else None,
                "entries": entries, "identity": self.identity()}

    def manifest(self, deadline_s=12.0):
        """Read the per-pid INTENT MANIFESTS read-only (epic #135 Phase D inc. 2):
        every bound citizen's DECLARED allow-set, spawn quota (used/max), and
        cpu_ticks — a policy layer above raw capabilities that the KERNEL writes,
        so a citizen cannot forge its own row. Returns {truncated, manifests:{pid:
        {bound, spawn_used, spawn_max (None=unlimited), cpu_ticks, allow:[{res,id,
        perms}]}}}. Same disclosure stance as the audit ledger for grants (already
        public there); cpu_ticks is the one added, documented timing signal. Read
        over COM1; the ^MANIFEST: anchor + the newline-injection prefixing in
        user_console_write close the column-0 forgery the audit ledger documented."""
        self._ensure_verified()
        text = self._collect(["manifest"], deadline_s, "manifest")
        manifests = {}
        # qsub= is an OPTIONAL trailing suffix (epic #148): the anchor stays on
        # the stable prefix so pre-qsub kernels still parse, and qsub is None
        # when absent.
        for m in re.finditer(
                r"^MANIFEST: pid=(\d+) bound=(\d+) spawn=(\d+)/(\*|\d+) cpu_ticks=(\d+)"
                r"(?: qsub=(\d+)/(\*|\d+))?",
                text, re.M):
            pid = int(m.group(1))
            manifests[pid] = {
                "bound": int(m.group(2)),
                "spawn_used": int(m.group(3)),
                "spawn_max": None if m.group(4) == "*" else int(m.group(4)),
                "cpu_ticks": int(m.group(5)),
                "qsub_used": None if m.group(6) is None else int(m.group(6)),
                "qsub_max": (None if m.group(7) in (None, "*") else int(m.group(7))),
                "allow": [],
            }
        for m in re.finditer(
                r"^MANIFEST: pid=(\d+) allow res=(\w+):(\d+) perms=0x([0-9a-fA-F]+)",
                text, re.M):
            pid = int(m.group(1))
            if pid in manifests:
                manifests[pid]["allow"].append({
                    "res": m.group(2), "id": int(m.group(3)),
                    "perms": int(m.group(4), 16)})
        return {"truncated": "MANIFEST: truncated=1" in text,
                "manifests": manifests, "identity": self.identity()}

    def run(self, program, args="", deadline_s=20.0):
        """Spawn a /bin citizen via qsh with an optional argument string and
        collect its output + exit code. Matches the SPECIFIC spawned pid's exit
        line, so an interleaved boot process exiting can't be mistaken for this
        program's completion. `args` is validated (no control bytes, bounded,
        no marker impersonation) before it is spliced into the run line."""
        self._ensure_verified()
        if not _ALLOWED_RUN.match(program):
            raise QosError(f"program must match /bin/<name>: {program!r}")
        if args:
            _qsh_text(args, QSH_LINE_MAX - len("run ") - len(program) - 1, "run args")
            line = "run " + program + " " + args
        else:
            line = "run " + program
        with self._io_lock:
            start = self._send_line(line)
            _spawned = re.compile(r"^qsh: spawned pid (\d+)", re.M)
            _nostart = re.compile(r"^qsh: run: cannot start", re.M)
            _text, m = self._wait_markers(start, (_spawned, _nostart), deadline_s,
                                          f"run {program}")
            if m.re is _nostart:
                raise QosError(f"qsh could not start {program}")
            pid = m.group(1)
            _done = re.compile(r"^qsh: pid " + pid + r" exited \(code (\d+)\)", re.M)
            _gone = re.compile(r"^qsh: pid " + pid + r" (?:still running|vanished)", re.M)
            text, m = self._wait_markers(start, (_done, _gone), deadline_s,
                                         f"run {program} completion")
            code = int(m.group(1)) if m.re is _done else None
        return {"output": text.strip(), "exit_code": code, "args": args,
                "identity": self.identity()}

    # -- sysinfo / entropy / files / network over scripted qsh --------------
    def sysinfo(self, deadline_s=12.0):
        """One structured snapshot of the machine: uptime, heap/frame memory,
        the live process table, and the RTC date. Collected in a single
        sentinel-bracketed batch; every field is parsed by its own line prefix
        (qsh: uptime / MEM: / PS: / TIME:), so boot noise cannot leak in."""
        self._ensure_verified()
        text = self._collect(["uptime", "free", "ps", "date"], deadline_s, "sysinfo")
        uptime = {}
        mu = re.search(r"^qsh: uptime (\d+) ticks \((\d+) s\)", text, re.M)
        if mu:
            uptime = {"ticks": int(mu.group(1)), "s": int(mu.group(2))}
        mem = {}
        mm = re.search(r"^MEM: heap free=(\d+) bytes, frames free=(\d+)/(\d+)", text, re.M)
        if mm:
            mem = {"heap_free_bytes": int(mm.group(1)),
                   "frames_free": int(mm.group(2)), "frames_total": int(mm.group(3))}
        procs = [{"pid": int(p.group(1)), "name": p.group(2), "state": p.group(3)}
                 for p in re.finditer(r"^PS: (\d+) (\S+) (\w+)", text, re.M)]
        md = re.search(r"^TIME: (.+?)\s*$", text, re.M)
        return {"uptime": uptime, "mem": mem, "processes": procs,
                "date": md.group(1) if md else None, "identity": self.identity()}

    def sched(self, deadline_s=12.0):
        """One scheduler-baseline sample (ADR-0022 prerequisite 2): the SCHED: line
        from the argument-free qsh `sched` command, parsed into counters. `switches`
        is total context switches (yield-dominated), `preempt` is timer-quantum
        expiries only (the quantum-sensitive metric), `ticks` the current PIT tick,
        and maxgap/spread/runnable the fairness/tail snapshot over the runnable
        citizens. Argument-free -> no agent bytes reach the parsed line. Raises
        QosError if the line never appears."""
        self._ensure_verified()
        text = self._collect(["sched"], deadline_s, "sched")
        m = re.search(
            r"^SCHED: switches=(\d+) preempt=(\d+) ticks=(\d+) maxgap=(\d+) spread=(\d+) runnable=(\d+)"
            r"(?: boost=(\d+))?",
            text, re.M)
        if not m:
            raise QosError("sched: SCHED line not found in qsh output")
        return {"switches": int(m.group(1)), "preempt": int(m.group(2)), "ticks": int(m.group(3)),
                "maxgap": int(m.group(4)), "spread": int(m.group(5)), "runnable": int(m.group(6)),
                # ADR-0022 I/O boost: HONORED boost picks (trailing, optional so a
                # pre-boost kernel still parses). 0 when absent.
                "boost": int(m.group(7)) if m.group(7) else 0}

    def qrand(self, deadline_s=8.0):
        """Draw 64 bits of quantum-seeded entropy via the qsh `qrand` command
        (SYS_QRAND, capability-gated in the guest). Returns lowercase hex."""
        self._ensure_verified()
        _ok = re.compile(r"^qsh: qrand ([0-9a-f]{16})", re.M)
        _err = re.compile(r"^qsh: qrand denied", re.M)
        _txt, m = self._qsh("qrand", (_ok, _err), deadline_s)
        if m.re is _err:
            raise QosError("qrand denied (EPERM)")
        return {"hex": m.group(1), "bits": 64, "identity": self.identity()}

    def fetch(self, host, port=80, allow_private=False, deadline_s=30.0):
        """Have QuantumOS fetch `http://host:port/` over its own TCP stack and
        report the status line, byte count, and whether the transfer completed.

        HONESTY: this reaches the HOST network namespace via SLIRP NAT
        (10.0.2.2 == the host's 127.0.0.1; DNS and outbound internet are live),
        NOT a sandbox — so loopback / RFC1918 targets are REFUSED by default as
        an SSRF guard (pass allow_private=True to override). The response BODY
        is not returned: qsh's `http` only prints the status line and byte count
        (a body-returning fetch is a filed guest follow-up)."""
        self._ensure_verified()
        if not _FETCH_HOST.match(host):
            raise QosError(f"invalid host {host!r}")
        port = int(port)
        if not 1 <= port <= 65535:
            raise QosError(f"port out of range: {port}")
        if not allow_private and _is_private_target(host):
            raise QosError(
                f"refusing private/loopback target {host!r}: qos_fetch reaches "
                f"the host network via SLIRP (SSRF guard); pass allow_private=True")
        self.await_network()
        _ok = re.compile(r"^qsh: http " + re.escape(host) + r" -> (.+?)\s*$", re.M)
        _err = re.compile(r"^qsh: http: (denied[^\r\n]*|could not resolve host|"
                          r"connect failed|send failed|no response[^\r\n]*)", re.M)
        _bytes = re.compile(r"^qsh: http " + re.escape(host) +
                            r": (\d+) bytes( then connection reset)?", re.M)
        # qsh's `http` prints the host label WITHOUT the port (qsh.c:920).
        line = "http " + host + (" " + str(port) if port != 80 else "")
        with self._io_lock:
            start = self._send_line(line)
            _t, m = self._wait_markers(start, (_ok, _err), deadline_s, f"fetch {host}")
            if m.re is _err:
                return {"ok": False, "error": m.group(1).strip(),
                        "identity": self.identity()}
            status_line = m.group(1).strip()
            _t2, m2 = self._wait_markers(start, (_bytes,), deadline_s,
                                         f"fetch {host} byte count")
        return {"ok": True, "status_line": status_line, "bytes": int(m2.group(1)),
                "complete": m2.group(2) is None, "identity": self.identity()}

    def fs(self, op, path="", text="", deadline_s=12.0):
        """Overlay filesystem write / rm / sync over scripted qsh. read and ls
        are intentionally NOT here: their output is unprefixed (ls rows) or
        agent-controlled (cat content can impersonate the cannot-open line), so
        they need a guest-side machine marker — a filed follow-up. write/rm/sync
        each report a prefixed `qsh:` line that echoed input cannot forge."""
        self._ensure_verified()
        if op == "sync":
            _ok = re.compile(r"^qsh: sync ok \((\d+) files flushed\)", re.M)
            _err = re.compile(r"^qsh: sync failed \(([^)]*)\)", re.M)
            _txt, m = self._qsh("sync", (_ok, _err), deadline_s)
            if m.re is _err:
                raise QosError(f"sync failed: {m.group(1)}")
            return {"op": "sync", "flushed": int(m.group(1)), "identity": self.identity()}
        _qsh_path(path)
        if op == "write":
            _qsh_text(text, QSH_LINE_MAX - len("write ") - len(path) - 1, "write text")
            _ok = re.compile(r"^qsh: wrote (\d+) bytes to " + re.escape(path), re.M)
            _err = re.compile(r"^qsh: write: (cannot create[^\r\n]*|usage[^\r\n]*)", re.M)
            _txt, m = self._qsh("write " + path + " " + text, (_ok, _err), deadline_s)
            if m.re is _err:
                raise QosError(f"write failed: {m.group(1).strip()}")
            return {"op": "write", "path": path, "bytes": int(m.group(1)),
                    "identity": self.identity()}
        if op == "rm":
            _ok = re.compile(r"^qsh: removed", re.M)
            _err = re.compile(r"^qsh: rm: failed \(err (\d+)\)", re.M)
            _txt, m = self._qsh("rm " + path, (_ok, _err), deadline_s)
            if m.re is _err:
                raise QosError(f"rm failed (err {m.group(1)})")
            return {"op": "rm", "path": path, "removed": True, "identity": self.identity()}
        raise QosError(f"unknown fs op {op!r} (write|rm|sync; read/ls deferred)")

    # -- Kannaka HRM bridge (epic #127) -------------------------------------
    def memory_import(self, query, top_k=5, region=0):
        """Recall the top-k HRM memories for `query` and imprint each into the
        kernel field, so the OS's associative memory is seeded from the host
        memory system.

        HONEST LIMITATIONS: the field slot is <= 64 BYTES, so longer content is
        truncated (reported per result); the field is ASCII-only (qsh line
        protocol), so a non-ASCII memory is SKIPPED, not imprinted. Kannaka
        strength IS now carried into the slot energy (as an `imprint --energy`
        percent) so importance survives the hop (epic #127 B1). One bad result
        never aborts the batch."""
        self._ensure_verified()
        results = _kannaka.recall(query, top_k)
        out = []
        for r in results:
            content = r.get("content", "") if isinstance(r, dict) else ""
            kid = r.get("id") if isinstance(r, dict) else None
            sim = r.get("similarity") if isinstance(r, dict) else None
            stored = content[:FIELD_PAT_MAX]
            truncated = len(content.encode("utf-8", "replace")) > FIELD_PAT_MAX
            # Kannaka similarity/strength in [0,1] -> energy percent [1,100]; a
            # missing/degenerate score floors at 1 (never 0 — 0 is the kernel's
            # "use the default" sentinel).
            pct = max(1, min(100, round(sim * 100))) if isinstance(sim, (int, float)) else None
            try:
                if '"' in stored:
                    raise QosError("content contains a quote (field forge guard)")
                res = self.imprint(stored, energy_pct=pct)  # validates ASCII/budget
                out.append({"slot": res["slot"], "kannaka_id": kid,
                            "content": stored, "truncated": truncated,
                            "similarity": sim, "energy_pct": pct})
            except QosError as exc:
                out.append({"slot": None, "kannaka_id": kid, "content": None,
                            "skipped": str(exc), "similarity": sim})
        return {"imported": sum(1 for o in out if o["slot"] is not None),
                "results": out, "identity": self.identity()}

    def memory_export(self, probe, importance=0.6, allow_write=False, region=0):
        """Recall the field completion for `probe` and persist it to the HRM.

        REFUSES unless allow_write=True: this is a WRITE to the user's real
        ~/.kannaka memory (or KANNAKA_DATA_DIR), content derived from the
        (possibly agent-seeded) field — a memory-poisoning surface, so it is
        gated like qos_fetch's private-target guard. It also REINFORCES the
        recalled field slot (qsh holds CAP_WRITE — recall is not a pure read).
        `importance` is a caller-supplied constant (the slot's own energy is not
        readable until the field-info follow-up)."""
        self._ensure_verified()
        if not allow_write:
            raise QosError("qos_memory_export writes the real HRM: pass "
                           "allow_write=True to confirm (memory-poisoning guard)")
        rec = self.recall(probe)                       # reinforces the winner
        winner = rec.get("winner")
        if not winner or rec.get("n", 0) == 0:
            return {"exported": False, "reason": "field empty — nothing resonated",
                    "identity": self.identity()}
        kid = _kannaka.remember(winner, importance)    # refuses '-'/quote text
        return {"exported": True, "kannaka_id": kid, "content": winner,
                "field_score_q15": rec.get("score"), "importance": float(importance),
                "reinforced": True, "identity": self.identity()}

    def bridged_recall(self, query, top_k=3, region=0):
        """Query BOTH memories on the same cue and return them side by side: the
        field's completion (resonance = cosine x energy) and the HRM's top-k
        (its own resonance). NOTE: the field recall REINFORCES its winner (qsh
        CAP_WRITE), and `field_score_q15` is cosine x energy in Q15 — NOT a bare
        similarity, so it is not directly comparable to Kannaka's [0,1] score."""
        self._ensure_verified()
        rec = self.recall(query)                       # reinforces the winner
        khits = _kannaka.recall(query, top_k)
        return {
            "field": {"winner": rec.get("winner"), "score_q15": rec.get("score"),
                      "n": rec.get("n"), "reinforced": True,
                      "score_note": "cosine x energy in Q15, not a bare similarity"},
            "kannaka": [{"content": h.get("content"), "similarity": h.get("similarity")}
                        for h in khits if isinstance(h, dict)],
            "identity": self.identity(),
        }


# ============================================================================
# QosSociety — two attested VMs coupled into one field (epic #131 Phase C)
# ============================================================================
_societies = set()
_societies_lock = threading.RLock()   # RLock: the signal handler may re-enter
_society_signals_armed = False


def _society_register(soc):
    """Register a live society under the SINGLE process-wide reaper (one atexit +
    one signal handler that reaps EVERY society member) — never a per-VM handler,
    which would collide last-writer-wins and orphan the other VMs."""
    global _society_signals_armed
    with _societies_lock:
        _societies.add(soc)
        if not _society_signals_armed:
            atexit.register(_reap_all_societies)
            for sig in (signal.SIGTERM, signal.SIGINT):
                try:
                    signal.signal(sig, _society_signal_handler)
                except (ValueError, OSError):
                    pass  # not the main thread — atexit still covers a clean exit
            _society_signals_armed = True


def _society_unregister(soc):
    with _societies_lock:
        _societies.discard(soc)


def _reap_all_societies():
    with _societies_lock:
        socs = list(_societies)
    for soc in socs:
        try:
            soc.shutdown()
        except OSError:
            pass


def _society_signal_handler(*_):
    _reap_all_societies()
    os._exit(1)


class QosSociety:
    """Two attested QuantumOS VMs wired into one coupled field (epic #131). Each
    member boots NON-quiet with a static IP and a socket-netdev peer link, and
    its ghostd couples its holographic field to the peer's over UDP — the R_x
    cross-order parameter climbs to synchronization. The society verifies each
    member's COM2 Lamport attestation and ANNOTATES it verified/unverified.

    HONEST TRUST NOTE: attestation and coupling are cryptographically UNLINKED.
    The FSYN coupling wire carries NO identity (user/fieldsyncd.c), so a verified
    'synchronized' result proves two fields coupled ON THE LOOPBACK L2, NOT that
    the two COM2-attested identities coupled with each other. The coupling L2 is
    loopback-scoped but unauthenticated: trust reduces to control of the host
    (the same class as the single-VM serial channel). qseed is a host-assigned
    label, not a secret."""

    _NET_A, _NET_B = "10.0.0.1", "10.0.0.2"
    _MAC_A, _MAC_B = "52:54:00:00:c1:01", "52:54:00:00:c1:02"
    # N-way society (epic #139): N members share ONE broadcast L2 via a QEMU
    # mcast-socket netdev (the only single-NIC shared-L2 primitive — listen/
    # connect does NOT repeat frames across connections), each with a distinct
    # static IP + MAC, and each ghostd folds the MEAN field over its N-1 peers.
    _NET = ["10.0.0.1", "10.0.0.2", "10.0.0.3", "10.0.0.4"]
    _MAC = ["52:54:00:00:c1:01", "52:54:00:00:c1:02",
            "52:54:00:00:c1:03", "52:54:00:00:c1:04"]
    _MCAST_GROUP = "230.0.0.9"
    # ghostd/fieldsyncd emit these as ring-3 writes, so the kernel prefixes each
    # with "[user pid=N] " — they are NOT line-anchored (the proven Makefile gate
    # greps them unanchored too). Safe here: society VMs run no scripted qsh, so
    # there is no echoed input to forge them, and the [user pid=] prefix can't be.
    _R_X = re.compile(r"FIELDSYNC: R_x=(\d\.\d\d)")          # 1.00 matches \d\.\d\d
    _SYNC = re.compile(r"FIELDSYNC: SYNCHRONIZED")
    # Readiness = actual reception of the peer's phase frames (the un-fakeable
    # "the wire works both ways" signal the proven ci-smoke-fieldsync checks).
    _COUPLING = re.compile(r"FIELDSYNC: frame from ")

    def __init__(self, kernel=None):
        self.kernel = kernel
        self.a = None
        self.b = None
        self.members = []  # N-way society (epic #139); empty for the 2-VM path
        self._mcast_group = None  # set by boot_n; the replay-injection gate reads it
        self._mcast_port = None
        self._lock = threading.RLock()

    def is_running(self):
        if self.members:
            return all(m.is_running() for m in self.members)
        return (self.a is not None and self.a.is_running()
                and self.b is not None and self.b.is_running())

    def boot(self, qseed_a, qseed_b, expect_a=_UNSET, expect_b=_UNSET, timeout=45,
             extra_tokens=""):
        """Boot two coupled members. qseed_a != qseed_b is REQUIRED (identical
        qseeds are a duplicate attested identity and a vacuous instant 'sync').
        expect_a/expect_b verify against a DIFFERENT qseed (negative admission).
        extra_tokens: additional cmdline tokens appended to BOTH members (e.g.
        "agentdemo" for the society-of-societies gate, epic #178)."""
        with self._lock:
            if self.a is not None or self.b is not None:
                raise QosError("society already booted — shut it down first")
            if qseed_a == qseed_b:
                raise QosError("qseed_a and qseed_b must differ (distinct identities)")
            # Allocate BOTH coupling UDP ports while holding both sockets open, so
            # the two numbers are guaranteed DISTINCT; close them as late as we can
            # before QEMU binds them (the readiness check catches a lost race).
            s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s1.bind(("127.0.0.1", 0))
                s2.bind(("127.0.0.1", 0))
                port_a = s1.getsockname()[1]
                port_b = s2.getsockname()[1]
            finally:
                s1.close()
                s2.close()
            self.a = QosVM(kernel=self.kernel)
            self.b = QosVM(kernel=self.kernel)
            _society_register(self)          # reference held BEFORE booting either
            try:
                # Directed udp= crossing must be EXACT: A localaddr=port_a sends to
                # port_b; B mirrors. Get it backwards and phases flow one way only.
                extra = f" {extra_tokens.strip()}" if extra_tokens.strip() else ""
                self.a.boot(qseed=qseed_a, expect_qseed=expect_a, timeout=timeout,
                            quiet=False, arm_signals=False, mac=self._MAC_A,
                            append_extra=f"ip={self._NET_A} peer={self._NET_B}{extra}",
                            netdev=("socket,id=n0,"
                                    f"udp=127.0.0.1:{port_b},localaddr=127.0.0.1:{port_a}"))
                self.b.boot(qseed=qseed_b, expect_qseed=expect_b, timeout=timeout,
                            quiet=False, arm_signals=False, mac=self._MAC_B,
                            append_extra=f"ip={self._NET_B} peer={self._NET_A}{extra}",
                            netdev=("socket,id=n0,"
                                    f"udp=127.0.0.1:{port_a},localaddr=127.0.0.1:{port_b}"))
                # Both coupling NICs must come up — fieldsyncd logs this even under
                # quiet, so a lost port race surfaces HERE, not as a sync timeout.
                self._await_both(self._COUPLING, time.time() + 15, "coupling NIC up")
            except BaseException:
                self.shutdown()             # reap BOTH partially-booted members
                raise
            return self.status()

    def _await_both(self, marker, deadline, what):
        while time.time() < deadline:
            for vm, name in ((self.a, "A"), (self.b, "B")):
                if not vm.is_running():
                    raise QosDead(f"society node {name} exited before {what}")
            if marker.search(self.a._log_text()) and marker.search(self.b._log_text()):
                return
            time.sleep(0.2)
        raise QosTimeout(f"society: {what} not observed on both nodes")

    def _node_status(self, vm, since=0):
        # `since` anchors R_x / SYNCHRONIZED at a log OFFSET (a member's FSKEY
        # position), so a multi-phase gate reads only evidence produced AFTER the
        # phase boundary — the whole-log grep is sticky and would report a stale
        # pre-key SYNCHRONIZED forever (the panel's central observability finding).
        text = vm._log_text(since)
        rxs = self._R_X.findall(text)
        return {"identity": vm.identity(),
                "verified": vm.attestation.verified if vm.attestation else False,
                "r_x": float(rxs[-1]) if rxs else None,
                "r_x_min": min((float(x) for x in rxs), default=None),
                "synchronized": bool(self._SYNC.search(text)),
                "alive": vm.is_running()}

    def status(self):
        with self._lock:
            if self.a is None or self.b is None:
                raise QosError("no society booted")
            return {"a": self._node_status(self.a), "b": self._node_status(self.b),
                    "trust_note": (
                        "coupling and attestation are cryptographically UNLINKED: a "
                        "verified 'synchronized' proves two fields coupled on the "
                        "loopback L2, NOT that the two attested identities coupled "
                        "with each other. Trust == control of the host.")}

    def await_sync(self, threshold=0.80, timeout=90):
        """Poll until BOTH members' fields synchronize (R_x >= threshold), or a
        node dies (fail fast, distinct from a timeout), or the deadline passes."""
        with self._lock:
            if self.a is None:
                raise QosError("no society booted")
            deadline = time.time() + timeout
            while time.time() < deadline:
                for vm, name in ((self.a, "A"), (self.b, "B")):
                    if not vm.is_running():
                        raise QosDead(f"society node {name} exited during sync")
                st = self.status()
                if (st["a"]["synchronized"] and st["b"]["synchronized"]
                        and (st["a"]["r_x"] or 0) >= threshold
                        and (st["b"]["r_x"] or 0) >= threshold):
                    return st
                time.sleep(0.3)
            raise QosTimeout(f"society did not synchronize (>= {threshold}) in {timeout}s")

    # ---- N-way society (epic #139): N>=3 members on one mcast L2 -------------
    def boot_n(self, qseeds, expects=None, timeout=45, extra_tokens=""):
        """Boot N (>=3) attested members into ONE mean-field society on a shared
        mcast L2. Every qseed must be distinct (divergent starts → a non-vacuous
        R_x climb). `expects` (optional, per-member) verifies against a DIFFERENT
        qseed for negative admission. `extra_tokens` are appended to EVERY
        member's cmdline (e.g. "agentdemo" to run each node's local sub-agent
        society for the N-way society-of-societies gate). Each ghostd folds the
        MEAN field over its
        N-1 peers, so all N reaching min-pairwise R_x >= threshold is convergence
        two VMs structurally cannot fake."""
        with self._lock:
            n = len(qseeds)
            if n < 3 or n > len(self._NET):
                raise QosError(f"boot_n needs 3..{len(self._NET)} members, got {n}")
            if len(set(qseeds)) != n:
                raise QosError("all qseeds must differ (distinct divergent identities)")
            if self.a is not None or self.b is not None or self.members:
                raise QosError("society already booted — shut it down first")
            if expects is None:
                expects = [_UNSET] * n
            # One free UDP port for the whole mcast group (distinct groups/ports
            # across concurrent runs avoid cross-talk).
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.bind(("127.0.0.1", 0))
                mport = s.getsockname()[1]
            finally:
                s.close()
            # Randomize the mcast GROUP per boot, not just the port: concurrent
            # local runs on the identical static _NET IPs sharing one fixed
            # 230.0.0.9 group would cross-couple (the guests never see the group
            # address, so distinct ports alone don't isolate the L2). A random
            # 230.x.y.z admin-scope group makes each society's shared L2 disjoint.
            mgroup = "230.%d.%d.%d" % (random.randint(1, 254),
                                       random.randint(0, 255),
                                       random.randint(1, 254))
            self._mcast_group = mgroup   # exposed for the replay-injection gate
            self._mcast_port = mport
            netdev = f"socket,id=n0,mcast={mgroup}:{mport}"
            # Create + register ALL members BEFORE booting any, so a boot failure
            # on member k still reaps every already-forked QEMU (no orphan leak).
            self.members = [QosVM(kernel=self.kernel) for _ in range(n)]
            _society_register(self)
            try:
                extra = f" {extra_tokens.strip()}" if extra_tokens.strip() else ""
                for i in range(n):
                    peers = ",".join(self._NET[j] for j in range(n) if j != i)
                    self.members[i].boot(
                        qseed=qseeds[i], expect_qseed=expects[i], timeout=timeout,
                        quiet=False, arm_signals=False, mac=self._MAC[i],
                        append_extra=f"ip={self._NET[i]} peer={peers}{extra}", netdev=netdev)
                # mcast reachability precheck (FAIL LOUD — never a silent green):
                # every node must receive a frame from EACH of its N-1 peers (the
                # full mesh), or the shared L2 is not delivering. Scale the
                # deadline with N — an N-body full mesh needs more boots to settle
                # than a 3-body one, so a fixed 20s would flake load, not detect
                # absent multicast. Absent multicast still fails in <20s (no frame
                # from ANY peer arrives), distinguished by the missing-IP list.
                self._await_all_frames(time.time() + 10 + 5 * n)
            except QosTimeout as exc:
                self.shutdown()
                raise QosTimeout(f"N-way coupling mesh not established — is host "
                                 f"multicast available on this runner? ({exc})")
            except BaseException:
                self.shutdown()
                raise
            return self.status_n()

    def _member_peers(self, i):
        n = len(self.members)
        return [self._NET[j] for j in range(n) if j != i]

    def _await_all_frames(self, deadline):
        """Each member must log 'FIELDSYNC: frame from <ip>' for ALL of its peer
        IPs (the full N-cycle, per-IP — not a generic match that a 2-of-N
        masquerade could satisfy)."""
        missing = None
        while time.time() < deadline:
            for i, m in enumerate(self.members):
                if not m.is_running():
                    raise QosDead(f"society member {i} exited before coupling")
            missing = []
            for i, m in enumerate(self.members):
                text = m._log_text()
                for ip in self._member_peers(i):
                    if f"FIELDSYNC: frame from {ip}" not in text:
                        missing.append(f"member{i}<-{ip}")
            if not missing:
                return
            time.sleep(0.2)
        # List the unheard directed edges so a load-flake (a few edges missing,
        # frames flowing) reads differently from absent multicast (EVERY edge
        # missing — no frame from any peer reached any node).
        raise QosTimeout("not every member received a frame from each of its "
                         f"peers; unheard edges: {missing}")

    def status_n(self, since=None):
        # `since` (optional) is a PER-MEMBER list of log offsets — each member's
        # FSKEY position — so a keyed-phase gate anchors every member's status at
        # its own keying boundary. None => whole log (the boot-time default).
        with self._lock:
            if not self.members:
                raise QosError("no N-way society booted")
            offs = since if since is not None else [0] * len(self.members)
            if len(offs) != len(self.members):
                raise QosError("since must have one offset per member")
            return {"members": [self._node_status(m, offs[i])
                                for i, m in enumerate(self.members)],
                    "trust_note": (
                        "coupling and attestation are cryptographically UNLINKED: a "
                        "verified 'synchronized' proves N fields coupled on the shared "
                        "loopback L2, NOT that the N attested identities coupled. A "
                        "forged source on the L2 can skew (not starve — ghostd never "
                        "evicts a live peer) the mean. Trust == control of the host.")}

    def await_sync_n(self, threshold=0.80, timeout=120, since=None):
        """Poll until EVERY member's field synchronizes (min-pairwise R_x >=
        threshold), a member dies, or the deadline passes. `since` (per-member
        offset list) anchors the R_x/SYNCHRONIZED read at each member's keying
        boundary, so a post-admission sync is proven fresh — not read off a stale
        pre-key SYNCHRONIZED line still sitting in the sticky log."""
        with self._lock:
            if not self.members:
                raise QosError("no N-way society booted")
            deadline = time.time() + timeout
            while time.time() < deadline:
                for i, m in enumerate(self.members):
                    if not m.is_running():
                        raise QosDead(f"society member {i} exited during sync")
                st = self.status_n(since=since)
                if all(nd["synchronized"] and (nd["r_x"] or 0) >= threshold
                       for nd in st["members"]):
                    return st
                time.sleep(0.3)
            raise QosTimeout(f"N-way society did not synchronize (>= {threshold}) in {timeout}s")

    def _await_fskey(self, m, since_len, timeout):
        """Wait for a member to print an FSKEY line (install OR rekey) at/after
        `since_len`, and return the log offset just past THAT line — the anchor
        for post-keying exclusion/resume/sync checks. Fail fast on a dead member.

        The anchor is the end of the FSKEY line, not the log length when the poll
        sees it (#291): fieldsyncd prints the bad-MAC line ONCE (latched), often
        within the poll interval (or while an earlier member is being awaited),
        and an anchor taken later hid it from the leg-(a) check for good."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not m.is_running():
                raise QosDead("society member exited before installing the key")
            with m._log_lock:
                tail = bytes(m._log[since_len:])
            i = tail.find(b"FSKEY:")
            if i >= 0:
                nl = tail.find(b"\n", i)
                return since_len + (nl + 1 if nl >= 0 else len(tail))
            time.sleep(0.3)
        raise QosTimeout("society member did not install the session key")

    def admit_key_n(self, key, indices=None, timeout=25):
        """Admit the SAME host group session key (ADR-0019) to N members — the
        N-way generalization of the 2-VM keyauth `_wait_both`. Built on per-member
        admit_key(SAME key bytes): every admitted member gets the IDENTICAL 32
        bytes, so their tags verify against each other. NEVER attest() — it mints
        a fresh os.urandom key PER VM, which would leave the society mutually
        keyless (each member rejecting every other's tag), the exact trap the
        panel flagged. `indices` restricts admission to a subset (default all) so
        a caller can key a proper subset and observe the excluded member. Awaits
        each admitted member's FSKEY line and returns {index: post-FSKEY offset}
        so post-admission checks anchor at the keying boundary. Re-calling admits
        again (a rekey) — the FSKEY-since scan catches the fresh 'rekeyed' line."""
        with self._lock:
            if not self.members:
                raise QosError("no N-way society booted")
            key = bytes(key)
            idxs = list(range(len(self.members))) if indices is None else list(indices)
            pre = {i: self.members[i]._log_len() for i in idxs}
            for i in idxs:
                self.members[i].admit_key(key)   # SAME key to every member
            return {i: self._await_fskey(self.members[i], pre[i], timeout)
                    for i in idxs}

    def frame_from_count(self, member_index, src_ip, since=0):
        """Count 'FIELDSYNC: frame from <src_ip>' lines in a member's log since an
        offset — the per-source frame-ADMISSION signal (a keyless peer's frames
        stop being admitted once the member is keyed) the exclusion gate reads,
        the discriminator R_x cannot provide (a keyless follower still prints
        SYNCHRONIZED off the keyed pair's frames)."""
        with self._lock:
            text = self.members[member_index]._log_text(since)
            return text.count(f"FIELDSYNC: frame from {src_ip}")

    def shutdown(self):
        with self._lock:
            _society_unregister(self)
            for vm in list(reversed(self.members)) + [self.b, self.a]:
                if vm is not None:
                    try:
                        vm.shutdown()
                    except OSError:
                        pass
            self.a = self.b = None
            self.members = []
            return {"stopped": True}


def _pdeathsig():
    """Linux: die with the parent so a killed MCP process never orphans QEMU."""
    try:
        libc = ctypes.CDLL("libc.so.6", use_errno=True)
        PR_SET_PDEATHSIG = 1
        libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
    except OSError:
        pass
