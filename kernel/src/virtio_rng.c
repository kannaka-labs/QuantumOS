/**
 * QuantumOS virtio-rng — the entropy device, independent of transport (#254).
 *
 * The same driver runs over virtio-mmio (Firecracker's "entropy" device) and
 * virtio-pci (QEMU's -device virtio-rng-pci), through the virtio_transport_t
 * the probe fills in. Probe order: mmio first, then pci (ADR-0024).
 *
 * The simplest virtio device (virtio 1.2 §5.4): no feature bits beyond
 * VERSION_1, no config space, one queue. The driver posts a device-writable
 * buffer; the device fills some of it with random bytes and reports how many
 * in the used element's length. Fully polled, one request in flight, a
 * bounded wait — the virtio-blk pattern.
 *
 * Honest labelling: these bytes come from the host's random source (its
 * /dev/urandom, typically). They are mixed into the kernel generator, and the
 * boot line says "host randomness", never "quantum". A qseed= handoff is the
 * only path that can carry QPU bits, and it keeps its own label.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio_rng.h>
#include <kernel/virtio.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

#define REQ_Q 0
#define QSZ 2

/* Largest single request. Callers ask for a few bytes at a time. */
#define CHUNK 64

/* Bounded spin per request: a dead device costs seconds, never a hang. */
#define VRNG_SPIN_MAX 20000000u

/* A device may return fewer bytes than asked (or zero); give up on a fill
 * after this many requests rather than loop forever on a starved host. */
#define VRNG_MAX_REQUESTS 64

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

static virtio_transport_t tp;
static int rng_present_flag;

static volatile vring_desc_t *desc;
static volatile vring_avail_t *avail;
static volatile vring_used_t *used;
static uint16_t last_used;
static volatile uint8_t *bounce; /* CHUNK bytes */

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

/* Post one device-writable buffer of `len` bytes and wait for it. Returns the
 * number of bytes the device wrote (0..len), or -1 if it never answered. */
static int request(uint32_t len) {
    desc[0].addr = (uint64_t)(uintptr_t)bounce;
    desc[0].len = len;
    desc[0].flags = VRING_DESC_F_WRITE;
    desc[0].next = 0;

    avail->ring[avail->idx % QSZ] = 0;
    vbarrier();
    avail->idx = (uint16_t)(avail->idx + 1);
    vbarrier();
    tp.notify(&tp, REQ_Q);

    int done = 0;
    for (uint32_t s = 0; s < VRNG_SPIN_MAX; s++) {
        if (used->idx != last_used) { /* volatile read: re-fetched each pass */
            done = 1;
            break;
        }
        __asm__ volatile("pause");
    }
    (void)tp.isr_ack(&tp); /* never leave a shared INTx line asserted */
    if (!done) {
        return -1;
    }
    vbarrier();
    uint32_t got = used->ring[last_used % QSZ].len;
    last_used = (uint16_t)(last_used + 1);
    return (int)(got > len ? len : got);
}

static int bring_up(void) {
    /* Initialisation sequence (§3.1.1), in order. */
    tp.set_status(&tp, 0); /* reset */
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER);

    uint64_t offered = tp.get_features(&tp);
    if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) {
        boot_log("RNG: virtio-rng is not VERSION_1 - unsupported");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    tp.set_features(&tp, 1ULL << VIRTIO_F_VERSION_1);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK);
    if (!(tp.get_status(&tp) & VIRTIO_ST_FEATURES_OK)) {
        boot_log("RNG: virtio-rng refused our feature set");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }

    desc = (volatile vring_desc_t *)alloc_aligned((uint32_t)sizeof(vring_desc_t) * QSZ, 16);
    avail = (volatile vring_avail_t *)alloc_aligned((uint32_t)sizeof(vring_avail_t), 16);
    used = (volatile vring_used_t *)alloc_aligned((uint32_t)sizeof(vring_used_t), 16);
    bounce = (volatile uint8_t *)kmalloc(CHUNK);
    if (!desc || !avail || !used || !bounce) {
        boot_log("RNG: virtio-rng buffer alloc failed");
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
        boot_log("RNG: virtio queue setup refused");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }

    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK |
                           VIRTIO_ST_DRIVER_OK);
    return 1;
}

int virtio_rng_init(void) {
    if (rng_present_flag) {
        return 1;
    }
    if (!virtio_mmio_probe(VIRTIO_ID_ENTROPY, &tp) && !virtio_pci_probe(VIRTIO_ID_ENTROPY, &tp)) {
        return 0; /* silent: no entropy device */
    }
    if (!bring_up()) {
        return 0;
    }
    rng_present_flag = 1;
    if (tp.name[0] == 'p') {
        boot_log("RNG: virtio-rng up (pci)");
    } else {
        boot_log("RNG: virtio-rng up (mmio)");
    }
    return 1;
}

int virtio_rng_present(void) {
    return rng_present_flag;
}

int virtio_rng_fill(void *buf, uint32_t len) {
    if (!rng_present_flag || !buf) {
        return -1;
    }
    uint8_t *p = (uint8_t *)buf;
    uint32_t requests = 0;
    while (len) {
        if (++requests > VRNG_MAX_REQUESTS) {
            boot_log("RNG: virtio-rng starved - entropy device dropped");
            rng_present_flag = 0;
            return -1;
        }
        uint32_t want = len < CHUNK ? len : CHUNK;
        int got = request(want);
        if (got < 0) {
            boot_log("RNG: virtio-rng request timed out - entropy device dropped");
            rng_present_flag = 0;
            return -1;
        }
        for (int i = 0; i < got; i++) {
            p[i] = bounce[i];
        }
        p += got;
        len -= (uint32_t)got;
    }
    return 0;
}
