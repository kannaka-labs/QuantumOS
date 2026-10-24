/**
 * QuantumOS blkdev binding (#255).
 *
 * A static probe list, bound once, like netdev.c. Probe ORDER is policy:
 * virtio-blk first (the paravirtual disk every modern hypervisor offers,
 * and the only one Firecracker has), then ATA. A virtio-blk probe with no
 * device is silent, so a diskless boot still logs exactly one line, the
 * ATA "no disk" line the CI gates already check.
 *
 * The boot-time sector RW self-test lives here, not in a driver, so every
 * disk gets the same proof under its own tag ("ATA: sector RW self-test
 * OK ...", "BLK: sector RW self-test OK ...").
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/blkdev.h>
#include <kernel/ata.h>
#include <kernel/virtio_blk.h>
#include <kernel/boot.h>

static const blkdev_ops_t *const drivers[] = {
    &virtio_blk_blkdev,
    &ata_blkdev,
};

#define DRIVER_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

static const blkdev_ops_t *bound; /* NULL until a driver reports present */

/* "<tag>: <msg>" through boot_log (which also tees to the screen). */
static void blk_log(const char *msg) {
    char line[128];
    uint32_t n = 0;
    const char *tag = (bound && bound->tag) ? bound->tag : "BLK";
    for (const char *p = tag; *p && n < sizeof(line) - 3; p++) {
        line[n++] = *p;
    }
    line[n++] = ':';
    line[n++] = ' ';
    for (const char *p = msg; *p && n < sizeof(line) - 1; p++) {
        line[n++] = *p;
    }
    line[n] = '\0';
    boot_log(line);
}

/* Boot-time write-path proof. The destructive write happens ONLY when
 * the last sector is safe to clobber — all-zero (a fresh image) or on a
 * volume already carrying our own QDSK superblock. On any foreign disk
 * the test degrades to a non-destructive read probe, so a user's data
 * is never risked by a crash between the pattern write and the restore. */
static void blk_rw_selftest(void) {
    static uint8_t original[BLK_SECTOR_SIZE];
    static uint8_t pattern[BLK_SECTOR_SIZE];
    static uint8_t readback[BLK_SECTOR_SIZE];
    static uint8_t sb[BLK_SECTOR_SIZE];

    uint32_t lba = blkdev_sector_count() - 1;

    if (blkdev_read(lba, original, 1) != 0) {
        blk_log("RW self-test skipped (last-sector read failed)");
        return;
    }

    int last_all_zero = 1;
    for (int i = 0; i < BLK_SECTOR_SIZE; i++) {
        if (original[i] != 0) {
            last_all_zero = 0;
            break;
        }
    }
    int qdsk_owned = 0;
    if (blkdev_read(0, sb, 1) == 0) {
        static const char m[5] = {'Q', 'D', 'S', 'K', '1'};
        qdsk_owned =
            (sb[0] == m[0] && sb[1] == m[1] && sb[2] == m[2] && sb[3] == m[3] && sb[4] == m[4]);
    }
    if (!last_all_zero && !qdsk_owned) {
        blk_log("foreign volume — destructive RW self-test skipped (read probe OK)");
        return;
    }

    for (int i = 0; i < BLK_SECTOR_SIZE; i++) {
        pattern[i] = (uint8_t)(0xA5 ^ i);
    }
    if (blkdev_write(lba, pattern, 1) != 0) {
        blk_log("RW self-test FAILED (write)");
        return;
    }
    if (blkdev_read(lba, readback, 1) != 0) {
        blk_log("RW self-test FAILED (readback)");
        return;
    }
    for (int i = 0; i < BLK_SECTOR_SIZE; i++) {
        if (readback[i] != pattern[i]) {
            blk_log("RW self-test FAILED (verify mismatch)");
            return;
        }
    }
    if (blkdev_write(lba, original, 1) != 0) {
        blk_log("RW self-test FAILED (restore)");
        return;
    }
    blk_log("sector RW self-test OK (pattern verified, original restored)");
}

int blkdev_init(void) {
    for (int i = 0; i < DRIVER_COUNT; i++) {
        const blkdev_ops_t *d = drivers[i];
        if (d && d->init && d->init()) {
            bound = d;
            blk_rw_selftest();
            return 1;
        }
    }
    return 0; /* the ATA probe already logged "no disk" */
}

const char *blkdev_name(void) {
    return (bound && bound->name) ? bound->name : "none";
}

int blkdev_present(void) {
    return (bound && bound->present) ? bound->present() : 0;
}

uint32_t blkdev_sector_count(void) {
    return (bound && bound->sector_count) ? bound->sector_count() : 0;
}

int blkdev_read(uint32_t lba, void *buf, uint32_t count) {
    return (bound && bound->read) ? bound->read(lba, buf, count) : -1;
}

int blkdev_write(uint32_t lba, const void *buf, uint32_t count) {
    return (bound && bound->write) ? bound->write(lba, buf, count) : -1;
}
