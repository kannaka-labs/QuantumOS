"""Tests for backup_saves.py (#281 part 1): an in-memory bucket stands in for OCI."""
from __future__ import annotations

import json
import os
import stat
import sys
from pathlib import Path

import pytest

pytest.importorskip("cryptography")
sys.path.insert(0, str(Path(__file__).resolve().parent))
import backup_saves as bs  # noqa: E402

KEY = bytes(range(32))


class FakeBucket:
    def __init__(self):
        self.objects: dict[str, bytes] = {}
        self.puts = 0

    def list(self):
        return sorted(self.objects)

    def put(self, name, data):
        self.objects[name] = data
        self.puts += 1

    def get(self, name):
        return self.objects[name]

    def delete(self, name):
        del self.objects[name]


def machine(saves: Path, sid: str, body: bytes, last_used=1.0):
    saves.mkdir(exist_ok=True)
    (saves / f"{sid}.img").write_bytes(b"QDSK1" + body)
    (saves / f"{sid}.json").write_text(json.dumps({"created": 1.0, "last_used": last_used}))


def test_seal_roundtrip_and_tamper_detection():
    blob = bs.seal(KEY, "a.qosb", {"x": 1}, b"QDSK1disk")
    assert b"QDSK1disk" not in blob, "the image must not be stored in the clear"
    assert bs.unseal(KEY, "a.qosb", blob) == ({"x": 1}, b"QDSK1disk")
    with pytest.raises(ValueError, match="authentication"):
        bs.unseal(KEY, "b.qosb", blob)                       # renamed / swapped object
    with pytest.raises(ValueError, match="authentication"):
        bs.unseal(bytes(32), "a.qosb", blob)                 # wrong key
    flipped = blob[:-1] + bytes([blob[-1] ^ 1])
    with pytest.raises(ValueError, match="authentication"):
        bs.unseal(KEY, "a.qosb", flipped)
    with pytest.raises(ValueError, match="not a qos backup"):
        bs.unseal(KEY, "a.qosb", b"junk")
    assert bs.seal(KEY, "a.qosb", {}, b"x") != bs.seal(KEY, "a.qosb", {}, b"x"), "fresh nonce each time"


def test_sync_uploads_changes_mirrors_deletes_and_restores(tmp_path):
    saves, state, bucket = tmp_path / "saves", tmp_path / "state.json", FakeBucket()
    machine(saves, "aa", b"one")
    machine(saves, "bb", b"two")
    machine(saves, "cc", b"three")
    assert bs.sync(saves, bucket, KEY, state)["uploaded"] == 3
    assert bucket.list() == ["aa.qosb", "bb.qosb", "cc.qosb"]
    assert bs.sync(saves, bucket, KEY, state)["uploaded"] == 0, "unchanged machines are not re-sent"
    machine(saves, "aa", b"one-changed", last_used=2.0)
    assert bs.sync(saves, bucket, KEY, state)["uploaded"] == 1
    for f in ("bb.img", "bb.json"):
        (saves / f).unlink()                                  # expired locally
    res = bs.sync(saves, bucket, KEY, state)
    assert res["deleted"] == 1 and bucket.list() == ["aa.qosb", "cc.qosb"]
    out = tmp_path / "restored"
    assert bs.restore(out, bucket, KEY) == 2
    assert (out / "aa.img").read_bytes() == b"QDSK1one-changed"
    assert json.loads((out / "aa.json").read_text())["last_used"] == 2.0
    assert all(stat.S_IMODE(p.stat().st_mode) == 0o600 for p in out.iterdir())
    assert stat.S_IMODE(state.stat().st_mode) == 0o600


def test_a_lost_store_never_erases_the_backup(tmp_path):
    saves, state, bucket = tmp_path / "saves", tmp_path / "state.json", FakeBucket()
    for i in range(4):
        machine(saves, f"m{i}", b"x%d" % i)
    bs.sync(saves, bucket, KEY, state)
    for p in saves.iterdir():
        p.unlink()                                            # the store was wiped, the state survived
    res = bs.sync(saves, bucket, KEY, state)
    assert res["delete_guarded"] == 4 and res["deleted"] == 0 and len(bucket.list()) == 4
    machine(saves, "m0", b"x0")                               # only 1 of 4 left: still too many to delete
    res = bs.sync(saves, bucket, KEY, state)
    assert res["delete_guarded"] == 3 and len(bucket.list()) == 4
    state.unlink()                                            # the disk died: store and state both gone
    for p in saves.iterdir():
        p.unlink()
    res = bs.sync(saves, bucket, KEY, state)
    assert res["deleted"] == 0 and res["unknown_remote"] == 4 and len(bucket.list()) == 4


def test_the_last_machine_expiring_is_deleted(tmp_path):
    saves, state, bucket = tmp_path / "saves", tmp_path / "state.json", FakeBucket()
    machine(saves, "only", b"x")
    bs.sync(saves, bucket, KEY, state)
    for p in saves.iterdir():
        p.unlink()
    res = bs.sync(saves, bucket, KEY, state)
    assert res["deleted"] == 1 and bucket.list() == [] and json.loads(state.read_text()) == {}


def test_keygen_never_replaces_a_key(tmp_path):
    assert bs.main(["keygen", "--key", str(tmp_path / "k")]) == 0
    assert os.path.getsize(tmp_path / "k") == 32 and stat.S_IMODE((tmp_path / "k").stat().st_mode) == 0o600
    with pytest.raises(SystemExit):
        bs.keygen(tmp_path / "k")                             # never replaces an existing key


def test_incomplete_machines_are_skipped(tmp_path):
    saves, bucket = tmp_path / "saves", FakeBucket()
    saves.mkdir()
    (saves / "half.json").write_text("{}")                    # a keep that was never synced
    (saves / "stray.img").write_bytes(b"x")
    assert bs.sync(saves, bucket, KEY, tmp_path / "s.json")["uploaded"] == 0 and bucket.list() == []
