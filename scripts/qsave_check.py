#!/usr/bin/env python3
"""Host-side checker for QSAV1 encrypted saves (#281 slice 1, ADR-0034).

Parses the self-describing header, verifies the HMAC-SHA256 MAC against a known
key BEFORE decrypting, and optionally decrypts and compares against an expected
plaintext. Mirrors user/qsave.h byte for byte using only hashlib/hmac.

Blob layout (little-endian): magic "QSAV"(4) version u16(=1) suite u16(=1)
nonce(16) plaintext-length u64 | mac(32) | ciphertext(length). The MAC covers
header[0:32] || ciphertext.

  qsave_check.py --key HEX64 --blob FILE            verify (and print header)
  qsave_check.py --key HEX64 --log qemu.log         blob lines "QSAVE: blob <hex>" from a boot log
  qsave_check.py ... --expect FILE                  also decrypt and compare to FILE
  qsave_check.py --key HEX64 --seal FILE --out OUT  host-side seal (test fixtures)
Exit 0 only on a verified blob (and a matching plaintext when --expect is given).
"""
from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import re
import struct
import sys

MAGIC = b"QSAV"
VERSION = 1
SUITE = 1
HDR_FIELDS = 32
HDR_LEN = 64


class QsaveError(Exception):
    pass


def _hmac(key: bytes, *parts: bytes) -> bytes:
    h = hmac.new(key, digestmod=hashlib.sha256)
    for p in parts:
        h.update(p)
    return h.digest()


def derive(master: bytes):
    if len(master) != 32:
        raise QsaveError("key must be 32 bytes (64 hex chars)")
    return (_hmac(master, b"QSAVE1-enc"), _hmac(master, b"QSAVE1-mac"),
            _hmac(master, b"QSAVE1-nonce"))


def keystream_xor(enc_key: bytes, nonce: bytes, data: bytes) -> bytes:
    out = bytearray(len(data))
    for blk in range((len(data) + 31) // 32):
        ks = _hmac(enc_key, nonce + struct.pack("<Q", blk))
        chunk = data[blk * 32:(blk + 1) * 32]
        for i, b in enumerate(chunk):
            out[blk * 32 + i] = b ^ ks[i]
    return bytes(out)


def parse_header(blob: bytes) -> dict:
    if len(blob) < HDR_LEN:
        raise QsaveError(f"blob too short ({len(blob)} < {HDR_LEN})")
    if blob[:4] != MAGIC:
        raise QsaveError("bad magic")
    version, suite = struct.unpack_from("<HH", blob, 4)
    if version != VERSION:
        raise QsaveError(f"unsupported version {version}")
    if suite != SUITE:
        raise QsaveError(f"unsupported suite {suite}")
    (length,) = struct.unpack_from("<Q", blob, 24)
    if length != len(blob) - HDR_LEN:
        raise QsaveError(f"length field {length} != ciphertext bytes {len(blob) - HDR_LEN}")
    return {"version": version, "suite": suite, "nonce": blob[8:24], "length": length,
            "mac": blob[32:64]}


def verify(blob: bytes, master: bytes) -> dict:
    """Return the parsed header if the MAC verifies; raise QsaveError otherwise."""
    hdr = parse_header(blob)
    _, mac_key, _ = derive(master)
    want = _hmac(mac_key, blob[:HDR_FIELDS], blob[HDR_LEN:])
    if not hmac.compare_digest(want, hdr["mac"]):
        raise QsaveError("MAC mismatch: wrong key or tampered blob")
    return hdr


def open_blob(blob: bytes, master: bytes) -> bytes:
    hdr = verify(blob, master)
    enc_key, _, _ = derive(master)
    return keystream_xor(enc_key, hdr["nonce"], blob[HDR_LEN:])


def seal(master: bytes, plaintext: bytes, rng: bytes) -> bytes:
    if len(rng) != 16:
        raise QsaveError("rng must be 16 bytes")
    enc_key, mac_key, nonce_key = derive(master)
    nonce = _hmac(nonce_key, rng, plaintext)[:16]
    fields = (MAGIC + struct.pack("<HH", VERSION, SUITE) + nonce
              + struct.pack("<Q", len(plaintext)))
    ct = keystream_xor(enc_key, nonce, plaintext)
    return fields + _hmac(mac_key, fields, ct) + ct


def blob_from_log(text: str) -> bytes:
    """Concatenate the 'QSAVE: blob <hex>' lines of the LAST seal in a console log."""
    chunks, last = [], None
    for line in text.splitlines():
        m = re.search(r"QSAVE: blob ([0-9a-fA-F]+)\s*$", line)
        if m:
            chunks.append(m.group(1))
            continue
        m = re.search(r"QSAVE: blob-end len=(\d+)", line)
        if m and chunks:
            last = (bytes.fromhex("".join(chunks)), int(m.group(1)))
            chunks = []
    if last is None:
        raise QsaveError("no complete 'QSAVE: blob ... blob-end' sequence in the log")
    blob, declared = last
    if len(blob) != declared:
        raise QsaveError(f"log blob is {len(blob)} bytes but blob-end says {declared}")
    return blob


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--key", required=True, help="32-byte master key, 64 hex chars")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--blob")
    src.add_argument("--log")
    src.add_argument("--seal", help="plaintext file to seal on the host")
    ap.add_argument("--out", help="with --seal: where to write the blob")
    ap.add_argument("--expect", help="plaintext file the decrypted blob must equal")
    a = ap.parse_args(argv)
    try:
        master = bytes.fromhex(a.key)
        if a.seal:
            if not a.out:
                raise QsaveError("--seal needs --out")
            with open(a.seal, "rb") as f:
                blob = seal(master, f.read(), os.urandom(16))
            with open(a.out, "wb") as f:
                f.write(blob)
            print(f"QSAVE-CHECK: sealed {len(blob)} bytes to {a.out}")
            return 0
        if a.blob:
            with open(a.blob, "rb") as f:
                blob = f.read()
        else:
            with open(a.log, encoding="utf-8", errors="replace") as f:
                blob = blob_from_log(f.read())
        hdr = verify(blob, master)
        print(f"QSAVE-CHECK: header ok version={hdr['version']} suite={hdr['suite']} "
              f"nonce={hdr['nonce'].hex()} length={hdr['length']}")
        print("QSAVE-CHECK: MAC verified")
        if a.expect:
            pt = open_blob(blob, master)
            with open(a.expect, "rb") as f:
                if pt != f.read():
                    raise QsaveError("decrypted plaintext differs from --expect file")
            print(f"QSAVE-CHECK: plaintext matches {a.expect} ({len(pt)} bytes)")
    except (QsaveError, ValueError, OSError) as e:
        print(f"QSAVE-CHECK: FAILED: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

