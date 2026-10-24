/**
 * QuantumOS ATA PIO Block Driver (epic #71 phase 1)
 *
 * A minimal polled driver for the primary-master ATA disk (I/O base
 * 0x1F0, control 0x3F6), LBA28 only. This is the persistence substrate:
 * the writable RAM filesystem serializes itself to this disk on
 * SYS_SYNC and is restored from it at boot.
 *
 * Design constraints, in the house tradition:
 *  - Every hardware wait is BOUNDED (the console_write lesson): a
 *    wedged controller costs a bounded spin and an error return, never
 *    a hung kernel.
 *  - A diskless boot degrades honestly ("ATA: no disk — persistence
 *    disabled") and changes nothing else — the default -kernel CI path
 *    boots exactly as before.
 *  - Kernel-internal only: ring 3 never touches sectors. Filesystem
 *    write authority is a capability over DEVICE_ID_DISK, checked at
 *    the syscall layer; the block device itself has no user surface.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef ATA_H
#define ATA_H

#include <kernel/types.h>
#include <kernel/blkdev.h>

/* I/O base of the primary ATA channel. Also the capability resource_id
 * for filesystem-write authority (self-identifying, like DEVICE_ID_COM2
 * and DEVICE_ID_CONSOLE). */
#define ATA_IO_BASE 0x1F0
#define DEVICE_ID_DISK ATA_IO_BASE

#define ATA_SECTOR_SIZE BLK_SECTOR_SIZE

/* Probe the primary master via IDENTIFY. Logs the outcome; safe to call
 * once at boot. Returns 1 if a usable ATA disk is present, 0 if not.
 * Called through blkdev_init(), which then runs the sector RW self-test. */
int ata_init(void);

/* Non-zero once ata_init() found a disk. */
int ata_present(void);

/* Addressable LBA28 sectors on the detected disk (0 when absent). */
uint32_t ata_sector_count(void);

/* Read/write `count` sectors starting at `lba` into/from `buf`
 * (count * ATA_SECTOR_SIZE bytes). Returns 0 on success, -1 on any
 * error or timeout (bounded polling; never spins forever). Writes are
 * followed by a cache flush before returning. */
int ata_read(uint32_t lba, void *buf, uint32_t count);
int ata_write(uint32_t lba, const void *buf, uint32_t count);

/* The driver as a blkdev; bound by blkdev_init() via the probe list. The
 * persistence layer calls it only through blkdev_*, never by name. */
extern const blkdev_ops_t ata_blkdev;

#endif /* ATA_H */
