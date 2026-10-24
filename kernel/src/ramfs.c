/**
 * QuantumOS Writable RAM Filesystem Overlay implementation (epic #71
 * phase 2).
 *
 * A fixed table of kmalloc-backed files. Single CPU and every caller is
 * a cli'd syscall or the boot path, so no locking. Storage is allocated
 * at full RAMFS_FILE_MAX capacity on create (16 x 64 KiB = 1 MiB worst
 * case against a ~67 MB heap) and freed on unlink, keeping the append
 * path allocation-free and the data pointer stable for open fds.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/ramfs.h>
#include <kernel/memory.h>
#include <kernel/blkdev.h>
#include <kernel/tar.h>
#include <kernel/field.h>
#include <kernel/audit.h>
#include <kernel/boot.h>

typedef struct {
    char name[RAMFS_NAME_MAX + 1];
    uint8_t *data;
    uint32_t size;
    bool used;
} ramfile_t;

static ramfile_t files[RAMFS_MAX_FILES];

/* Strip "./" and leading '/' — the same normalization the initrd uses,
 * so /data/note and data/note are one path across both layers. */
static const char *normalize(const char *name) {
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    while (name[0] == '/') {
        name++;
    }
    return name;
}

static int str_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

static int str_starts(const char *s, const char *prefix) {
    int i = 0;
    while (prefix[i] && s[i] == prefix[i]) {
        i++;
    }
    return prefix[i] == '\0';
}

static int find_by_name(const char *norm) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used && str_eq(files[i].name, norm)) {
            return i;
        }
    }
    return -1;
}

int ramfs_create(const char *path) {
    if (!path) {
        return -1;
    }
    const char *norm = normalize(path);
    if (!norm[0]) {
        return -1;
    }

    size_t len = 0;
    while (norm[len]) {
        len++;
    }
    if (len > RAMFS_NAME_MAX) {
        return -1;
    }

    /* Existing file: truncate in place (data pointer stays valid for
     * any open fd; its recorded size snapshot just goes stale, exactly
     * like a POSIX file truncated behind a reader). */
    int idx = find_by_name(norm);
    if (idx >= 0) {
        files[idx].size = 0;
        return idx;
    }

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) {
            uint8_t *buf = kmalloc(RAMFS_FILE_MAX);
            if (!buf) {
                return -1;
            }
            for (size_t c = 0; c <= len; c++) {
                files[i].name[c] = norm[c];
            }
            files[i].data = buf;
            files[i].size = 0;
            files[i].used = true;
            return i;
        }
    }
    return -1; /* table full */
}

int ramfs_append(int idx, const uint8_t *data, uint32_t len) {
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !files[idx].used || !data) {
        return -1;
    }
    uint32_t room = RAMFS_FILE_MAX - files[idx].size;
    if (len > room) {
        len = room;
    }
    for (uint32_t i = 0; i < len; i++) {
        files[idx].data[files[idx].size + i] = data[i];
    }
    files[idx].size += len;
    return (int)len;
}

int ramfs_lookup(const char *path, const uint8_t **data_out, uint32_t *size_out) {
    if (!path || !data_out || !size_out) {
        return -1;
    }
    int idx = find_by_name(normalize(path));
    if (idx < 0) {
        return -1;
    }
    *data_out = files[idx].data;
    *size_out = files[idx].size;
    return idx;
}

int ramfs_unlink(const char *path) {
    if (!path) {
        return -1;
    }
    int idx = find_by_name(normalize(path));
    if (idx < 0) {
        return -1;
    }
    kfree(files[idx].data);
    files[idx].data = NULL;
    files[idx].size = 0;
    files[idx].used = false;
    files[idx].name[0] = '\0';
    return 0;
}

uint32_t ramfs_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (files[i].used) {
            n++;
        }
    }
    return n;
}

int ramfs_get(int idx, const char **name_out, const uint8_t **data_out, uint32_t *size_out) {
    if (idx < 0 || idx >= RAMFS_MAX_FILES || !files[idx].used) {
        return -1;
    }
    if (name_out) {
        *name_out = files[idx].name;
    }
    if (data_out) {
        *data_out = files[idx].data;
    }
    if (size_out) {
        *size_out = files[idx].size;
    }
    return 0;
}

/* ============================================================================
 * Persistence (epic #71 phase 3, #255): overlay <-> the bound disk as ustar
 * ============================================================================ */

#define QDSK_MAGIC "QDSK1"
#define QDSK_MAGIC_LEN 5
#define QDSK_TARBYTES_OFF 8   /* u32: archive byte count */
#define QDSK_FILECOUNT_OFF 12 /* u32: files in the archive (informational) */
#define QDSK_CHECKSUM_OFF 16  /* u32: additive checksum over the archive */
/* Field section descriptor (epic #96). Offsets 20..31 stay zero for
 * compatibility slack; pre-#96 superblocks are all-zero past byte 19
 * (the sync path zeroes the whole sector), so field_lba == 0 is a
 * reliable "no field section" sentinel — LBA 0 is the superblock itself
 * and can never be a legitimate blob location. */
#define QDSK_FIELD_LBA_OFF 32   /* u32: blob location (the FIXED home) */
#define QDSK_FIELD_BYTES_OFF 36 /* u32: blob byte count (exact) */
#define QDSK_FIELD_CSUM_OFF 40  /* u32: additive checksum over the blob */
/* Audit-ledger section descriptor (durable authority ledger). Same
 * "fixed home + independent checksum" discipline as the field section;
 * bytes 44..55 were reserved-zero on pre-audit QDSK1 disks (the sync path
 * zeroes the whole sector), so audit_lba == 0 is a reliable "no audit
 * section" sentinel and old disks cold-start the ledger cleanly. */
#define QDSK_AUDIT_LBA_OFF 44   /* u32: blob location (the FIXED home) */
#define QDSK_AUDIT_BYTES_OFF 48 /* u32: blob byte count (exact) */
#define QDSK_AUDIT_CSUM_OFF 52  /* u32: additive checksum over the blob */
#define TAR_BLOCK_SZ 512

/* The audit-ledger blob home is FROZEN at its legacy absolute offset from
 * the disk top (1 scratch sector + the legacy 5-sector field slot + 21
 * audit sectors = 27). The ledger's on-disk format does NOT change when
 * the FIELD blob grows, so pinning its home lets the durable authority
 * ledger survive field-geometry upgrades on existing disks — a moving
 * home would fail the restore's descriptor==recomputed-home check and
 * discard a perfectly valid ledger as collateral damage (epic #177). */
#define AUDIT_HOME_TOP_OFFSET 27u
_Static_assert(AUDIT_HOME_TOP_OFFSET == 1u + 5u + AUDIT_BLOB_SECTORS,
               "audit home drifted from its frozen legacy derivation");
static uint32_t audit_home_lba(void) {
    return blkdev_sector_count() - AUDIT_HOME_TOP_OFFSET;
}

/* The field blob lives BELOW the audit blob inside a RESERVED growth span:
 * FIELD_REGION_COUNT bumps grow FIELD_BLOB_SECTORS, and without the
 * reserve every bump would relocate both homes (restore rejects a
 * descriptor that no longer names the recomputed home, so BOTH sections
 * would cold-start). With the reserve only the field section itself
 * honestly cold-starts across a field-geometry change, at the descriptor
 * geometry guard below — the audit home never moves. The legacy field
 * sectors just under the scratch are orphaned until the next sync.
 * Constant location keeps the corruption CI gate a deterministic dd
 * offset. */
#define FIELD_HOME_RESERVE_SECTORS 16u
_Static_assert(FIELD_BLOB_SECTORS <= FIELD_HOME_RESERVE_SECTORS,
               "field blob outgrew its reserved span - widening relocates homes on disk!");
static uint32_t field_home_lba(void) {
    return audit_home_lba() - FIELD_HOME_RESERVE_SECTORS;
}

/* Smallest disk that can hold the superblock + a minimal archive + both
 * fixed homes (audit at top-27, field in the reserve below it) + the
 * scratch sector. Below this the home LBAs would unsigned-underflow to
 * wild values; we skip BOTH blob persists entirely (leaving the
 * descriptors zero so restore cold-starts) rather than issue writes off
 * the end of a degenerate volume. */
static int audit_disk_fits(void) {
    return blkdev_sector_count() >= AUDIT_HOME_TOP_OFFSET + FIELD_HOME_RESERVE_SECTORS + 2u;
}

/* Shared bounce for the field blob (FIELD_BLOB_SECTORS sectors). Boot
 * restore is single-threaded and sync runs cli'd in syscall context, so
 * one static buffer serves both — and unlike a kmalloc, it cannot fail. */
static uint8_t field_blob_buf[FIELD_BLOB_SECTORS * BLK_SECTOR_SIZE];

/* Shared bounce for the audit-ledger blob (AUDIT_BLOB_SECTORS = 21
 * sectors, 10752 bytes). Same rationale as field_blob_buf: one static
 * buffer serves sync and restore and cannot fail like a kmalloc; a
 * stack local this size would risk the guard-page-less kernel stack. */
static uint8_t audit_blob_buf[AUDIT_BLOB_SECTORS * BLK_SECTOR_SIZE];

/* Simple additive checksum over the archive. Written into the
 * superblock only after the archive itself is on disk, so a torn sync
 * (new superblock never reached) leaves the OLD superblock+archive
 * intact, and a torn archive write is caught by the mismatch at restore
 * rather than half-restored as truth. */
static uint32_t archive_checksum(const uint8_t *buf, uint32_t len) {
    uint32_t sum = 0x517E7A11u;
    for (uint32_t i = 0; i < len; i++) {
        sum = (sum << 1 | sum >> 31) ^ buf[i];
    }
    return sum;
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Write an 11-digit octal field (POSIX size/mtime format), NUL-closed. */
static void tar_put_octal(uint8_t *field, uint32_t v) {
    for (int i = 10; i >= 0; i--) {
        field[i] = (uint8_t)('0' + (v & 7));
        v >>= 3;
    }
    field[11] = '\0';
}

/* Compose one POSIX ustar header (valid checksum) for a regular file. */
static void tar_put_header(uint8_t *h, const char *name, uint32_t size) {
    for (int i = 0; i < TAR_BLOCK_SZ; i++) {
        h[i] = 0;
    }
    /* name (bounded by RAMFS_NAME_MAX <= 100) */
    for (int i = 0; i < 100 && name[i]; i++) {
        h[i] = (uint8_t)name[i];
    }
    /* mode/uid/gid: octal ASCII */
    const char *mode = "0000644";
    const char *zero7 = "0000000";
    for (int i = 0; i < 7; i++) {
        h[100 + i] = (uint8_t)mode[i];  /* mode */
        h[108 + i] = (uint8_t)zero7[i]; /* uid */
        h[116 + i] = (uint8_t)zero7[i]; /* gid */
    }
    tar_put_octal(h + 124, size); /* size */
    tar_put_octal(h + 136, 0);    /* mtime */
    h[156] = '0';                 /* typeflag: regular file */
    const char *magic = "ustar";
    for (int i = 0; i < 5; i++) {
        h[257 + i] = (uint8_t)magic[i];
    }
    h[262] = '\0';
    h[263] = '0'; /* version "00" */
    h[264] = '0';

    /* Checksum: sum of all header bytes with the chksum field (148..155)
     * read as spaces; stored as 6 octal digits + NUL + space. */
    for (int i = 0; i < 8; i++) {
        h[148 + i] = ' ';
    }
    uint32_t sum = 0;
    for (int i = 0; i < TAR_BLOCK_SZ; i++) {
        sum += h[i];
    }
    for (int i = 5; i >= 0; i--) {
        h[148 + i] = (uint8_t)('0' + (sum & 7));
        sum >>= 3;
    }
    h[154] = '\0';
    h[155] = ' ';
}

static uint32_t align_block(uint32_t v) {
    return (v + TAR_BLOCK_SZ - 1) & ~(uint32_t)(TAR_BLOCK_SZ - 1);
}

int persist_sync(void) {
    if (!blkdev_present()) {
        return -1;
    }

    /* SAFETY GUARD (learned the moment QuantumOS booted from USB on a
     * real laptop): sync writes the superblock to LBA 0 of the primary
     * ATA disk — on real hardware that can be the machine's INTERNAL
     * drive, and LBA 0 is its partition table. Only ever write to a
     * volume that is already OURS (QDSK magic) or provably BLANK (an
     * all-zero sector 0, e.g. a fresh CI image). Anything else is
     * someone's disk: refuse loudly, destroy nothing. */
    {
        uint8_t sb0[BLK_SECTOR_SIZE];
        if (blkdev_read(0, sb0, 1) != 0) {
            boot_log("SYNC: cannot read sector 0 — not flushed");
            return -1;
        }
        int is_qdsk = 1;
        const char *magic = QDSK_MAGIC;
        for (int i = 0; i < QDSK_MAGIC_LEN; i++) {
            if (sb0[i] != (uint8_t)magic[i]) {
                is_qdsk = 0;
                break;
            }
        }
        if (!is_qdsk) {
            int blank = 1;
            for (int i = 0; i < BLK_SECTOR_SIZE; i++) {
                if (sb0[i] != 0) {
                    blank = 0;
                    break;
                }
            }
            if (!blank) {
                boot_log("SYNC: disk carries a foreign volume - refusing to overwrite");
                return -1;
            }
        }
    }

    /* Size the archive: header + block-aligned data per file, plus the
     * two terminating zero blocks. */
    uint32_t tar_bytes = 2 * TAR_BLOCK_SZ;
    uint32_t nfiles = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        const uint8_t *d;
        uint32_t sz;
        const char *nm;
        if (ramfs_get(i, &nm, &d, &sz) == 0) {
            tar_bytes += TAR_BLOCK_SZ + align_block(sz);
            nfiles++;
        }
    }

    uint32_t tar_sectors = tar_bytes / BLK_SECTOR_SIZE;
    /* Fit check: superblock + archive must end BELOW the LOWEST fixed home.
     * The field blob now sits in the reserve below the audit blob, so the
     * field home is the floor — otherwise a large overlay could grow over
     * it, and since the archive is written FIRST, the later blob writes
     * would then scribble the archive tail and cold-start the fs section
     * at restore. On a disk too small for the fixed homes, both blob
     * persists are skipped, so only the scratch sector bounds the archive. */
    uint32_t tar_floor = audit_disk_fits() ? field_home_lba() : blkdev_sector_count() - 1;
    if (1 + tar_sectors > tar_floor) {
        boot_log("SYNC: archive does not fit on disk — not flushed");
        return -1;
    }

    uint8_t *buf = kmalloc(tar_bytes);
    if (!buf) {
        boot_log("SYNC: out of kernel heap — not flushed");
        return -1;
    }
    for (uint32_t i = 0; i < tar_bytes; i++) {
        buf[i] = 0;
    }

    uint32_t off = 0;
    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        const uint8_t *d;
        uint32_t sz;
        const char *nm;
        if (ramfs_get(i, &nm, &d, &sz) != 0) {
            continue;
        }
        tar_put_header(buf + off, nm, sz);
        off += TAR_BLOCK_SZ;
        for (uint32_t b = 0; b < sz; b++) {
            buf[off + b] = d[b];
        }
        off += align_block(sz);
    }

    uint32_t csum = archive_checksum(buf, tar_bytes);

    /* Sections first (each flushed durable by blkdev_write), superblock
     * LAST — the single commit point. Any tear before the superblock
     * leaves the old superblock authoritative; whatever partial bytes
     * landed in either section fail that section's checksum at restore.
     * The honest guarantee is DETECT-AND-COLD-START, never garbage:
     * sections are overwritten in place, so a tear can lose the old
     * snapshot — it can never be misread as truth. */
    if (blkdev_write(1, buf, tar_sectors) != 0) {
        kfree(buf);
        boot_log("SYNC: disk write failed — not flushed");
        return -1;
    }
    kfree(buf);

    /* Field section (epic #96): every region, at the fixed home. Guarded
     * like audit below: on a degenerate disk both homes underflow, so both
     * persists are skipped (descriptors stay zero, restore cold-starts). */
    uint32_t field_lba = 0, field_bytes = 0;
    uint32_t field_slots = field_blob_write(field_blob_buf);
    uint32_t field_csum = archive_checksum(field_blob_buf, FIELD_BLOB_BYTES);
    if (audit_disk_fits()) {
        if (blkdev_write(field_home_lba(), field_blob_buf, FIELD_BLOB_SECTORS) != 0) {
            boot_log("SYNC: field write failed — not flushed");
            return -1;
        }
        field_lba = field_home_lba();
        field_bytes = FIELD_BLOB_BYTES;
    } else {
        field_csum = 0;
    }

    /* Audit section (durable authority ledger): the whole ledger at its
     * fixed home. BEST-EFFORT — a write failure or a too-small disk leaves
     * the descriptors zero (restore cold-starts) and MUST NOT abort the
     * fs/field commit; durable authority is an addition, never a hostage of
     * the fs/field snapshot. The serialize snapshots the ring under irqsave;
     * the blkdev_write runs here, outside that window. */
    uint32_t audit_lba = 0, audit_bytes = 0, audit_csum = 0;
    if (audit_disk_fits() &&
        audit_serialize(audit_blob_buf, AUDIT_BLOB_BUFSZ) == AUDIT_BLOB_BYTES) {
        if (blkdev_write(audit_home_lba(), audit_blob_buf, AUDIT_BLOB_SECTORS) == 0) {
            audit_lba = audit_home_lba();
            audit_bytes = AUDIT_BLOB_BYTES;
            audit_csum = archive_checksum(audit_blob_buf, AUDIT_BLOB_BYTES);
        } else {
            boot_log("SYNC: audit ledger write failed — ledger not persisted this sync");
        }
    }

    uint8_t sb[BLK_SECTOR_SIZE];
    for (int i = 0; i < BLK_SECTOR_SIZE; i++) {
        sb[i] = 0;
    }
    const char *magic = QDSK_MAGIC;
    for (int i = 0; i < QDSK_MAGIC_LEN; i++) {
        sb[i] = (uint8_t)magic[i];
    }
    put_u32(sb + QDSK_TARBYTES_OFF, tar_bytes);
    put_u32(sb + QDSK_FILECOUNT_OFF, nfiles);
    put_u32(sb + QDSK_CHECKSUM_OFF, csum);
    put_u32(sb + QDSK_FIELD_LBA_OFF, field_lba);
    put_u32(sb + QDSK_FIELD_BYTES_OFF, field_bytes);
    put_u32(sb + QDSK_FIELD_CSUM_OFF, field_csum);
    put_u32(sb + QDSK_AUDIT_LBA_OFF, audit_lba);
    put_u32(sb + QDSK_AUDIT_BYTES_OFF, audit_bytes);
    put_u32(sb + QDSK_AUDIT_CSUM_OFF, audit_csum);
    if (blkdev_write(0, sb, 1) != 0) {
        boot_log("SYNC: superblock write failed — not flushed");
        return -1;
    }

    boot_log("SYNC: overlay flushed to disk, files:");
    early_console_write_hex(nfiles);
    boot_log("FIELD: synced slots to disk:");
    early_console_write_hex(field_slots);
    /* Logged only after the superblock commit and only on a real write, so a
     * skipped/failed audit write is attributable at THIS boot (never surfaces
     * confusingly as a missing restore next boot). */
    if (audit_lba != 0) {
        boot_log("AUDIT: synced ledger to disk");
    }
    return (int)nfiles;
}

static int restore_cb(const char *name, const uint8_t *data, uint32_t size, void *ctx) {
    uint32_t *count = (uint32_t *)ctx;
    int idx = ramfs_create(name);
    if (idx >= 0) {
        ramfs_append(idx, data, size);
        (*count)++;
    }
    return 0;
}

static void restore_fs_section(const uint8_t *sb) {
    uint32_t tar_bytes = get_u32(sb + QDSK_TARBYTES_OFF);
    uint32_t want_csum = get_u32(sb + QDSK_CHECKSUM_OFF);

    /* Sanity BEFORE the kmalloc: the archive can never legitimately
     * exceed the overlay's worst case nor the disk that holds it (a
     * scribbled superblock could otherwise demand a ~4 GB allocation). */
    uint32_t worst = (uint32_t)RAMFS_MAX_FILES * (TAR_BLOCK_SZ + RAMFS_FILE_MAX) + 2 * TAR_BLOCK_SZ;
    if (tar_bytes < 2 * TAR_BLOCK_SZ || tar_bytes % TAR_BLOCK_SZ != 0 || tar_bytes > worst ||
        tar_bytes / BLK_SECTOR_SIZE > blkdev_sector_count() - 1) {
        boot_log("FS: superblock names an implausible archive — starting empty");
        return;
    }

    uint8_t *buf = kmalloc(tar_bytes);
    if (!buf) {
        boot_log("FS: out of kernel heap for restore — starting empty");
        return;
    }
    if (blkdev_read(1, buf, tar_bytes / BLK_SECTOR_SIZE) != 0) {
        kfree(buf);
        boot_log("FS: archive read failed — starting empty");
        return;
    }

    /* Whole-archive checksum: catches a torn sync (superblock committed
     * but archive write interrupted) that the per-header check might not.
     * Mismatch -> start empty rather than restore corruption as truth. */
    if (archive_checksum(buf, tar_bytes) != want_csum) {
        kfree(buf);
        boot_log("FS: persisted archive checksum mismatch (torn sync?) — starting empty");
        return;
    }

    /* restore_cb routes every entry through ramfs_create/append, so the
     * 100-char name cap, 64 KB size cap, 16-file cap, and normalize()
     * all apply — a hostile archive can create no file the syscall path
     * could not. tar_walk_mem also verifies each header checksum. */
    uint32_t restored = 0;
    tar_walk_mem(buf, tar_bytes, restore_cb, &restored);
    kfree(buf);

    boot_log("FS: restored persisted files from disk:");
    early_console_write_hex(restored);
}

/* Field section restore (epic #96). Independent of the fs section: each
 * carries its own checksum, so a torn fs archive never blocks a valid
 * field restore and vice versa. The blob crossed a trust boundary (the
 * disk is attacker-writable offline): the descriptor must name EXACTLY
 * the fixed home and the exact blob size, the whole blob must checksum,
 * and field_blob_load revalidates every slot (len bounds BEFORE the
 * wavefront recompute). Anything off -> cold start, honestly logged. */
static void restore_field_section(const uint8_t *sb) {
    uint32_t field_lba = get_u32(sb + QDSK_FIELD_LBA_OFF);
    uint32_t field_bytes = get_u32(sb + QDSK_FIELD_BYTES_OFF);
    uint32_t want_csum = get_u32(sb + QDSK_FIELD_CSUM_OFF);

    if (field_lba == 0) {
        /* Pre-#96 disk: bytes past 19 are guaranteed zero. */
        boot_log("FIELD: no persisted field on disk (cold start)");
        return;
    }
    if (field_lba != field_home_lba() || field_bytes != FIELD_BLOB_BYTES) {
        boot_log("FIELD: superblock names an implausible field section — cold start");
        return;
    }
    if (blkdev_read(field_lba, field_blob_buf, FIELD_BLOB_SECTORS) != 0) {
        boot_log("FIELD: blob read failed — cold start");
        return;
    }
    if (archive_checksum(field_blob_buf, FIELD_BLOB_BYTES) != want_csum) {
        boot_log("FIELD: persisted field checksum mismatch - cold start");
        return;
    }
    int64_t restored = field_blob_load(field_blob_buf, FIELD_BLOB_BYTES);
    if (restored < 0) {
        boot_log("FIELD: blob header mismatch - cold start");
        return;
    }
    boot_log("FIELD: restored slots from disk:");
    early_console_write_hex((uint32_t)restored);
}

/* Audit section restore (durable authority ledger). Independent of the fs
 * and field sections: its own checksum + geometry guard, so a torn audit
 * blob never blocks a valid fs/field restore and vice versa. The blob
 * crossed a trust boundary (the disk is attacker-writable offline): the
 * descriptor must name EXACTLY the fixed home and exact size, the whole
 * blob must checksum, and audit_load revalidates the header geometry.
 * Anything off -> cold start, honestly logged. */
static void restore_audit_section(const uint8_t *sb) {
    uint32_t audit_lba = get_u32(sb + QDSK_AUDIT_LBA_OFF);
    uint32_t audit_bytes = get_u32(sb + QDSK_AUDIT_BYTES_OFF);
    uint32_t want_csum = get_u32(sb + QDSK_AUDIT_CSUM_OFF);

    if (audit_lba == 0) {
        /* Pre-audit disk (bytes past 43 are zero) or a sync that skipped the
         * ledger (write failure / too-small disk). */
        boot_log("AUDIT: no persisted ledger on disk (cold start)");
        return;
    }
    if (!audit_disk_fits() || audit_lba != audit_home_lba() || audit_bytes != AUDIT_BLOB_BYTES) {
        boot_log("AUDIT: superblock names an implausible ledger section - cold start");
        return;
    }
    if (blkdev_read(audit_lba, audit_blob_buf, AUDIT_BLOB_SECTORS) != 0) {
        boot_log("AUDIT: ledger read failed - cold start");
        return;
    }
    if (archive_checksum(audit_blob_buf, AUDIT_BLOB_BYTES) != want_csum) {
        boot_log("AUDIT: persisted ledger checksum mismatch - cold start");
        return;
    }
    if (audit_load(audit_blob_buf, AUDIT_BLOB_BYTES) != 0) {
        boot_log("AUDIT: ledger header mismatch - cold start");
        return;
    }
    boot_log("AUDIT: restored ledger from disk");
    /* Content proof: persist_restore runs BEFORE core_services_init/user_init,
     * so this boot has generated ZERO audit entries — any entry now in the ring
     * provably came from disk. core_services_init emits GRANT deterministically
     * every boot (no IRQ, no race), so a restored GRANT is a race-free,
     * un-fakeable durable-authority artifact. A restored CPUKILL (from a prior
     * boot's runaway kill) is logged too, informationally. */
    if (audit_ring_has_kind(AUDIT_GRANT)) {
        boot_log("AUDIT: restored ledger carries a prior-boot GRANT");
    }
    if (audit_ring_has_kind(AUDIT_CPUKILL)) {
        boot_log("AUDIT: restored ledger carries a prior-boot CPUKILL");
    }
}

void persist_restore(void) {
    if (!blkdev_present()) {
        return;
    }

    uint8_t sb[BLK_SECTOR_SIZE];
    if (blkdev_read(0, sb, 1) != 0) {
        boot_log("FS: superblock read failed — starting empty");
        return;
    }

    const char *magic = QDSK_MAGIC;
    for (int i = 0; i < QDSK_MAGIC_LEN; i++) {
        if (sb[i] != (uint8_t)magic[i]) {
            boot_log("FS: no persisted archive on disk (fresh volume)");
            return;
        }
    }

    restore_fs_section(sb);
    restore_field_section(sb);
    restore_audit_section(sb);
}

size_t ramfs_format_list(const char *prefix, char *buf, size_t max, size_t o) {
    const char *pref = normalize(prefix ? prefix : "");

    for (int i = 0; i < RAMFS_MAX_FILES; i++) {
        if (!files[i].used) {
            continue;
        }
        if (pref[0] && !str_starts(files[i].name, pref)) {
            continue;
        }

        /* Build "FS: <name> <size> [ram]\r\n" as a whole row. */
        char row[RAMFS_NAME_MAX + 32];
        size_t r = 0;
        const char *pfx = "FS: ";
        while (*pfx) {
            row[r++] = *pfx++;
        }
        for (int c = 0; files[i].name[c] && r < sizeof(row) - 24; c++) {
            row[r++] = files[i].name[c];
        }
        row[r++] = ' ';
        char t[10];
        int n = 0;
        uint32_t v = files[i].size;
        if (v == 0) {
            t[n++] = '0';
        }
        while (v && n < (int)sizeof(t)) {
            t[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (n > 0) {
            row[r++] = t[--n];
        }
        const char *tag = " [ram]\r\n";
        while (*tag) {
            row[r++] = *tag++;
        }

        if (o + r + 1 > max) {
            break;
        }
        for (size_t c = 0; c < r; c++) {
            buf[o++] = row[c];
        }
    }
    buf[o] = '\0';
    return o;
}
