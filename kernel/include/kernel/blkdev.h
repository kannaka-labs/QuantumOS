/**
 * QuantumOS block device abstraction (blkdev; #255)
 *
 * The persistence layer (ramfs.c: the QDSK superblock, tar archive, field
 * and audit blobs) was written against the ATA PIO driver and called
 * `ata_*` by name, so no other disk could carry it. Firecracker has no ATA
 * and no PCI, only virtio-mmio, so under Firecracker nothing persisted.
 * This header is the seam, shaped like netdev.h: one small ops table,
 * filled in by a driver, bound once at boot.
 *
 * `blkdev_init()` probes the registered drivers in order and keeps the
 * first that reports itself present, then runs the boot-time sector RW
 * self-test on it. With no disk every accessor gives the no-disk answer
 * and persistence stays off — honest degrade, boot continues.
 *
 * The contract every driver keeps (ramfs.c relies on it):
 *  - sectors are BLK_SECTOR_SIZE bytes;
 *  - read/write return 0 on success and -1 on any error, after a BOUNDED
 *    wait (never a hung kernel);
 *  - a write is durable when it returns: the driver flushes the device's
 *    write cache before reporting success.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef BLKDEV_H
#define BLKDEV_H

#include <kernel/types.h>

#define BLK_SECTOR_SIZE 512

/**
 * One block driver. `init` is the probe: it returns 1 only if a device was
 * found AND brought up, 0 otherwise, and logs its own outcome. `tag` names
 * the driver in boot lines ("ATA", "BLK") — the self-test logs under it.
 */
typedef struct {
    const char *name; /* "ata" or "virtio-blk" */
    const char *tag;
    int (*init)(void);
    int (*present)(void);
    uint32_t (*sector_count)(void);
    int (*read)(uint32_t lba, void *buf, uint32_t count);
    int (*write)(uint32_t lba, const void *buf, uint32_t count);
} blkdev_ops_t;

/* Probe every registered driver in order, bind the first present one and
 * self-test it. Safe to call with no disk. Returns 1 if one was bound. */
int blkdev_init(void);

/* Name of the bound driver, or "none". Never NULL — for logs. */
const char *blkdev_name(void);

/* Forwarded to the bound driver. With none bound: 0 / -1. */
int blkdev_present(void);
uint32_t blkdev_sector_count(void);
int blkdev_read(uint32_t lba, void *buf, uint32_t count);
int blkdev_write(uint32_t lba, const void *buf, uint32_t count);

#endif /* BLKDEV_H */
