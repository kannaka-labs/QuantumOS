# Persistent Storage: ATA or virtio-blk disk, writable overlay, survival across reboots

Epic #71. Until now everything QuantumOS held evaporated at power-off:
the initrd is read-only, and there was no block device. This epic adds
the storage half of a working OS — a disk driver, a writable filesystem,
and the property that ties them together: **files written in one boot
come back in the next.**

## Phase 1 — ATA PIO block driver (`kernel/src/ata.c`)

Polled LBA28 on the primary master (I/O base `0x1F0`). No interrupts
(nIEN set), no DMA, no slave, no ATAPI — the narrowest driver that makes
persistence real.

- **Every wait is bounded** (the `console_write` lesson): a wedged
  controller costs a bounded spin and an error return, never a hung
  kernel. On any timeout or protocol error the channel is *latched dead*
  (`disk_present = 0`) — leaving it mid-protocol could let a later
  command drain a previous command's data and return the wrong sector as
  success, which for a persistence store is worse than failing.
- **Cache-flush ordering**: `write_sector` completes the WRITE
  handshake (`BSY` clear, no `ERR`/`DF`) before any further command;
  `ata_write` issues exactly **one** `CACHE FLUSH` after the whole run of
  sectors has drained — not one per sector, which would be one host
  fsync per sector. (A flush issued while the drive is still `BSY` from
  the write is silently ignored by QEMU and illegal on real hardware.)
- **Diskless is honest**: `ata_init` logs `ATA: no disk — persistence
  disabled` and everything above it is unaffected — the default `-kernel`
  CI path boots exactly as before.
- **Boot RW self-test**: proves the write path by writing a pattern to
  the last sector, reading it back, and restoring the original — but the
  destructive write runs *only* when that sector is safe to clobber (all
  zero, or the volume already carries our `QDSK1` superblock). A foreign
  disk gets a non-destructive read probe, so a crash between the pattern
  write and the restore can never corrupt a user's data. Since #255 the
  self-test lives in `blkdev.c` and runs on whichever disk bound, under
  that driver's tag (`ATA: sector RW self-test OK ...`,
  `BLK: sector RW self-test OK ...`).

## Block-device indirection and virtio-blk (#255)

Firecracker has no ATA and no PCI, only virtio-mmio devices, so until #255
nothing persisted under Firecracker (the public console and the debain2
microVMs). Two pieces fix that:

- **`blkdev_ops_t`** (`kernel/include/kernel/blkdev.h`, `kernel/src/blkdev.c`):
  `present`, `sector_count`, `read`, `write`, bound once at boot like
  `netdev`. `ramfs.c` calls only `blkdev_*`. The contract every driver
  keeps: 512-byte sectors, bounded waits, and **a write is durable when it
  returns** (the driver flushes before reporting success). Probe order:
  virtio-blk, then ATA. A diskless boot still logs only `ATA: no disk`.
- **virtio-blk** (`kernel/src/virtio_blk.c`) over both virtio transports:
  virtio-mmio (Firecracker `drives`) and virtio-pci (QEMU `-drive
  ...,if=virtio`). Fully polled: one request in flight, three descriptors
  (header, data, status), at most 32 KiB per request, bounded wait. The
  avail ring asks for no interrupts and any that arrives is acknowledged,
  so a shared INTx line never stays asserted. FLUSH is negotiated when
  offered and issued once after each write run. A device without FLUSH
  (Firecracker's default `cache_type` "Unsafe") is logged:
  `BLK: device offers no FLUSH - writes are durable only as the host makes them`.
  A read-only drive (`VIRTIO_BLK_F_RO`) binds, and `sync` fails.
- **Transports bind several devices** (ADR-0024 amended): each virtio-mmio
  slot and each virtio-pci function is its own binding, and every transport
  op takes the binding. Before this, each transport kept one device's
  registers in a global, so probing a disk would have re-pointed the NIC's.

Gates: `ci-smoke-disk-virtio` runs the whole `ci-smoke-disk` recipe
(foreign-volume refusal, two-boot fs/field/audit readback, both
corruption-isolation checks) on `if=virtio`. `ci-smoke-net-virtio` now
attaches a virtio disk next to the virtio NIC and requires both to bind and
the full network suite to pass.

## Phase 2 — writable RAM filesystem overlay (`kernel/src/ramfs.c`)

A bounded table (16 files × 64 KiB, kmalloc on create, kfree on unlink)
layered over the read-only initrd. Lookups hit the overlay first, so a
written file shadows an initrd file of the same path; `readdir` merges
both (overlay rows tagged `[ram]`).

New syscalls:

| Call | Purpose |
|---|---|
| `SYS_OPEN` gains flags (rsi) | `O_WRONLY\|O_CREAT\|O_TRUNC` — create/append on the overlay |
| `SYS_FWRITE` (23) | append through a write-opened fd |
| `SYS_UNLINK` (24) | remove an overlay file, free its storage |
| `SYS_SYNC` (25) | flush the overlay to disk |

**Writing is real authority.** Unlike the VFS reads, any write-intent
open, `SYS_UNLINK`, and `SYS_SYNC` require a `CAP_RESOURCE_DEVICE` write
capability over `DEVICE_ID_DISK` — volume-level filesystem-write
authority granted declaratively to `qsh` alone (`grant_disk`, minted
**unconditionally**, so writes work diskless and only `sync` fails on the
missing disk). The capless `ghost_test` proves the denial by attack every
boot (`FSW: capless caller denied (EPERM)`).

**Safety details worth knowing:**

- fd entries carry `writable` + `ram_idx`; every open path sets *all*
  fields and `close` resets them, so a read-only reopen of a slot can
  never inherit a stale write capability.
- A write-intent open **reserves a free fd before touching the overlay**.
  `ramfs_create` either truncates an existing file to zero or creates a new
  slot, so doing it before the fd install would, on a full fd table, leave the
  overlay half-mutated while returning `EIO` — silent data loss, or a phantom
  file that persists to disk — a failure the caller reads as "open failed, file
  untouched". Reserving the fd up front makes a failed open side-effect-free
  (POSIX `EMFILE`-before-`O_TRUNC` semantics).
- `SYS_UNLINK` refuses (with EIO) while any *live* process holds the file
  open — a terminated process's stale fds don't count, and
  `process_destroy` clears fds so they can't pin a file forever.
- The shell gains `write <path> <text>`, `rm <path>`, `sync`.

## Phase 3 — persistence (`persist_sync` / `persist_restore`)

On-disk layout: LBA 0 is a superblock (`QDSK1` magic, archive byte count,
file count, and an additive checksum over the archive); LBA 1.. is the
overlay serialized as a POSIX ustar archive — the *same format* the
initrd parser already walks (the walker is shared via `kernel/tar.h`).

**Crash consistency** is two layers deep:

1. **Write ordering** — the archive is written (and flushed) *first*, the
   superblock *last*. A crash before the superblock leaves the old
   superblock pointing at the old, intact archive.
2. **Checksums** — the superblock carries a whole-archive checksum, and
   every ustar header carries its standard checksum (now *verified* by
   the shared walker). A torn archive write (superblock committed, data
   interrupted) is caught by a mismatch at restore, which then starts
   empty rather than restoring interleaved garbage as truth.

Restore is as paranoid as the syscall boundary: the superblock's
`tar_bytes` is clamped before the allocation (a scribbled field can't
demand a 4 GB kmalloc), and every restored entry is routed through the
same `ramfs_create`/`ramfs_append` the syscalls use, so the 100-char
name cap, 64 KiB size cap, 16-file cap, and normalization all apply — a
hostile disk can create no file the syscall path could not.

## The capstone gate: `make ci-smoke-disk`

Boots the **same** disk image twice:

1. **Boot 1** attaches a freshly-zeroed image, pipes `write /data/note
   tide-remembers-x91` + `sync`. Gated (in boot 1's log) on `ATA: disk
   present`, the RW self-test, and `qsh: sync ok` — *before* boot 2 runs,
   so a broken write/sync fails here, correctly attributed.
2. **Boot 2** attaches the **same** image and pipes only `cat /data/note`.
   Gated (in boot 2's log **only**) on the restore line and the content
   `tide-remembers-x91`.

The two-boot design makes a spurious green impossible: the image is
recreated fresh inside the recipe every run (a stale image can't carry a
false pass), and because `qsh` echoes typed input, the content string
appears in *boot 1's* log via the `write` command echo — so the content
is checked only in boot 2's log, which never types it. Its appearance
there can only be a genuine disk read-back. This runs as a **required**
CI job (`persistence`).

## The field section (epic #96): associative memories that survive reboot

The kernel holographic field (epic #95, `kernel/src/field.c`) rides the
same QDSK volume. `persist_sync` serializes every region into a
fixed-size blob (header + 4x8 fixed 76-byte slot records: bytes +
metadata only — the Q15 wavefront is **recomputed** on restore, which
also revalidates it, and boot-relative imprint ticks reset to 0) at a
**fixed home at the top of the disk** (`ata_sector_count()-6..-2`, just
below the RW self-test scratch). Fixed, because the archive *grows*
between syncs: a moving blob location would let a crash leave the old
superblock pointing at field sectors a larger archive had already
overwritten. The superblock (which has always zeroed its unused tail)
gains a field descriptor at offset 32 — `field_lba`/`field_bytes`/
`field_checksum` — written LAST as part of the same single commit
point. Pre-#96 disks read as `field_lba == 0` → honest cold start.

The crash guarantee, stated honestly: sections are overwritten in
place, so a torn sync can lose an old snapshot — but per-section
checksums mean it is always **detect-and-cold-start, never garbage**,
and a torn *field* write never blocks a valid *fs* restore (or vice
versa).

Restore runs before any service starts, but epic #95's rule scrubs a
region at every capability grant — deliberately, so reborn/successor
services inherit nothing. The reconciliation is **opt-in, once-per-boot
inheritance**: a service declaring `field_inherit` (qsh only) skips the
scrub at the *first* grant of a boot when the disk restored its region,
consuming the inheritance mark only after the cap actually minted and
logging `service: field region 0 inherited from disk (scrub skipped)`.
Watchdog rebirths and every later grant scrub exactly as before.
kannakad does *not* opt in: its boot demo re-seeds region 1 from
scratch every time. Durable operator memories live in region 0 —
`imprint` at the prompt, `sync`, power off, boot, `recall`.

`make ci-smoke-disk` now proves it in three boots: boot 1 imprints and
syncs; boot 2 — which never types the pattern — restores, inherits
region 0, and recalls the exact text from a corrupted probe; boot 3
`dd`s garbage over the blob's fixed LBA and must report
`FIELD: persisted field checksum mismatch - cold start` while the
filesystem still restores (section isolation).

## The audit section: the authority ledger survives reboot

The capability authority ledger (epic #133, `kernel/src/audit.c`) — the
kernel-written record of every GRANT, DENY, SPAWN, manifest denial, quota
refusal, CPU-quota kill, explicit REVOKE, and death-cleanup REAP — rides the
**same** QDSK volume, so an agent's provable-authority history is durable
rather than a per-boot scratchpad.

`persist_sync` serializes the whole ring (256 slots) as a small little-endian
header (`AUD1` magic, version, geometry, and the 64-bit `total`) followed by
the **raw ring image**, into a blob at a **fixed home just below the field
blob** (`ata_sector_count()-1-FIELD_BLOB_SECTORS-AUDIT_BLOB_SECTORS`, 21
sectors). Persisting the whole ring — empty slots and all — means a restore
`memcpy`s it back and sets `total`, reconstructing the exact modular ring
state so the next append lands correctly and coalescing resumes cleanly. The
superblock gains an audit descriptor at offset 44 (`audit_lba`/`audit_bytes`/
`audit_checksum`), written LAST as part of the same single commit. Pre-audit
disks read `audit_lba == 0` → honest cold start.

Because audit is now the **lowest** fixed home, the archive-fit check floors
on `audit_home_lba()` (not the field home) — otherwise a large overlay could
grow over the audit blob, and since the archive is written first the later
audit write would scribble its tail and cold-start the *fs* section. On a disk
too small for the audit blob, audit persistence is skipped (descriptors left
zero) rather than writing off the end of a degenerate volume.

Two semantics are worth pinning down:

- **`seq` continues across reboots.** `total` is restored, so sequence numbers
  are monotonic across power cycles — a genuine append-history. `seq` is the
  **sole** durable ordering key.
- **`tick` is boot-relative and is zeroed on restore** (the timer resets to 0
  each boot, exactly as the field blob zeroes `imprint_tick`), so a restored
  entry never masquerades as "newer" than a post-restore one.
- **The restore boundary is sealed.** A `coalesce_floor` set at restore stops
  the first post-restore append from coalescing into (and mutating) the durable
  prior-boot tail entry — coalescing resumes only among this boot's own records.

`make ci-smoke-disk` proves it with no new typed commands: boot 1's existing
`sync` now persists the ledger (gated in boot 1's log on `AUDIT: synced ledger
to disk`), and boot 2's restore runs at `persist_restore` — **before** this
boot emits a single audit entry — so any entry then in the ring can only have
come from disk. The gate asserts boot 2 logs `AUDIT: restored ledger carries a
prior-boot GRANT` (a GRANT `core_services_init` emits deterministically every
boot — race-free, unlike a timing-driven kill, and un-fakeable). A fourth boot
`dd`s garbage over the audit blob's fixed LBA and must report an `AUDIT: ...
cold start` while the **fs and field sections both still restore** (section
isolation, mirroring the field-corruption boot).

## Known limits / follow-ups

- Single primary-master drive, polled PIO (no DMA, no second disk).
- Volume-level write capability (per-file capabilities remain the honest
  follow-up first promised in `docs/INITRD_VFS.md`).
- `SYS_SYNC` builds the whole archive and writes it under one cli'd
  syscall. Fine for the bounded overlay (16 × 64 KiB), but a near-full
  sync is a multi-sector write with interrupts off; a chunked/cursor sync
  is the scaling follow-up. `SYS_SPAWN` deliberately still loads only
  from the initrd — the overlay is data, not an executable namespace.
- No directory tree (flat normalized paths), no journal (`sync` is
  explicit), no partial-file random write (append + truncate only).
