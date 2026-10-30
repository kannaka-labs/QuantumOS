#!/usr/bin/env python3
"""Encrypted off-site copies of the console's kept machines (#281, part 1).

  backup_saves.py sync    --saves /var/lib/qos-console/saves --config /etc/qos-backup/backup.json
  backup_saves.py restore --to DIR --config /etc/qos-backup/backup.json
  backup_saves.py keygen  --key /etc/qos-backup/backup.key

`sync` mirrors the save store (saves.py) into one private object-storage bucket:
every kept machine is uploaded encrypted, a machine changed since the last upload is
uploaded again, and a machine deleted locally (expired) is deleted from the bucket.
`restore` downloads and decrypts every machine into DIR, the shape --save-dir expects.

Encryption happens before anything leaves the host: AES-256-GCM under a 32-byte key
that lives only on hosts we control (keep an offline copy: without it the bucket is
noise). Each object holds one machine's metadata and disk image together:

    b"QOSB1" | 12-byte nonce | AES-GCM( u32 meta length | meta JSON | image )

with the object name as associated data, so an object renamed or swapped in the
bucket fails to decrypt instead of restoring as another visitor's machine. Object
names are the store's ids (sha256 of a resume key): they reveal nothing.

Deletion is mirrored, but only for machines this host itself uploaded (the state file
records them) and has since lost: a dead disk loses that record too and deletes
nothing, and objects of unknown origin are left alone. If more than
--max-delete-fraction of the bucket (and more than two machines) would go at once, the
store looks wiped: sync deletes nothing and exits 3. A wiped disk must never erase the
backup.

backup.json: {"key_file": ..., "oci_config": ..., "oci_profile": "DEFAULT",
              "namespace": ..., "bucket": ..., "state_file": ...}
The state file records the sha256 of each machine as last uploaded, so unchanged
machines are not uploaded again.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import secrets
import struct
import sys
from pathlib import Path

log = logging.getLogger("qos-backup")

MAGIC = b"QOSB1"
NONCE = 12
SUFFIX = ".qosb"


def _aes(key: bytes):
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    if len(key) != 32:
        raise ValueError("backup key must be 32 bytes")
    return AESGCM(key)


def seal(key: bytes, name: str, meta: dict, image: bytes) -> bytes:
    m = json.dumps(meta, sort_keys=True).encode()
    nonce = secrets.token_bytes(NONCE)
    plain = struct.pack(">I", len(m)) + m + image
    return MAGIC + nonce + _aes(key).encrypt(nonce, plain, name.encode())


def unseal(key: bytes, name: str, blob: bytes) -> tuple[dict, bytes]:
    """Raises ValueError for anything that is not an intact object of this name."""
    from cryptography.exceptions import InvalidTag
    if not blob.startswith(MAGIC) or len(blob) < len(MAGIC) + NONCE + 16 + 4:
        raise ValueError("not a qos backup object")
    nonce = blob[len(MAGIC):len(MAGIC) + NONCE]
    try:
        plain = _aes(key).decrypt(nonce, blob[len(MAGIC) + NONCE:], name.encode())
    except InvalidTag:
        raise ValueError("object fails authentication (wrong key, tampered, or renamed)") from None
    (n,) = struct.unpack(">I", plain[:4])
    return json.loads(plain[4:4 + n]), plain[4 + n:]


# ---- object store -------------------------------------------------------------
class OciBucket:
    """The four calls sync and restore need, on one OCI Object Storage bucket."""

    def __init__(self, config_file: str, profile: str, namespace: str, bucket: str):
        import oci
        cfg = oci.config.from_file(config_file, profile)
        self.client = oci.object_storage.ObjectStorageClient(cfg)
        self.ns, self.bucket = namespace, bucket

    def list(self) -> list[str]:
        out, start = [], None
        while True:
            r = self.client.list_objects(self.ns, self.bucket, start=start, limit=1000).data
            out += [o.name for o in r.objects]
            if not r.next_start_with:
                return out
            start = r.next_start_with

    def put(self, name: str, data: bytes) -> None:
        self.client.put_object(self.ns, self.bucket, name, data)

    def get(self, name: str) -> bytes:
        return self.client.get_object(self.ns, self.bucket, name).data.content

    def delete(self, name: str) -> None:
        self.client.delete_object(self.ns, self.bucket, name)


# ---- sync / restore -----------------------------------------------------------
def local_machines(saves: Path) -> dict[str, tuple[Path, Path]]:
    """id -> (image, meta) for every complete machine in the store."""
    out = {}
    for meta in saves.glob("*.json"):
        img = meta.with_suffix(".img")
        if img.exists():
            out[meta.stem] = (img, meta)
    return out


def load_state(path: Path) -> dict[str, str]:
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def save_state(path: Path, state: dict[str, str]) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(state, sort_keys=True))
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def sync(saves: Path, store, key: bytes, state_file: Path, max_delete_fraction: float = 0.5) -> dict:
    state = load_state(state_file)
    known = set(state)                      # what this host uploaded before this run
    local = local_machines(saves)
    remote = {n[:-len(SUFFIX)] for n in store.list() if n.endswith(SUFFIX)}
    up = 0
    for sid, (img, meta_p) in sorted(local.items()):
        image = img.read_bytes()
        meta = json.loads(meta_p.read_text())
        digest = hashlib.sha256(image + json.dumps(meta, sort_keys=True).encode()).hexdigest()
        if state.get(sid) == digest and sid in remote:
            continue
        store.put(sid + SUFFIX, seal(key, sid + SUFFIX, meta, image))
        state[sid] = digest
        up += 1
    # Only machines this host uploaded (recorded in the state) and has since lost are
    # deleted. A dead disk loses the state too, so it deletes nothing; a store wiped while
    # the state survived trips the fraction guard (a few at a time are always allowed,
    # so the last machine expiring is still deleted).
    gone = sorted(sid for sid in remote - set(local) if sid in known)
    guarded = len(gone) > 2 and len(gone) > max_delete_fraction * len(remote)
    deleted = 0
    if not guarded:
        for sid in gone:
            store.delete(sid + SUFFIX)
            state.pop(sid, None)
            deleted += 1
    for sid in list(state):
        if sid not in local and sid not in remote:
            state.pop(sid)
    save_state(state_file, state)
    return {"local": len(local), "uploaded": up, "deleted": deleted,
            "delete_guarded": len(gone) if guarded else 0,
            "unknown_remote": len(remote - set(local) - known)}


def restore(to: Path, store, key: bytes) -> int:
    to.mkdir(mode=0o700, parents=True, exist_ok=True)
    n = 0
    for name in store.list():
        if not name.endswith(SUFFIX):
            continue
        sid = name[:-len(SUFFIX)]
        meta, image = unseal(key, name, store.get(name))
        for path, data in ((to / f"{sid}.img", image), (to / f"{sid}.json", json.dumps(meta).encode())):
            tmp = path.with_name(path.name + ".tmp")
            tmp.write_bytes(data)
            os.chmod(tmp, 0o600)
            os.replace(tmp, path)
        n += 1
    return n


def keygen(path: Path) -> None:
    if path.exists():
        raise SystemExit(f"{path} exists: refusing to replace a backup key")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(secrets.token_bytes(32))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sync", help="encrypt and mirror the saved machines to the bucket, deleting what was deleted here")
    s.add_argument("--saves", required=True, help="the launcher's save directory")
    s.add_argument("--config", required=True, help="backup config JSON (bucket, key file, OCI profile)")
    s.add_argument("--max-delete-fraction", type=float, default=0.5,
                   help="refuse a sync that would delete more than this fraction of the bucket (default 0.5)")
    r = sub.add_parser("restore", help="decrypt the bucket's copy into a directory")
    r.add_argument("--to", required=True, help="destination directory (created)")
    r.add_argument("--config", required=True, help="backup config JSON")
    k = sub.add_parser("keygen", help="create the encryption key file (0600) the other commands use")
    k.add_argument("--key", required=True, help="path of the key file to create")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", stream=sys.stdout)
    if a.cmd == "keygen":
        keygen(Path(a.key))
        return 0
    c = json.loads(Path(a.config).read_text())
    key = Path(c["key_file"]).read_bytes()
    store = OciBucket(c["oci_config"], c.get("oci_profile", "DEFAULT"), c["namespace"], c["bucket"])
    if a.cmd == "sync":
        res = sync(Path(a.saves), store, key, Path(c["state_file"]), a.max_delete_fraction)
        log.info("backup sync %s", json.dumps(res))
        return 3 if res["delete_guarded"] else 0
    n = restore(Path(a.to), store, key)
    log.info("restored %d machines into %s", n, a.to)
    return 0


if __name__ == "__main__":
    sys.exit(main())
