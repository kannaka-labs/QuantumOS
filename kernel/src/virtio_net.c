/**
 * QuantumOS virtio-net — the device, independent of transport (ADR-0024).
 *
 * The same driver runs over virtio-mmio (Firecracker) and virtio-pci
 * (QEMU/KVM, the clouds): it talks to the device only through the
 * virtio_transport_t the probe fills in (kernel/include/kernel/virtio.h).
 * Probe order inside init: mmio first (cheap, and the only option on a
 * bus-less microVM), then pci.
 *
 * Modern only (VIRTIO_F_VERSION_1). Features negotiated: VERSION_1, MAC,
 * STATUS. No offloads (CSUM, GSO/TSO, MRG_RXBUF, multiqueue) — the stack
 * computes its own checksums and sends one frame per buffer, so the
 * virtio-net header is always the 12-byte form and always zero on TX.
 *
 * Split virtqueues, two of them: 0 receiveq, 1 transmitq. RX descriptors
 * are published once and recycled; the IRQ drains the used ring into a
 * small software queue the network layer pops (the shape rtl8139.c set).
 * If the interrupt never arrives (a mis-routed INTx line), receive()
 * drains the used ring itself — a polling fallback, so the stack still
 * works. TX is one descriptor, polled to completion with a bounded wait.
 *
 * Ring memory is read and written through volatile accessors: the device
 * updates used->idx behind the compiler's back, and a cached read in the
 * TX wait loop would spin forever.
 *
 * DMA: kmalloc returns low identity-mapped memory, so a kernel VA is its
 * own physical address (the assumption rtl8139.c documents).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio_net.h>
#include <kernel/virtio.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* virtio-net feature bits (§5.1.3) */
#define F_NET_MAC 5
#define F_NET_STATUS 16

/* Device config (§5.1.4): mac[6] at 0, status u16 at 6 */
#define NET_CFG_STATUS 6
#define NET_S_LINK_UP 1

#define RX_Q 0
#define TX_Q 1

/* Ring size: a power of two. 64 RX buffers of ~1.6 KB is about 100 KiB. */
#define QSZ 64

/* With VERSION_1 the header is always 12 bytes (num_buffers present). */
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
    volatile vring_desc_t *desc;
    volatile vring_avail_t *avail;
    volatile vring_used_t *used;
    uint16_t last_used; /* our cursor into used->ring */
} vq_t;

static virtio_transport_t tp; /* the bound transport */
static int nic_present_flag;
static uint8_t mac[ETH_ADDR_LEN];

static vq_t rxq, txq;
static uint8_t *rx_buf[QSZ]; /* NET_HDR_LEN + NET_FRAME_MAX each */
static uint8_t *tx_buf;      /* single polled TX buffer */

/* Software RX queue the drain fills and the net layer pops. */
#define RXQ_SLOTS 16
static uint8_t rxq_data[RXQ_SLOTS][NET_FRAME_MAX];
static uint16_t rxq_len[RXQ_SLOTS];
static volatile int rxq_head, rxq_tail;

/* Bounded spin for TX completion. */
#define VIRTIO_SPIN_MAX 1000000u

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

static int vq_setup(uint16_t index, vq_t *q) {
    q->desc = (volatile vring_desc_t *)alloc_aligned((uint32_t)sizeof(vring_desc_t) * QSZ, 16);
    q->avail = (volatile vring_avail_t *)alloc_aligned((uint32_t)sizeof(vring_avail_t), 16);
    q->used = (volatile vring_used_t *)alloc_aligned((uint32_t)sizeof(vring_used_t), 16);
    if (!q->desc || !q->avail || !q->used) {
        return 0;
    }
    vzero(q->desc, (uint32_t)sizeof(vring_desc_t) * QSZ);
    vzero(q->avail, (uint32_t)sizeof(vring_avail_t));
    vzero(q->used, (uint32_t)sizeof(vring_used_t));
    q->last_used = 0;
    vbarrier();
    if (!tp.queue_setup(&tp, index, QSZ, (uint64_t)(uintptr_t)q->desc,
                        (uint64_t)(uintptr_t)q->avail, (uint64_t)(uintptr_t)q->used)) {
        boot_log("NET: virtio queue setup refused (missing queue, too small, or already live)");
        return 0;
    }
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
    rxq.avail->idx = (uint16_t)(rxq.avail->idx + 1);
    vbarrier();
}

/* Move every completed RX buffer into the software queue and recycle it.
 * Returns how many were consumed. Callers keep it single-threaded: the IRQ
 * runs with interrupts off, receive() disables them around its call. */
static int rx_drain(void) {
    int n = 0;
    while (rxq.last_used != rxq.used->idx) {
        volatile vring_used_elem_t *e = &rxq.used->ring[rxq.last_used % QSZ];
        uint16_t d = (uint16_t)e->id;
        uint32_t total = e->len;
        if (d < QSZ) {
            if (total > NET_HDR_LEN) {
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
            }
            rx_publish(d);
            n++;
        }
        rxq.last_used++;
    }
    if (n) {
        tp.notify(&tp, RX_Q);
    }
    return n;
}

static int bring_up(void) {
    /* Initialisation sequence (§3.1.1), in order. */
    tp.set_status(&tp, 0); /* reset */
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER);

    uint64_t offered = tp.get_features(&tp);
    if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) {
        boot_log("NET: virtio-net is not VERSION_1 - unsupported");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    uint64_t want = (1ULL << VIRTIO_F_VERSION_1) | (offered & (1ULL << F_NET_MAC)) |
                    (offered & (1ULL << F_NET_STATUS));
    tp.set_features(&tp, want);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK);
    if (!(tp.get_status(&tp) & VIRTIO_ST_FEATURES_OK)) {
        boot_log("NET: virtio-net refused our feature set");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }

    if (want & (1ULL << F_NET_MAC)) {
        for (int i = 0; i < ETH_ADDR_LEN; i++) {
            mac[i] = tp.config_read8(&tp, (uint32_t)i);
        }
    } else {
        /* A locally-administered address so the stack still has one. */
        mac[0] = 0x02;
        mac[1] = mac[2] = mac[3] = mac[4] = 0x00;
        mac[5] = 0x01;
    }

    if (!vq_setup(RX_Q, &rxq) || !vq_setup(TX_Q, &txq)) {
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    for (int i = 0; i < QSZ; i++) {
        rx_buf[i] = (uint8_t *)kmalloc(NET_HDR_LEN + NET_FRAME_MAX);
        if (!rx_buf[i]) {
            boot_log("NET: virtio-net RX buffer alloc failed - networking disabled");
            tp.set_status(&tp, VIRTIO_ST_FAILED);
            return 0;
        }
    }
    tx_buf = (uint8_t *)kmalloc(NET_HDR_LEN + NET_FRAME_MAX);
    if (!tx_buf) {
        boot_log("NET: virtio-net TX buffer alloc failed - networking disabled");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    for (uint16_t i = 0; i < QSZ; i++) {
        rx_publish(i);
    }

    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK |
                           VIRTIO_ST_DRIVER_OK);
    tp.notify(&tp, RX_Q);

    if (want & (1ULL << F_NET_STATUS)) {
        uint16_t st = (uint16_t)(tp.config_read8(&tp, NET_CFG_STATUS) |
                                 ((uint16_t)tp.config_read8(&tp, NET_CFG_STATUS + 1) << 8));
        if (!(st & NET_S_LINK_UP)) {
            boot_log("NET: virtio-net link is down at init (continuing)");
        }
    }
    return 1;
}

int virtio_net_init(void) {
    if (nic_present_flag) {
        return 1;
    }
    /* mmio first: on a bus-less microVM it is the only option, and the
     * probe is a few register reads at cmdline-named addresses. */
    if (!virtio_mmio_probe(VIRTIO_ID_NET, &tp) && !virtio_pci_probe(VIRTIO_ID_NET, &tp)) {
        return 0; /* silent: not this hypervisor; the rtl8139 probe follows */
    }
    if (!bring_up()) {
        return 0;
    }
    rxq_head = 0;
    rxq_tail = 0;
    nic_present_flag = 1;

    /* One line the CI gates grep for, naming driver, transport and MAC. */
    if (tp.name[0] == 'p') {
        boot_log("NET: virtio-net up (pci) - MAC:");
    } else {
        boot_log("NET: virtio-net up (mmio) - MAC:");
    }
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        early_console_write_hex(mac[i]);
    }
    boot_log("NET: virtio-net IRQ line:");
    early_console_write_hex(tp.irq);
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
    return tp.irq;
}

int virtio_net_transmit(const uint8_t *frame, uint16_t len) {
    if (!nic_present_flag || len == 0 || len > NET_FRAME_MAX) {
        return -1;
    }
    /* One descriptor, header + frame, device-readable. One outstanding
     * transmit at a time, polled — the rtl8139 discipline. */
    for (uint32_t i = 0; i < NET_HDR_LEN; i++) {
        tx_buf[i] = 0;
    }
    for (uint16_t i = 0; i < len; i++) {
        tx_buf[NET_HDR_LEN + i] = frame[i];
    }
    txq.desc[0].addr = (uint64_t)(uintptr_t)tx_buf;
    txq.desc[0].len = (uint32_t)(NET_HDR_LEN + len);
    txq.desc[0].flags = 0;
    txq.desc[0].next = 0;

    uint16_t before = txq.used->idx;
    txq.avail->ring[txq.avail->idx % QSZ] = 0;
    vbarrier();
    txq.avail->idx = (uint16_t)(txq.avail->idx + 1);
    vbarrier();
    tp.notify(&tp, TX_Q);

    for (uint32_t s = 0; s < VIRTIO_SPIN_MAX; s++) {
        if (txq.used->idx != before) { /* volatile read: re-fetched each pass */
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
    if (tp.isr_ack(&tp) == 0) {
        return; /* a shared line, not us */
    }
    rx_drain();
}

uint16_t virtio_net_receive(uint8_t *buf, uint16_t max) {
    if (!nic_present_flag) {
        return 0;
    }
    if (rxq_head == rxq_tail) {
        /* Polling fallback: drain the used ring ourselves, with interrupts
         * off so the IRQ path cannot run rx_drain concurrently. */
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags)::"memory");
        rx_drain();
        if (flags & (1ULL << 9)) {
            __asm__ volatile("sti" ::: "memory");
        }
        if (rxq_head == rxq_tail) {
            return 0;
        }
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
