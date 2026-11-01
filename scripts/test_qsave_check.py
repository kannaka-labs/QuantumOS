#!/usr/bin/env python3
"""Host tests for scripts/qsave_check.py (no QEMU needed). Run: python3 scripts/test_qsave_check.py"""
import hashlib
import hmac
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qsave_check as q  # noqa: E402

KEY = bytes(range(32))
RNG = bytes(range(100, 116))


class T(unittest.TestCase):
    def test_hmac_matches_rfc4231_case2(self):
        # Pins the primitive the C side (user/sha256.h) must equal.
        d = hmac.new(b"Jefe", b"what do ya want for nothing?", hashlib.sha256).hexdigest()
        self.assertEqual(d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

    def test_roundtrip_all_lengths(self):
        for n in (0, 1, 31, 32, 33, 64, 136, 1000):
            pt = bytes((i * 7) & 255 for i in range(n))
            b = q.seal(KEY, pt, RNG)
            self.assertEqual(len(b), 64 + n)
            self.assertEqual(q.open_blob(b, KEY), pt)

    def test_header_layout(self):
        b = q.seal(KEY, b"hello", RNG)
        self.assertEqual(b[:4], b"QSAV")
        self.assertEqual(b[4:8], bytes([1, 0, 1, 0]))
        self.assertEqual(int.from_bytes(b[24:32], "little"), 5)
        self.assertEqual(q.verify(b, KEY)["length"], 5)
        self.assertNotEqual(b[64:], b"hello")

    def test_nonce_is_synthetic(self):
        a = q.seal(KEY, b"A" * 64, RNG)
        b = q.seal(KEY, b"B" * 64, RNG)
        self.assertNotEqual(a[8:24], b[8:24])  # same rng, different pt -> different nonce

    def test_any_single_bit_flip_is_rejected(self):
        b = bytearray(q.seal(KEY, b"save-data-0123456789", RNG))
        for i in range(len(b)):
            for bit in (0, 7):
                m = bytearray(b)
                m[i] ^= 1 << bit
                with self.assertRaises(q.QsaveError, msg=f"byte {i} bit {bit}"):
                    q.verify(bytes(m), KEY)

    def test_wrong_key_truncation_extension(self):
        b = q.seal(KEY, b"x" * 40, RNG)
        for bad_blob, key in ((b, bytes(32)), (b[:-1], KEY), (b + b"\0", KEY), (b[:10], KEY)):
            with self.assertRaises(q.QsaveError):
                q.verify(bad_blob, key)

    def test_key_length_enforced(self):
        with self.assertRaises(q.QsaveError):
            q.derive(b"short")

    def test_log_extraction_with_console_framing(self):
        pt = b"x" * 150
        b = q.seal(KEY, pt, RNG)
        lines = ["[user pid=7] QSAVE: sealed 150 plaintext bytes"]
        for o in range(0, len(b), 64):
            lines.append("[user pid=7] QSAVE: blob " + b[o:o + 64].hex() + "\r")
        lines.append(f"[user pid=7] QSAVE: blob-end len={len(b)}\r")
        text = "\n".join(lines)
        got = q.blob_from_log(text)
        self.assertEqual(got, b)
        self.assertEqual(q.open_blob(got, KEY), pt)
        with self.assertRaises(q.QsaveError):
            q.blob_from_log(text.replace("blob-end", "nope"))
        with self.assertRaises(q.QsaveError):  # declared length disagrees
            q.blob_from_log(text.replace(f"len={len(b)}", f"len={len(b) + 1}"))

    def test_cli(self):
        d = tempfile.mkdtemp()
        p, o = os.path.join(d, "pt"), os.path.join(d, "blob")
        with open(p, "wb") as f:
            f.write(b"cli-save")
        self.assertEqual(q.main(["--key", KEY.hex(), "--seal", p, "--out", o]), 0)
        self.assertEqual(q.main(["--key", KEY.hex(), "--blob", o, "--expect", p]), 0)
        self.assertEqual(q.main(["--key", bytes(32).hex(), "--blob", o]), 1)


if __name__ == "__main__":
    unittest.main()

