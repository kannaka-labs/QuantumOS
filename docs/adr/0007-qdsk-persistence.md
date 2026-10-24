# 7. QDSK Persistence — Fixed Homes, Superblock-Last Commit, Detect-and-Cold-Start

Date: 2026-07-11 (as-built record)
Status: Accepted

## Context

Epic #71 gave QuantumOS a single ATA disk and a ustar snapshot of the RAM
filesystem overlay. Two more durable payloads then arrived with different
shapes: the kernel holographic field (ADR-0006, epic #96) and the authority
ledger (ADR-0009, durable half in PR #146) — both fixed-size kernel blobs,
unlike the archive, which grows with file content. The constraints are severe:
no journal, no on-disk allocator, PIO ATA where the only atomic primitive is a
512-byte sector write, a kernel that must not depend on kmalloc succeeding
during restore, and a disk that is attacker-writable while the machine is off.
Real hardware added one more: LBA 0 of the primary ATA disk can be somebody's
partition table (learned booting from USB on a laptop, ramfs.c:348-354).

Each new durable payload therefore needed a place to live, a way to commit
consistently with the others, and an honest failure story — without inventing
a filesystem.

## Decision

One volume format, `QDSK1`, with the LBA-0 superblock as the **single commit
point** and every payload as an independent **section**:

1. **Descriptors in previously-zero superblock bytes.** The archive descriptor
   sits at offsets 8/12/16 (bytes, file count, checksum; ramfs.c:188-190); the
   field section at 32/36/40 and the audit section at 44/48/52
   (ramfs.c:196-206). Because the sync path zeroes the whole superblock sector,
   `field_lba == 0` / `audit_lba == 0` are reliable "no section" sentinels on
   pre-upgrade disks — LBA 0 can never be a legitimate blob home
   (ramfs.c:191-195, 199-203).
2. **Fixed homes counted from the disk top, away from the growing archive.**
   The audit blob home is FROZEN at `ata_sector_count() - 27`, where 27 is
   `_Static_assert`ed against its legacy derivation (1 scratch + 5 legacy field
   + 21 audit sectors; ramfs.c:216-221). The field blob lives BELOW it inside a
   16-sector reserve (`FIELD_HOME_RESERVE_SECTORS`, ramfs.c:233-238), with a
   second static assert that the 10-sector blob fits ("field blob outgrew its
   reserved span - widening relocates homes on disk!", ramfs.c:234-235).
   Freezing the ledger home is what lets field-geometry growth never relocate —
   and thus never discard — the format-unchanged ledger (ramfs.c:209-215).
3. **Sections first, superblock LAST.** `persist_sync` writes the archive at
   LBA 1, then the field blob, then the audit blob, and only then the
   superblock (ramfs.c:439-508). Any tear before the superblock leaves the OLD
   superblock authoritative; partial section bytes fail that section's checksum
   at restore. The stated guarantee is **DETECT-AND-COLD-START, never garbage**
   — sections are overwritten in place, so a tear can lose the old snapshot but
   can never be misread as truth (ramfs.c:439-445).
4. **Independent checksums, independent restore.** Each section carries its own
   rotate-xor additive checksum (seed 0x517E7A11, ramfs.c:266-272);
   `persist_restore` restores fs, field, and audit sections independently so a
   torn one never blocks the others (ramfs.c:685-687).
5. **Self-describing blobs, exact-geometry loads.** The field blob header
   (`QFL1`, version, region/slot/pattern geometry) must describe exactly this
   build (field.c:366-371, 402-406); the audit blob (`AUD1`, entry count/size,
   64-bit total) likewise, plus a `total > 2^48` plausibility rejection
   (audit.c:313-318, 335-348). Both serializers zero the whole sector-rounded
   buffer first so tail padding never carries kernel memory (field.c:361-365,
   audit.c:307-311). All fields are little-endian u32 written bytewise —
   compiler-independent (field.c:343-358).
6. **The disk is a trust boundary at restore.** A section restores only if its
   descriptor names EXACTLY the recomputed fixed home and the exact byte count
   (ramfs.c:596, 635), the whole blob checksums, and the blob's own load
   revalidates content — for the field, per slot: alive byte exactly 1, length
   bounds-checked BEFORE the wavefront recompute indexes with it, energy
   clamped, degenerate content dropped (field.c:413-427). Anything off is an
   honestly logged cold start.
7. **Floors and refusals.** Sync refuses a disk that is neither QDSK nor
   provably blank (foreign-volume guard, ramfs.c:348-381, commit 63a3d0a). The
   archive must end below `field_home_lba()` — the lowest fixed home is the
   fit floor (ramfs.c:399-410). `audit_disk_fits()` requires ≥ 45 sectors, else
   BOTH blob persists are skipped rather than issuing underflowed-LBA writes
   (ramfs.c:240-248). Blob I/O uses static bounce buffers — no kmalloc failure
   mode on the restore path (ramfs.c:250-259).

**The reusable pattern for any future durable section**: a descriptor triple
(LBA/bytes/csum) in reserved-zero superblock bytes; a fixed home below the
current lowest home, sized with growth reserve; a self-describing magic +
geometry header; an independent checksum; written before the superblock;
restored by exact-home + exact-size + checksum + content revalidation, with
cold start as the only failure mode. Sync criticality is a per-section choice:
the audit write is best-effort ("durable authority is an addition, never a
hostage of the fs/field commit", ramfs.c:470-475), while a field write failure
aborts the whole sync before the superblock (ramfs.c:460-463).

## Consequences

### Positive

- Crash consistency without a journal: one sector write commits all three
  sections, and every pre-commit tear is detected by a section checksum.
- Cross-version survival: PR #183's upgrade gate proves the ledger restores
  from a disk whose field descriptor predates the current geometry, while the
  field cold-starts gracefully (Makefile:1358-1428).
- Corruption isolation proven in both directions: CI dd-scribbles the audit
  home (LBA 4069 on the 4096-sector image) and the field home (4053) and
  requires the other sections to restore intact (Makefile:1303-1354).
- Restore-before-services ordering (`persist_restore` at main.c:177, before
  `core_services_init` at main.c:186) makes a restored GRANT an un-fakeable
  proof of durable ledger content (ramfs.c:652-663).

### Negative

- Sections are overwritten in place with no A/B copies: a torn sync can LOSE
  the previous good snapshot of a section. The system promises detection, not
  retention of the last good version (ramfs.c:443-445).
- A field-geometry upgrade honestly discards field content — durable memory
  does not migrate formats; only the ledger survives (ramfs.c:223-231).
- The 16-sector reserve caps field growth: at 24 + 608×R blob bytes, region
  count can grow only to 13 before the static assert forces an on-disk home
  relocation (field.h:162-163, ramfs.c:234-235).
- The legacy 5 field sectors just under the scratch are orphaned on upgraded
  disks until the next sync rewrites the superblock (ramfs.c:229-231).
- Persistence is explicit: nothing auto-syncs; state since the last SYS_SYNC
  (syscall 25, syscall.h:128-133 → persist_sync at syscall.c:748-757) is lost
  on power cut.

### Residual risks

- The rotate-xor checksum is not cryptographic and is not keyed: an offline
  attacker can craft a section that passes checksum, exact-home, and geometry
  checks. Content revalidation bounds the damage to well-formed field slots or
  a plausible ledger ring — the disk is validated, never authenticated.
- The superblock itself has no checksum: a scribbled magic makes all three
  sections cold-start as a "fresh volume" (ramfs.c:677-683) — a single point
  of detection, though never of false restore.
- The frozen audit home is asserted against the CONSTANT at compile time and
  against the FUNCTION's on-disk result only by the #183 gate's 4096-sector
  image; other disk sizes rely on the same arithmetic holding untested.

## Evidence

- Shipped in: PR #72 (epic #71: ATA disk, QDSK1 superblock, ustar overlay sync/restore)
- Shipped in: PR #114 (epic #96: field section at a fixed home, independent checksum)
- Shipped in: PR #146 (durable audit ledger section, best-effort sync)
- Shipped in: PR #177 (frozen audit home + 16-sector field growth reserve)
- Shipped in: PR #183 (disk-upgrade CI gate: frozen home + stale-descriptor cold start)
- Shipped in: commit 63a3d0a (sync refuses foreign volumes — real-hardware safety)
- Amended by: #255 (the QDSK layout is unchanged; it now sits on `blkdev_*`, so the same on-disk format is carried by ATA or virtio-blk, mmio or pci; gate `ci-smoke-disk-virtio`)
- Key code: ramfs.c:186-248 (descriptors, frozen homes, fit guard), ramfs.c:343-521 (`persist_sync`: guard → sections → superblock-last), ramfs.c:579-688 (independent section restores), field.c:360-453 + field.h:159-163 (field blob write/load), audit.c:303-367 + audit.h:119-131 (ledger blob serialize/load)
- CI gates: `ci-smoke-disk` (two-boot fs/field/audit persistence + both corruption-isolation sub-gates, Makefile:1234-1356); `ci-smoke-disk-upgrade` (frozen-home assert at LBA 4069 + pre-#177 descriptor cold start, Makefile:1374-1428)
