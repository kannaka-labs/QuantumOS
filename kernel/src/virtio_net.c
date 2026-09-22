/**
 * QuantumOS virtio-net over virtio-mmio — implementation.
 *
 * Modern transport only (MMIO Version 2 / VIRTIO_F_VERSION_1). Legacy
 * virtio is deliberately unsupported: Firecracker is modern-only, and a
 * legacy path would double the surface for a device we never meet.
 *
 * Split virtqueues, two of them: 0 receiveq, 1 transmitq. RX descriptors
 * are filled once at init and recycled forever; the IRQ drains the used
 * ring into a small software queue the network layer pops, which is the
 * shape rtl8139.c already established. TX is polled on a single
 * descriptor with a bounded wait — the console_write / ATA lesson.
 *
 * DMA: kmalloc returns low-identity-mapped memory, so a kernel VA is its
 * own physical address (the same assumption rtl8139.c documents). Every
 * ring and buffer below relies on that and on nothing else.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio_net.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* ---- virtio-mmio register offsets (spec 4.2.2) ---- */
#define VM_MAGIC 0x000
#define VM_VERSION 0x004
#define VM_DEVICE_ID 0x008
#define VM_DEVICE_FEATURES 0x010
#define VM_DEVICE_FEATURES_SEL 0x014
#define VM_DRIVER_FEATURES 0x020
#define VM_DRIVER_FEATURES_SEL 0x024
#define VM_QUEUE_SEL 0x030
#define VM_QUEUE_NUM_MAX 0x034
#define VM_QUEUE_NUM 0x038
#define VM_QUEUE_READY 0x044
#define VM_QUEUE_NOTIFY 0x050
#define VM_INTERRUPT_STATUS 0x060
#define VM_INTERRUPT_ACK 0x064
#define VM_STATUS 0x070
#define VM_QUEUE_DESC_LOW 0x080
#define VM_QUEUE_DESC_HIGH 0x084
#define VM_QUEUE_DRIVER_LOW 0x090
#define VM_QUEUE_DRIVER_HIGH 0x094
#define VM_QUEUE_DEVICE_LOW 0x0a0
#define VM_QUEUE_DEVICE_HIGH 0x0a4
#define VM_CONFIG 0x100

#define VIRTIO_MAGIC 0x74726976u /* the four bytes "virt" */
#define VIRTIO_ID_NET 1

/* Status bits (spec 2.1) */
#define ST_ACKNOWLEDGE 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8
#define ST_FAILED 128

/* Features. VERSION_1 is bit 32, hence the two-word select dance. */
#define F_NET_MAC 5
#define F_VERSION_1 32

/* Descriptor flags */
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

/* Queue indices */
#define RX_Q 0
#define TX_Q 1

/* Ring size: power of two, and we require the device to allow at least
 * this many. 64 RX buffers of 1600 bytes is about 100 KiB. */
#define QSZ 64

/* The virtio-net header precedes every frame both ways. With VERSION_1
 * negotiated it is always 12 bytes (num_buffers is present). */
#define NET_HDR_LEN 12

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

typedef struct {
    vring_desc_t *desc;
    vring_avail_t *avail;
    vring_used_t *used;
    uint16_t last_used; /* our cursor into used->ring */
} vq_t;

/* ---- cmdline-supplied device slots ---- */
typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t irq;
} mmio_slot_t;

static mmio_slot_t slots[VIRTIO_MMIO_MAX_DEVICES];
static int slot_count;

/* ---- bound device state ---- */
static volatile uint8_t *reg; /* MMIO window of the chosen device */
static uint8_t nic_irq;
static int nic_present_flag;
static uint8_t mac[ETH_ADDR_LEN];

static vq_t rxq, txq;
static uint8_t *rx_buf[QSZ]; /* NET_HDR_LEN + NET_FRAME_MAX each */
static uint8_t *tx_buf;      /* single polled TX buffer */

/* Software RX queue the IRQ fills and the net layer drains — the same
 * hand-off rtl8139.c uses, so net.c's discipline is unchanged. */
#define RXQ_SLOTS 16
static uint8_t rxq_data[RXQ_SLOTS][NET_FRAME_MAX];
static uint16_t rxq_len[RXQ_SLOTS];
static volatile int rxq_head, rxq_tail;

/* Bounded spin for TX completion. */
#define VIRTIO_SPIN_MAX 1000000u

static inline uint32_t mmio_r(uint32_t off) {
    return *(volatile uint32_t *)(reg + off);
}

static inline void mmio_w(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(reg + off) = v;
}

/* Order our ring writes before the store the device may observe. */
static inline void vbarrier(void) {
    __asm__ volatile("" ::: "memory");
}

static void vzero(void *p, uint32_t n) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

/* Align a kmalloc result upward. The virtqueue parts want 16/2/4-byte
 * alignment and the heap makes no promise, so over-allocate and step. */
static void *alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t *raw = (uint8_t *)kmalloc(size + align);
    if (!raw) {
        return 0;
    }
    uintptr_t a = (uintptr_t)raw;
    uintptr_t aligned = (a + (align - 1)) & ~(uintptr_t)(align - 1);
    return (void *)aligned;
}

/* Set up one split virtqueue and hand its three physical addresses to
 * the device. Returns 1 on success. */
static int vq_setup(int index, vq_t *q) {
    mmio_w(VM_QUEUE_SEL, (uint32_t)index);
    if (mmio_r(VM_QUEUE_READY) != 0) {
        boot_log("NET: virtio queue already ready - refusing");
        return 0;
    }
    uint32_t max = mmio_r(VM_QUEUE_NUM_MAX);
    if (max == 0) {
        boot_log("NET: virtio queue unavailable");
        return 0;
    }
    if (max < QSZ) {
        /* Our rings are fixed-size structs; a smaller device queue would
         * mean the device reads past what we sized. Refuse honestly. */
        boot_log("NET: virtio QueueNumMax below our ring size - unsupported");
        return 0;
    }

    q->desc = (vring_desc_t *)alloc_aligned((uint32_t)sizeof(vring_desc_t) * QSZ, 16);
    if (!q->desc) {
        return 0;
    }
    q->avail = (vring_avail_t *)alloc_aligned((uint32_t)sizeof(vring_avail_t), 16);
    if (!q->avail) {
        return 0;
    }
    q->used = (vring_used_t *)alloc_aligned((uint32_t)sizeof(vring_used_t), 16);
    if (!q->used) {
        return 0;
    }
    vzero(q->desc, (uint32_t)sizeof(vring_desc_t) * QSZ);
    vzero(q->avail, (uint32_t)sizeof(vring_avail_t));
    vzero(q->used, (uint32_t)sizeof(vring_used_t));
    q->last_used = 0;

    mmio_w(VM_QUEUE_NUM, QSZ);
    /* kmalloc is identity-mapped low memory: VA == phys, so the high
     * halves are zero. Written anyway, because the spec requires both. */
    mmio_w(VM_QUEUE_DESC_LOW, (uint32_t)(uintptr_t)q->desc);
    mmio_w(VM_QUEUE_DESC_HIGH, (uint32_t)((uint64_t)(uintptr_t)q->desc >> 32));
    mmio_w(VM_QUEUE_DRIVER_LOW, (uint32_t)(uintptr_t)q->avail);
    mmio_w(VM_QUEUE_DRIVER_HIGH, (uint32_t)((uint64_t)(uintptr_t)q->avail >> 32));
    mmio_w(VM_QUEUE_DEVICE_LOW, (uint32_t)(uintptr_t)q->used);
    mmio_w(VM_QUEUE_DEVICE_HIGH, (uint32_t)((uint64_t)(uintptr_t)q->used >> 32));
    vbarrier();
    mmio_w(VM_QUEUE_READY, 1);
    return 1;
}

/* Publish one RX descriptor back to the device. */
static void rx_publish(uint16_t d) {
    rxq.desc[d].addr = (uint64_t)(uintptr_t)rx_buf[d];
    rxq.desc[d].len = NET_HDR_LEN + NET_FRAME_MAX;
    rxq.desc[d].flags = VRING_DESC_F_WRITE; /* the device writes into it */
    rxq.desc[d].next = 0;
    rxq.avail->ring[rxq.avail->idx % QSZ] = d;
    vbarrier();
    rxq.avail->idx++;
    vbarrier();
}

int virtio_mmio_add_device(uint64_t base, uint64_t size, uint8_t irq) {
    if (slot_count >= VIRTIO_MMIO_MAX_DEVICES || base == 0) {
        return 0;
    }
    if (irq >= 16) {
        /* No usable 8259 line means RX can never fire. Refuse here
         * rather than unmask a bogus vector later (the rtl8139 lesson). */
        return 0;
    }
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].base == base) {
            return 1; /* idempotent: the same slot named twice */
        }
    }
    slots[slot_count].base = base;
    slots[slot_count].size = size;
    slots[slot_count].irq = irq;
    slot_count++;
    return 1;
}

int virtio_mmio_device_count(void) {
    return slot_count;
}

int virtio_net_init(void) {
    if (nic_present_flag) {
        return 1;
    }
    if (slot_count == 0) {
        /* No virtio_mmio.device= on the cmdline: not an error, just not
         * this hypervisor. Silent - the rtl8139 probe follows. */
        return 0;
    }

    int found = 0;
    for (int s = 0; s < slot_count; s++) {
        reg = (volatile uint8_t *)(uintptr_t)slots[s].base;
        if (mmio_r(VM_MAGIC) != VIRTIO_MAGIC) {
            continue;
        }
        if (mmio_r(VM_VERSION) != 2) {
            continue; /* legacy transport deliberately unsupported */
        }
        if (mmio_r(VM_DEVICE_ID) != VIRTIO_ID_NET) {
            continue; /* a block device or similar - not ours */
        }
        nic_irq = slots[s].irq;
        found = 1;
        break;
    }
    if (!found) {
        reg = 0;
        return 0;
    }

    /* Spec 3.1.1 initialisation, in order. */
    mmio_w(VM_STATUS, 0); /* reset */
    mmio_w(VM_STATUS, ST_ACKNOWLEDGE);
    mmio_w(VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER);

    /* Feature negotiation. We require VERSION_1 and want MAC, and accept
     * nothing else - which is what keeps the header length fixed at 12. */
    mmio_w(VM_DEVICE_FEATURES_SEL, 0);
    uint32_t feat_lo = mmio_r(VM_DEVICE_FEATURES);
    mmio_w(VM_DEVICE_FEATURES_SEL, 1);
    uint32_t feat_hi = mmio_r(VM_DEVICE_FEATURES);

    if (!(feat_hi & (1u << (F_VERSION_1 - 32)))) {
        boot_log("NET: virtio-net is not VERSION_1 - unsupported");
        mmio_w(VM_STATUS, ST_FAILED);
        return 0;
    }
    uint32_t want_lo = feat_lo & (1u << F_NET_MAC);
    uint32_t want_hi = (1u << (F_VERSION_1 - 32));

    mmio_w(VM_DRIVER_FEATURES_SEL, 0);
    mmio_w(VM_DRIVER_FEATURES, want_lo);
    mmio_w(VM_DRIVER_FEATURES_SEL, 1);
    mmio_w(VM_DRIVER_FEATURES, want_hi);

    mmio_w(VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK);
    if (!(mmio_r(VM_STATUS) & ST_FEATURES_OK)) {
        boot_log("NET: virtio-net refused our feature set");
        mmio_w(VM_STATUS, ST_FAILED);
        return 0;
    }

    /* MAC from config space when the device offered it, otherwise a
     * locally-administered address so the stack still has one. */
    if (want_lo & (1u << F_NET_MAC)) {
        for (int i = 0; i < ETH_ADDR_LEN; i++) {
            mac[i] = *(volatile uint8_t *)(reg + VM_CONFIG + i);
        }
    } else {
        mac[0] = 0x02;
        mac[1] = 0x00;
        mac[2] = 0x00;
        mac[3] = 0x00;
        mac[4] = 0x00;
        mac[5] = 0x01;
    }

    if (!vq_setup(RX_Q, &rxq) || !vq_setup(TX_Q, &txq)) {
        mmio_w(VM_STATUS, ST_FAILED);
        return 0;
    }

    /* RX buffers, published once and recycled for the life of the boot. */
    for (int i = 0; i < QSZ; i++) {
        rx_buf[i] = (uint8_t *)kmalloc(NET_HDR_LEN + NET_FRAME_MAX);
        if (!rx_buf[i]) {
            boot_log("NET: virtio-net RX buffer alloc failed - networking disabled");
            mmio_w(VM_STATUS, ST_FAILED);
            return 0;
        }
    }
    tx_buf = (uint8_t *)kmalloc(NET_HDR_LEN + NET_FRAME_MAX);
    if (!tx_buf) {
        boot_log("NET: virtio-net TX buffer alloc failed - networking disabled");
        mmio_w(VM_STATUS, ST_FAILED);
        return 0;
    }
    for (uint16_t i = 0; i < QSZ; i++) {
        rx_publish(i);
    }

    mmio_w(VM_STATUS, ST_ACKNOWLEDGE | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    mmio_w(VM_QUEUE_SEL, RX_Q);
    mmio_w(VM_QUEUE_NOTIFY, RX_Q);

    rxq_head = 0;
    rxq_tail = 0;
    nic_present_flag = 1;

    boot_log("NET: virtio-net up - MAC:");
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        early_console_write_hex(mac[i]);
    }
    boot_log("NET: virtio-net IRQ line:");
    early_console_write_hex(nic_irq);
    return 1;
}

int virtio_net_present(void) {
    return nic_present_flag;
}

void virtio_net_get_mac(uint8_t out[ETH_ADDR_LEN]) {
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out[i] = mac[i];
    }
}

uint8_t virtio_net_irq_line(void) {
    return nic_irq;
}

int virtio_net_transmit(const uint8_t *frame, uint16_t len) {
    if (!nic_present_flag || len == 0 || len > NET_FRAME_MAX) {
        return -1;
    }
    /* One descriptor carrying header + frame. The device reads it, so no
     * WRITE flag. A single outstanding segment, polled - matching the
     * rtl8139 TX discipline rather than inventing a second one. */
    vzero(tx_buf, NET_HDR_LEN);
    for (uint16_t i = 0; i < len; i++) {
        tx_buf[NET_HDR_LEN + i] = frame[i];
    }

    uint16_t d = 0; /* single TX descriptor, index 0 */
    txq.desc[d].addr = (uint64_t)(uintptr_t)tx_buf;
    txq.desc[d].len = (uint32_t)(NET_HDR_LEN + len);
    txq.desc[d].flags = 0;
    txq.desc[d].next = 0;

    uint16_t before = txq.used->idx;
    txq.avail->ring[txq.avail->idx % QSZ] = d;
    vbarrier();
    txq.avail->idx++;
    vbarrier();
    mmio_w(VM_QUEUE_SEL, TX_Q);
    mmio_w(VM_QUEUE_NOTIFY, TX_Q);

    for (uint32_t s = 0; s < VIRTIO_SPIN_MAX; s++) {
        if (txq.used->idx != before) {
            txq.last_used = txq.used->idx;
            return 0;
        }
    }
    return -1; /* bounded wait expired - report, never hang */
}

void virtio_net_irq(void) {
    if (!nic_present_flag) {
        return;
    }
    uint32_t st = mmio_r(VM_INTERRUPT_STATUS);
    if (st == 0) {
        return;
    }
    mmio_w(VM_INTERRUPT_ACK, st);

    while (rxq.last_used != rxq.used->idx) {
        vring_used_elem_t *e = &rxq.used->ring[rxq.last_used % QSZ];
        uint16_t d = (uint16_t)e->id;
        uint32_t total = e->len;

        if (d < QSZ && total > NET_HDR_LEN) {
            uint32_t flen = total - NET_HDR_LEN;
            if (flen > NET_FRAME_MAX) {
                flen = NET_FRAME_MAX;
            }
            int nxt = (rxq_tail + 1) % RXQ_SLOTS;
            if (nxt != rxq_head) { /* drop when full, never overwrite */
                const uint8_t *src = rx_buf[d] + NET_HDR_LEN;
                for (uint32_t i = 0; i < flen; i++) {
                    rxq_data[rxq_tail][i] = src[i];
                }
                rxq_len[rxq_tail] = (uint16_t)flen;
                rxq_tail = nxt;
            }
            rx_publish(d); /* recycle immediately */
        }
        rxq.last_used++;
    }
    mmio_w(VM_QUEUE_SEL, RX_Q);
    mmio_w(VM_QUEUE_NOTIFY, RX_Q);
}

uint16_t virtio_net_receive(uint8_t *buf, uint16_t max) {
    if (rxq_head == rxq_tail) {
        return 0;
    }
    uint16_t n = rxq_len[rxq_head];
    if (n > max) {
        n = max;
    }
    for (uint16_t i = 0; i < n; i++) {
        buf[i] = rxq_data[rxq_head][i];
    }
    rxq_head = (rxq_head + 1) % RXQ_SLOTS;
    return n;
}

/* ---- netdev binding ---- */
const netdev_ops_t virtio_net_netdev = {
    .name = "virtio-net",
    .init = virtio_net_init,
    .present = virtio_net_present,
    .get_mac = virtio_net_get_mac,
    .transmit = virtio_net_transmit,
    .irq = virtio_net_irq,
    .irq_line = virtio_net_irq_line,
    .receive = virtio_net_receive,
};
