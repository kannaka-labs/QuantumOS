"""Saved machines for the QuantumOS public console (#256).

A visitor who presses "Keep this machine" gets a resume token, shown once. What they
`sync` inside QuantumOS lands on their VM's virtio-blk disk (#255); when the session
ends, that disk image is kept here under the token. A later session that presents
the token cold-boots with the image attached, so the files, the field and the audit
ledger come back. The token is the only key: no account, no email, no address.

What is stored, per machine: <id>.img (the disk image, --save-mib in size) and
<id>.json (created and last-used times). <id> is the sha256 of the token, so the
store never holds a token, and a copy of the directory opens nothing. The images are
not encrypted: the operator can read them, and the page says so.

Limits: a machine unused for --save-days is deleted; the store holds at most
--save-total-mib of images (a new keep is refused when it is full, never by evicting
someone else's machine); each visitor address may keep --saves-per-ip-day machines per
UTC day (counted under the launcher's per-process address hash, in memory only); and a
machine is open in at most one session at a time.

A keep that is never synced stores nothing: at the end of the session, an image that
does not start with the QDSK superblock magic is not kept, and the token stops working.
"""
from __future__ import annotations

import datetime
import hashlib
import json
import logging
import os
import secrets
import shutil
import time
from pathlib import Path

log = logging.getLogger("qos-console")

QDSK_MAGIC = b"QDSK1"     # ramfs.c: the superblock a synced QuantumOS volume starts with
TOKEN_BYTES = 16          # 128-bit resume tokens


class SaveRefused(Exception):
    """A keep or resume that cannot be honoured; str() is the reason code."""


def id_of(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def synced(image: Path) -> bool:
    """True if the image carries a QuantumOS volume (the guest ran `sync` at least once)."""
    try:
        with open(image, "rb") as f:
            return f.read(len(QDSK_MAGIC)) == QDSK_MAGIC
    except OSError:
        return False


class SaveStore:
    def __init__(self, root: Path, image_mib: int = 4, days: float = 7, total_mib: int = 2048,
                 per_ip_day: int = 3, clock=time.time):
        self.root = root
        self.image_mib = image_mib
        self.days = days
        self.total_mib = total_mib
        self.per_ip_day = per_ip_day
        self.clock = clock
        self.open: set[str] = set()                 # ids checked out by a running session
        self.kept_today: dict[str, int] = {}        # address hash -> keeps, for self._day only
        self._day = ""
        root.mkdir(mode=0o700, parents=True, exist_ok=True)

    # ---- bookkeeping ----
    def _meta(self, sid: str) -> Path:
        return self.root / f"{sid}.json"

    def image(self, sid: str) -> Path:
        return self.root / f"{sid}.img"

    def ids(self) -> list[str]:
        return [p.stem for p in self.root.glob("*.json")]

    def count(self) -> int:
        return len(self.ids())

    def _write_meta(self, sid: str, meta: dict) -> None:
        tmp = self._meta(sid).with_suffix(".json.tmp")
        tmp.write_text(json.dumps(meta))
        os.chmod(tmp, 0o600)
        os.replace(tmp, self._meta(sid))

    def _read_meta(self, sid: str) -> dict | None:
        try:
            return json.loads(self._meta(sid).read_text())
        except (OSError, ValueError):
            return None

    def _delete(self, sid: str) -> None:
        for p in (self.image(sid), self._meta(sid)):
            p.unlink(missing_ok=True)

    # ---- keep / resume / end ----
    def keep(self, ip_hash: str) -> tuple[str, str]:
        """Reserve a new saved machine for a running session. Returns (token, id).
        Raises SaveRefused("storage_full" | "too_many_today")."""
        day = datetime.datetime.fromtimestamp(self.clock(), datetime.timezone.utc).strftime("%Y-%m-%d")
        if day != self._day:
            self._day, self.kept_today = day, {}
        if self.kept_today.get(ip_hash, 0) >= self.per_ip_day:
            raise SaveRefused("too_many_today")
        if (self.count() + 1) * self.image_mib > self.total_mib:
            raise SaveRefused("storage_full")
        token = secrets.token_urlsafe(TOKEN_BYTES)
        sid = id_of(token)
        now = self.clock()
        self._write_meta(sid, {"created": now, "last_used": now})
        self.kept_today[ip_hash] = self.kept_today.get(ip_hash, 0) + 1
        self.open.add(sid)
        return token, sid

    def checkout(self, token: str) -> str:
        """Open a saved machine for a session. Returns its id. Raises
        SaveRefused("unknown_machine" | "machine_open")."""
        sid = id_of(token)
        meta = self._read_meta(sid)
        if meta is None or not self.image(sid).exists() or self._expired(meta):
            raise SaveRefused("unknown_machine")
        if sid in self.open:
            raise SaveRefused("machine_open")
        self.open.add(sid)
        return sid

    def checkin(self, sid: str, disk: Path) -> bool:
        """End of a session that kept or resumed machine `sid`: keep the session's disk if
        it carries a synced volume. Returns True if the machine is kept. A machine that
        was never synced (no image yet, and none now) is forgotten."""
        try:
            if synced(disk):
                tmp = self.image(sid).with_suffix(".img.tmp")
                shutil.copyfile(disk, tmp)
                os.chmod(tmp, 0o600)
                os.replace(tmp, self.image(sid))
                meta = self._read_meta(sid) or {"created": self.clock()}
                meta["last_used"] = self.clock()
                self._write_meta(sid, meta)
                return True
            if self.image(sid).exists():     # resumed, but this disk is not ours: keep the old one
                meta = self._read_meta(sid) or {"created": self.clock()}
                meta["last_used"] = self.clock()
                self._write_meta(sid, meta)
                return True
            self._delete(sid)
            return False
        except OSError as e:
            log.warning("save checkin failed save=%s: %s", sid[:8], e)
            return self.image(sid).exists()
        finally:
            self.open.discard(sid)

    # ---- retention ----
    def _expired(self, meta: dict) -> bool:
        return self.clock() - float(meta.get("last_used", 0)) > self.days * 86400

    def sweep(self) -> int:
        """Delete machines unused for --save-days, reservations a crash left behind, and
        stray temp files. Never touches a machine that is open."""
        n = 0
        for p in self.root.glob("*.tmp"):
            p.unlink(missing_ok=True)
        for sid in self.ids():
            if sid in self.open:
                continue
            meta = self._read_meta(sid)
            if meta is None or self._expired(meta) or not self.image(sid).exists():
                self._delete(sid)
                n += 1
        for img in self.root.glob("*.img"):          # an image whose metadata is gone
            if not self._meta(img.stem).exists() and img.stem not in self.open:
                img.unlink(missing_ok=True)
        return n
