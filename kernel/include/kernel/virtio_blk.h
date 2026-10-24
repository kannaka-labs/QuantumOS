/**
 * QuantumOS virtio-blk (modern virtio 1.x; #255)
 *
 * One block driver over both virtio transports (kernel/include/kernel/
 * virtio.h): virtio-mmio for Firecracker, whose only disk is a virtio-mmio
 * drive, and virtio-pci for QEMU/KVM (`-drive ...,if=virtio`). It sits
 * behind the blkdev contract (blkdev.h) and is probed before ATA.
 *
 * Under Firecracker the drive's slot arrives on the kernel command line
 * like the NIC's (`virtio_mmio.device=4K@0xd0001000:6`); the cmdline is
 * parsed before blkdev_init(), so no extra wiring is needed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <kernel/types.h>
#include <kernel/blkdev.h>

/* The blkdev entry points. Non-static because the ops table takes their
 * addresses; the persistence layer calls them only through blkdev_*. */
int virtio_blk_init(void);
int virtio_blk_present(void);
uint32_t virtio_blk_sector_count(void);
int virtio_blk_read(uint32_t lba, void *buf, uint32_t count);
int virtio_blk_write(uint32_t lba, const void *buf, uint32_t count);

/* The driver as a blkdev; bound by blkdev_init() via the probe list. */
extern const blkdev_ops_t virtio_blk_blkdev;

#endif /* VIRTIO_BLK_H */
