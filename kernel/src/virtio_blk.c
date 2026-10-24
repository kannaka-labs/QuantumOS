/**
 * QuantumOS virtio-blk — the device, independent of transport (#255).
 *
 * The same driver runs over virtio-mmio (Firecracker) and virtio-pci
 * (QEMU/KVM): it talks to the device only through the virtio_transport_t
 * the probe fills in. Probe order inside init: mmio first, then pci —
 * the virtio-net order (ADR-0024).
 *
 * Modern only (VIRTIO_F_VERSION_1). Features negotiated: VERSION_1, and
 * RO and FLUSH when offered. Nothing else: no SEG_MAX/SIZE_MAX, so every
 * request is one data descriptor of at most CHUNK_SECTORS sectors, which
 * every device we target accepts.
 *
 * Fully polled, like the rtl8139 TX path: one request in flight, three
 * descriptors (header, data, status), a bounded wait on used->idx. The
 * persistence layer only touches the disk at boot (restore) and in
 * SYS_SYNC, both single-threaded, so there is nothing to overlap. The
 * avail ring asks for no interrupts, and any that arrives anyway is
 * acknowledged after each request, so a shared INTx line (the NIC's, on
 * QEMU) never stays asserted.
 *
 * Durability: a write returns only after the device completed it AND,
 * when FLUSH was negotiated, after a FLUSH request completed too — the
 * blkdev contract ramfs.c relies on ("flushed durable"). A device that
 * does not offer FLUSH (Firecracker's default cache_type "Unsafe") has no
 * cache we can drain; the boot line says so.
 *
 * DMA: kmalloc returns low identity-mapped memory, so a kernel VA is its
 * own physical address (the assumption virtio_net.c documents). Caller
 * buffers go through a bounce buffer, so they can live anywhere.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio_blk.h>
#include <kernel/virtio.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* virtio-blk feature bits (§5.2.3) */
#define F_BLK_RO 5
#define F_BLK_FLUSH 9

/* Device config (§5.2.4): capacity, u64 in 512-byte sectors, at 0 */
#define BLK_CFG_CAPACITY 0

/* Request types and status (§5.2.6) */
#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4
#define S_OK 0

#define REQ_Q 0

/* Ring size: a power of two; one request uses three descriptors. */
#define QSZ 4

/* Largest data transfer in one request (32 KiB). */
#define CHUNK_SECTORS 64

/* Bounded spin for one request's completion: a `pause` per pass, so a
 * dead device costs a few seconds at most, never a hang. */
#define VBLK_SPIN_MAX 20000000u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vring_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSZ];
    uint16_t used_event;
} vring_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} vring_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[QSZ];
    uint16_t avail_event;
} vring_used_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} blk_req_hdr_t;

static virtio_transport_t tp; /* the bound transport */
static int disk_present_flag;
static int read_only;
static int can_flush;
static uint32_t disk_sectors;

static volatile vring_desc_t *desc;
static volatile vring_avail_t *avail;
static volatile vring_used_t *used;
static uint16_t last_used;

static volatile blk_req_hdr_t *hdr;
static volatile uint8_t *status;
static uint8_t *bounce; /* CHUNK_SECTORS * BLK_SECTOR_SIZE */

static inline void vbarrier(void) {
    __asm__ volatile("" ::: "memory");
}

static void vzero(volatile void *p, uint32_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

/* Align a kmalloc result upward; the heap promises no alignment. */
static void *alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t *raw = (uint8_t *)kmalloc(size + align);
    if (!raw) {
        return 0;
    }
    uintptr_t a = (uintptr_t)raw;
    return (void *)((a + (align - 1)) & ~(uintptr_t)(align - 1));
}

/* Submit one request and wait for it. `data` may be 0 (FLUSH). Returns 0
 * when the device completed it with S_OK, -1 otherwise. */
static int submit(uint32_t type, uint64_t sector, uint8_t *data, uint32_t len) {
    hdr->type = type;
    hdr->reserved = 0;
    hdr->sector = sector;
    *status = 0xFF; /* the device overwrites it; 0xFF is never a valid status */

    desc[0].addr = (uint64_t)(uintptr_t)hdr;
    desc[0].len = (uint32_t)sizeof(blk_req_hdr_t);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1;
    uint16_t st = 1;
    if (data) {
        desc[1].addr = (uint64_t)(uintptr_t)data;
        desc[1].len = len;
        /* A read is the device writing into our buffer. */
        desc[1].flags = (uint16_t)(VRING_DESC_F_NEXT | (type == T_IN ? VRING_DESC_F_WRITE : 0));
        desc[1].next = 2;
        st = 2;
    }
    desc[st].addr = (uint64_t)(uintptr_t)status;
    desc[st].len = 1;
    desc[st].flags = VRING_DESC_F_WRITE;
    desc[st].next = 0;

    avail->ring[avail->idx % QSZ] = 0; /* the chain starts at descriptor 0 */
    vbarrier();
    avail->idx = (uint16_t)(avail->idx + 1);
    vbarrier();
    tp.notify(&tp, REQ_Q);

    int done = 0;
    for (uint32_t s = 0; s < VBLK_SPIN_MAX; s++) {
        if (used->idx != last_used) { /* volatile read: re-fetched each pass */
            last_used = used->idx;
            done = 1;
            break;
        }
        __asm__ volatile("pause");
    }
    (void)tp.isr_ack(&tp); /* never leave the line asserted */
    if (!done) {
        boot_log("BLK: request timed out - persistence disabled");
        disk_present_flag = 0;
        return -1;
    }
    vbarrier();
    return *status == S_OK ? 0 : -1;
}

static int bring_up(void) {
    /* Initialisation sequence (§3.1.1), in order. */
    tp.set_status(&tp, 0); /* reset */
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER);

    uint64_t offered = tp.get_features(&tp);
    if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) {
        boot_log("BLK: virtio-blk is not VERSION_1 - unsupported");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    uint64_t want = (1ULL << VIRTIO_F_VERSION_1) | (offered & (1ULL << F_BLK_RO)) |
                    (offered & (1ULL << F_BLK_FLUSH));
    tp.set_features(&tp, want);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK);
    if (!(tp.get_status(&tp) & VIRTIO_ST_FEATURES_OK)) {
        boot_log("BLK: virtio-blk refused our feature set");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    read_only = (want & (1ULL << F_BLK_RO)) != 0;
    can_flush = (want & (1ULL << F_BLK_FLUSH)) != 0;

    uint64_t cap = 0;
    for (int i = 7; i >= 0; i--) {
        cap = (cap << 8) | tp.config_read8(&tp, (uint32_t)(BLK_CFG_CAPACITY + i));
    }
    if (cap == 0) {
        boot_log("BLK: virtio-blk reports 0 sectors - persistence disabled");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    /* The QDSK layout addresses the disk with 32-bit LBAs, like LBA28 ATA
     * (up to 2 TiB here); a bigger disk is used up to that size. */
    disk_sectors = cap > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)cap;

    desc = (volatile vring_desc_t *)alloc_aligned((uint32_t)sizeof(vring_desc_t) * QSZ, 16);
    avail = (volatile vring_avail_t *)alloc_aligned((uint32_t)sizeof(vring_avail_t), 16);
    used = (volatile vring_used_t *)alloc_aligned((uint32_t)sizeof(vring_used_t), 16);
    hdr = (volatile blk_req_hdr_t *)alloc_aligned((uint32_t)sizeof(blk_req_hdr_t), 16);
    status = (volatile uint8_t *)kmalloc(1);
    bounce = (uint8_t *)kmalloc(CHUNK_SECTORS * BLK_SECTOR_SIZE);
    if (!desc || !avail || !used || !hdr || !status || !bounce) {
        boot_log("BLK: virtio-blk buffer alloc failed - persistence disabled");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    vzero(desc, (uint32_t)sizeof(vring_desc_t) * QSZ);
    vzero(avail, (uint32_t)sizeof(vring_avail_t));
    vzero(used, (uint32_t)sizeof(vring_used_t));
    avail->flags = VRING_AVAIL_F_NO_INTERRUPT; /* polled */
    last_used = 0;
    vbarrier();
    if (!tp.queue_setup(&tp, REQ_Q, QSZ, (uint64_t)(uintptr_t)desc, (uint64_t)(uintptr_t)avail,
                        (uint64_t)(uintptr_t)used)) {
        boot_log("BLK: virtio queue setup refused (missing queue, too small, or already live)");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }

    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK |
                           VIRTIO_ST_DRIVER_OK);
    return 1;
}

int virtio_blk_init(void) {
    if (disk_present_flag) {
        return 1;
    }
    if (!virtio_mmio_probe(VIRTIO_ID_BLOCK, &tp) && !virtio_pci_probe(VIRTIO_ID_BLOCK, &tp)) {
        return 0; /* silent: no virtio disk; the ATA probe follows */
    }
    if (!bring_up()) {
        return 0;
    }
    disk_present_flag = 1;

    /* The line the CI gates grep for, naming driver, transport and size. */
    if (tp.name[0] == 'p') {
        boot_log("BLK: virtio-blk up (pci), sectors:");
    } else {
        boot_log("BLK: virtio-blk up (mmio), sectors:");
    }
    early_console_write_hex(disk_sectors);
    if (read_only) {
        boot_log("BLK: device is read-only - sync will fail");
    }
    if (!can_flush) {
        boot_log("BLK: device offers no FLUSH - writes are durable only as the host makes them");
    }
    return 1;
}

int virtio_blk_present(void) {
    return disk_present_flag;
}

uint32_t virtio_blk_sector_count(void) {
    return disk_present_flag ? disk_sectors : 0;
}

static int in_range(uint32_t lba, uint32_t count) {
    return count != 0 && lba + count >= lba && lba + count <= disk_sectors;
}

int virtio_blk_read(uint32_t lba, void *buf, uint32_t count) {
    if (!disk_present_flag || !buf || !in_range(lba, count)) {
        return -1;
    }
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = count < CHUNK_SECTORS ? count : CHUNK_SECTORS;
        uint32_t bytes = n * BLK_SECTOR_SIZE;
        if (submit(T_IN, lba, bounce, bytes) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < bytes; i++) {
            p[i] = bounce[i];
        }
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

int virtio_blk_write(uint32_t lba, const void *buf, uint32_t count) {
    if (!disk_present_flag || read_only || !buf || !in_range(lba, count)) {
        return -1;
    }
    const uint8_t *p = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count < CHUNK_SECTORS ? count : CHUNK_SECTORS;
        uint32_t bytes = n * BLK_SECTOR_SIZE;
        for (uint32_t i = 0; i < bytes; i++) {
            bounce[i] = p[i];
        }
        if (submit(T_OUT, lba, bounce, bytes) != 0) {
            return -1;
        }
        p += bytes;
        lba += n;
        count -= n;
    }
    /* One flush for the whole run, as ata_write does. */
    if (can_flush && submit(T_FLUSH, 0, 0, 0) != 0) {
        return -1;
    }
    return 0;
}

/* ---- blkdev binding ---- */
const blkdev_ops_t virtio_blk_blkdev = {
    .name = "virtio-blk",
    .tag = "BLK",
    .init = virtio_blk_init,
    .present = virtio_blk_present,
    .sector_count = virtio_blk_sector_count,
    .read = virtio_blk_read,
    .write = virtio_blk_write,
};
