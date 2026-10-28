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
 * STATUS, and since #253 three offloads, each taken only if the device
 * offers it and the cmdline did not switch it off
 * (`virtio_net.nofeat=csum,mrgrx,msix`):
 *
 *   - CSUM + GUEST_CSUM: TX stores the TCP pseudo-header sum and marks the
 *     frame NEEDS_CSUM; the device finishes it (§5.1.6.2). The stack is told
 *     through netdev_set_tx_csum_offload() so it stops walking the payload.
 *     RX frames the device verified (DATA_VALID) skip the stack's check
 *     (netdev_rx_csum_valid_set()); frames it hands over NEEDS_CSUM are
 *     finished here before the stack sees them (§5.1.6.4.1).
 *   - MRG_RXBUF: a frame may span several RX descriptors; the header's
 *     num_buffers says how many used entries belong to it (§5.1.6.4). With
 *     it RX buffers are posted as 1 KiB chunks, so a full-size frame
 *     arrives in two and every ACK costs one.
 *   - MSI-X (virtio-pci only): one vector for config changes and one per
 *     queue, through the transport's msix_enable; INTx otherwise.
 *
 * No TSO/GSO and no multiqueue (#253 out of scope): the virtio-net header
 * is the 12-byte VERSION_1 form and the TX header is zero except the
 * checksum fields.
 *
 * Split virtqueues, two of them: 0 receiveq, 1 transmitq. RX descriptors
 * are published once and recycled; the IRQ drains the used ring into a
 * small software queue the network layer pops (the shape rtl8139.c set).
 * If the interrupt never arrives (a mis-routed INTx line), receive()
 * drains the used ring itself — a polling fallback, so the stack still
 * works. TX is one descriptor, polled to completion with a bounded wait,
 * and the TX ring asks the device not to interrupt (the completion is
 * observed by the poll, never by a handler).
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
#include <kernel/pci_msix.h>
#include <kernel/interrupts.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* virtio-net feature bits (§5.1.3) */
#define F_NET_CSUM 0
#define F_NET_GUEST_CSUM 1
#define F_NET_MAC 5
#define F_NET_MRG_RXBUF 15
#define F_NET_STATUS 16

/* Device config (§5.1.4): mac[6] at 0, status u16 at 6 */
#define NET_CFG_STATUS 6
#define NET_S_LINK_UP 1

#define RX_Q 0
#define TX_Q 1
#define NUM_QUEUES 2

/* Ring size: a power of two. 64 RX buffers of ~1.6 KB is about 100 KiB. */
#define QSZ 64

/* With VERSION_1 the header is always 12 bytes (num_buffers present). */
#define NET_HDR_LEN 12

/* struct virtio_net_hdr fields (§5.1.6), little-endian */
#define HDR_FLAGS 0
#define HDR_CSUM_START 6
#define HDR_CSUM_OFFSET 8
#define HDR_NUM_BUFFERS 10
#define HDR_F_NEEDS_CSUM 1
#define HDR_F_DATA_VALID 2

/* Ethernet / IPv4 / TCP offsets the TX offload needs to find the segment */
#define ETH_HLEN 14
#define ETH_TYPE_IPV4 0x0800
#define IP_PROTO_TCP 6
#define TCP_CSUM_OFFSET 16

/* RX descriptor size: the whole frame plus header when every frame must
 * fit one buffer; 1 KiB chunks once the device may merge (MRG_RXBUF). */
#define RX_CHUNK_SINGLE (NET_HDR_LEN + NET_FRAME_MAX)
#define RX_CHUNK_MRG 1024

/* `virtio_net.nofeat=` switches */
#define NOFEAT_CSUM 1u
#define NOFEAT_MRGRX 2u
#define NOFEAT_MSIX 4u

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

/* Feature state (#253): what the cmdline forbade, what was negotiated. */
static uint32_t nofeat_mask;
static int feat_csum, feat_mrgrx, feat_msix;
static uint32_t rx_chunk = RX_CHUNK_SINGLE; /* bytes posted per RX descriptor */
static volatile int msix_rx_seen;           /* first MSI-X RX interrupt logged once */

static vq_t rxq, txq;
static uint8_t *rx_buf[QSZ]; /* rx_chunk bytes each */
static uint8_t *tx_buf;      /* single polled TX buffer */

/* Software RX queue the drain fills and the net layer pops. */
#define RXQ_SLOTS 16
static uint8_t rxq_data[RXQ_SLOTS][NET_FRAME_MAX];
static uint16_t rxq_len[RXQ_SLOTS];
static uint8_t rxq_csum_ok[RXQ_SLOTS]; /* device said DATA_VALID */
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

/* EtherType of a frame, big-endian on the wire. */
static inline uint16_t frame_type(const uint8_t *frame) {
    return (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
}

static inline uint16_t le16_at(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline void le16_put(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
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
    rxq.desc[d].len = rx_chunk;
    rxq.desc[d].flags = VRING_DESC_F_WRITE; /* the device writes into it */
    rxq.desc[d].next = 0;
    rxq.avail->ring[rxq.avail->idx % QSZ] = d;
    vbarrier();
    rxq.avail->idx = (uint16_t)(rxq.avail->idx + 1);
    vbarrier();
}

/* Finish a checksum the device left partial (GUEST_CSUM, §5.1.6.4.1): the
 * field at csum_start + csum_offset holds the pseudo-header sum; add the
 * bytes from csum_start to the end and store the complement. Returns 0 if
 * the offsets do not fit the frame (then the frame is dropped: the stack
 * would reject its checksum anyway). */
static int rx_finish_csum(uint8_t *f, uint32_t flen, uint16_t start, uint16_t offset) {
    if ((uint32_t)start + offset + 2 > flen) {
        return 0;
    }
    /* Sum to the end of the IP datagram, not the frame: a padded runt would
     * otherwise fold its padding into the checksum. */
    if (flen >= ETH_HLEN + 20 && ((uint16_t)frame_type(f)) == ETH_TYPE_IPV4) {
        uint32_t ip_end = (uint32_t)ETH_HLEN + (((uint32_t)f[ETH_HLEN + 2] << 8) | f[ETH_HLEN + 3]);
        if (ip_end >= (uint32_t)start + offset + 2 && ip_end < flen) {
            flen = ip_end;
        }
    }
    uint32_t sum = 0;
    uint32_t i = start;
    while (i + 1 < flen) {
        sum += (uint32_t)((f[i] << 8) | f[i + 1]);
        i += 2;
    }
    if (i < flen) {
        sum += (uint32_t)(f[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    uint16_t c = (uint16_t)~sum;
    f[start + offset] = (uint8_t)(c >> 8);
    f[start + offset + 1] = (uint8_t)c;
    return 1;
}

/* Move every completed RX frame into the software queue and recycle its
 * descriptors. Returns how many descriptors were consumed. Callers keep it
 * single-threaded: the IRQ runs with interrupts off, receive() disables
 * them around its call.
 *
 * A frame is one used entry, or with MRG_RXBUF the num_buffers consecutive
 * entries the header in the first one announces. The device publishes all
 * of a frame's entries with one used->idx bump, so a count that reaches
 * past idx is malformed: what is there is recycled and the frame dropped. */
static int rx_drain(void) {
    int n = 0;
    while (rxq.last_used != rxq.used->idx) {
        uint16_t pending = (uint16_t)(rxq.used->idx - rxq.last_used);
        volatile vring_used_elem_t *e0 = &rxq.used->ring[rxq.last_used % QSZ];
        uint16_t d0 = (uint16_t)e0->id;
        uint32_t len0 = e0->len;
        if (d0 >= QSZ) {
            rxq.last_used++; /* not ours to recycle; skip the entry */
            continue;
        }
        /* The header, copied out before the first chunk is republished:
         * once a descriptor is back on the avail ring the device owns it. */
        uint8_t hdr[NET_HDR_LEN];
        int have_hdr = len0 >= NET_HDR_LEN && len0 <= rx_chunk;
        for (uint32_t i = 0; i < NET_HDR_LEN; i++) {
            hdr[i] = have_hdr ? rx_buf[d0][i] : 0;
        }
        uint16_t nbufs = 1;
        if (feat_mrgrx && have_hdr) {
            nbufs = le16_at(hdr + HDR_NUM_BUFFERS);
            if (nbufs == 0) {
                nbufs = 1;
            }
        }
        int drop = !have_hdr;
        if (nbufs > pending) {
            nbufs = pending;
            drop = 1;
        }
        int slot = rxq_tail;
        int nxt = (rxq_tail + 1) % RXQ_SLOTS;
        if (nxt == rxq_head) {
            drop = 1; /* software queue full: drop, never overwrite */
        }
        uint32_t flen = 0;
        for (uint16_t k = 0; k < nbufs; k++) {
            volatile vring_used_elem_t *e = &rxq.used->ring[(rxq.last_used + k) % QSZ];
            uint16_t d = (uint16_t)e->id;
            uint32_t len = e->len;
            if (d >= QSZ) {
                drop = 1;
                continue;
            }
            if (len > rx_chunk) {
                len = rx_chunk; /* never read past what was posted */
            }
            uint32_t skip = (k == 0) ? NET_HDR_LEN : 0;
            if (!drop && len > skip) {
                uint32_t take = len - skip;
                if (flen + take > NET_FRAME_MAX) {
                    drop = 1; /* longer than the stack accepts */
                } else {
                    const uint8_t *src = rx_buf[d] + skip;
                    for (uint32_t i = 0; i < take; i++) {
                        rxq_data[slot][flen + i] = src[i];
                    }
                    flen += take;
                }
            }
            rx_publish(d);
            n++;
        }
        rxq.last_used = (uint16_t)(rxq.last_used + nbufs);
        if (drop || flen == 0) {
            continue;
        }
        uint8_t flags = hdr[HDR_FLAGS];
        if (flags & HDR_F_NEEDS_CSUM) {
            if (!rx_finish_csum(rxq_data[slot], flen, le16_at(hdr + HDR_CSUM_START),
                                le16_at(hdr + HDR_CSUM_OFFSET))) {
                continue;
            }
        }
        rxq_len[slot] = (uint16_t)flen;
        rxq_csum_ok[slot] = (flags & HDR_F_DATA_VALID) ? 1 : 0;
        rxq_tail = nxt;
    }
    if (n) {
        tp.notify(&tp, RX_Q);
    }
    return n;
}

/* ---- MSI-X handlers (#253): one per vector, each ends with the APIC EOI.
 * The ISR status register is not involved in MSI-X mode (§4.1.4.5). ---- */

static void msix_config_handler(cpu_state_t *state) {
    (void)state; /* link status only; nothing acts on it yet */
    pci_msix_eoi();
}

static void msix_rx_handler(cpu_state_t *state) {
    (void)state;
    if (!msix_rx_seen) {
        msix_rx_seen = 1;
        boot_log("NET: virtio-net first MSI-X rx interrupt");
    }
    rx_drain();
    pci_msix_eoi();
}

static void msix_tx_handler(cpu_state_t *state) {
    (void)state; /* TX is polled; the avail flag asks for no interrupt */
    pci_msix_eoi();
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
    /* Offloads (#253): CSUM and GUEST_CSUM travel together under one switch,
     * since the stack's partial-sum rule is one decision, not two. */
    int csum_offered = (offered & (1ULL << F_NET_CSUM)) != 0;
    int mrg_offered = (offered & (1ULL << F_NET_MRG_RXBUF)) != 0;
    feat_csum = csum_offered && !(nofeat_mask & NOFEAT_CSUM);
    feat_mrgrx = mrg_offered && !(nofeat_mask & NOFEAT_MRGRX);
    if (feat_csum) {
        want |= (1ULL << F_NET_CSUM) | (offered & (1ULL << F_NET_GUEST_CSUM));
    }
    if (feat_mrgrx) {
        want |= (1ULL << F_NET_MRG_RXBUF);
    }
    tp.set_features(&tp, want);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK);
    if (!(tp.get_status(&tp) & VIRTIO_ST_FEATURES_OK)) {
        boot_log("NET: virtio-net refused our feature set");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    rx_chunk = feat_mrgrx ? RX_CHUNK_MRG : RX_CHUNK_SINGLE;

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

    /* MSI-X before the queues, so queue_setup can stamp each vector. The
     * transport says whether it has any (virtio-mmio never does). */
    int msix_offered = tp.msix_enable != 0;
    feat_msix = 0;
    if (msix_offered && !(nofeat_mask & NOFEAT_MSIX)) {
        feat_msix = tp.msix_enable(&tp, NUM_QUEUES);
        if (!feat_msix) {
            msix_offered = 0; /* the function has no usable capability */
        }
    }

    if (!vq_setup(RX_Q, &rxq) || !vq_setup(TX_Q, &txq)) {
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    /* TX completion is polled: ask the device not to interrupt for it. */
    txq.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    for (int i = 0; i < QSZ; i++) {
        rx_buf[i] = (uint8_t *)kmalloc(rx_chunk);
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

    if (feat_msix) {
        /* Vector 0 config, 1 + RX_Q, 1 + TX_Q — the transport's numbering. */
        interrupt_register((uint8_t)(MSIX_VECTOR_BASE + 0), msix_config_handler, 0);
        interrupt_register((uint8_t)(MSIX_VECTOR_BASE + 1 + RX_Q), msix_rx_handler, 0);
        interrupt_register((uint8_t)(MSIX_VECTOR_BASE + 1 + TX_Q), msix_tx_handler, 0);
    }

    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK |
                           VIRTIO_ST_DRIVER_OK);
    tp.notify(&tp, RX_Q);

    /* The stack's half of TX offload: partial sums from now on (#253). */
    netdev_set_tx_csum_offload(feat_csum);

    if (want & (1ULL << F_NET_STATUS)) {
        uint16_t st = (uint16_t)(tp.config_read8(&tp, NET_CFG_STATUS) |
                                 ((uint16_t)tp.config_read8(&tp, NET_CFG_STATUS + 1) << 8));
        if (!(st & NET_S_LINK_UP)) {
            boot_log("NET: virtio-net link is down at init (continuing)");
        }
    }

    /* One line per feature, the words the gates grep for (#253). */
    boot_log(feat_csum ? "NET: virtio-net feature csum: negotiated"
                       : (csum_offered ? "NET: virtio-net feature csum: off-by-cmdline"
                                       : "NET: virtio-net feature csum: not-offered"));
    boot_log(feat_mrgrx ? "NET: virtio-net feature mrgrx: negotiated"
                        : (mrg_offered ? "NET: virtio-net feature mrgrx: off-by-cmdline"
                                       : "NET: virtio-net feature mrgrx: not-offered"));
    boot_log(feat_msix ? "NET: virtio-net feature msix: negotiated"
                       : (msix_offered ? "NET: virtio-net feature msix: off-by-cmdline"
                                       : "NET: virtio-net feature msix: not-offered"));
    boot_log(feat_msix ? "NET: virtio-net interrupts: MSI-X (config, rx, tx vectors)"
                       : "NET: virtio-net interrupts: INTx");
    return 1;
}

/* Does the n-byte word at p spell `word` exactly? (No strncmp in-kernel.) */
static int word_is(const char *p, int n, const char *word) {
    int i = 0;
    while (i < n && word[i] && p[i] == word[i]) {
        i++;
    }
    return i == n && word[i] == ' ';
}

void virtio_net_set_nofeat(const char *list) {
    /* Comma-separated words; unknown ones are logged and ignored, so a typo
     * never silently disables something else. */
    const char *p = list;
    while (p && *p) {
        const char *q = p;
        while (*q && *q != ',') {
            q++;
        }
        int n = (int)(q - p);
        if (word_is(p, n, "csum")) {
            nofeat_mask |= NOFEAT_CSUM;
        } else if (word_is(p, n, "mrgrx")) {
            nofeat_mask |= NOFEAT_MRGRX;
        } else if (word_is(p, n, "msix")) {
            nofeat_mask |= NOFEAT_MSIX;
        } else if (n > 0) {
            boot_log("NET: virtio_net.nofeat: unknown feature word ignored");
        }
        p = *q ? q + 1 : q;
    }
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

/* Fill the TX header. Zero unless CSUM is negotiated and the frame is an
 * IPv4/TCP segment: then the stack left the pseudo-header sum in the TCP
 * checksum field (netdev_tx_csum_offload) and the device finishes it from
 * csum_start. Only TCP: it is the one protocol the stack checksums. */
static void tx_fill_hdr(uint8_t *hdr, const uint8_t *frame, uint16_t len) {
    for (uint32_t i = 0; i < NET_HDR_LEN; i++) {
        hdr[i] = 0;
    }
    if (!feat_csum || len < ETH_HLEN + 20) {
        return;
    }
    if (frame_type(frame) != ETH_TYPE_IPV4) {
        return;
    }
    uint16_t ihl = (uint16_t)((frame[ETH_HLEN] & 0x0F) * 4);
    if (ihl < 20 || (uint32_t)ETH_HLEN + ihl + 20 > len || frame[ETH_HLEN + 9] != IP_PROTO_TCP) {
        return;
    }
    hdr[HDR_FLAGS] = HDR_F_NEEDS_CSUM;
    le16_put(hdr + HDR_CSUM_START, (uint16_t)(ETH_HLEN + ihl));
    le16_put(hdr + HDR_CSUM_OFFSET, TCP_CSUM_OFFSET);
}

int virtio_net_transmit(const uint8_t *frame, uint16_t len) {
    if (!nic_present_flag || len == 0 || len > NET_FRAME_MAX) {
        return -1;
    }
    /* One descriptor, header + frame, device-readable. One outstanding
     * transmit at a time, polled — the rtl8139 discipline. */
    tx_fill_hdr(tx_buf, frame, len);
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
    /* The INTx path. With MSI-X live the ISR register stays clear and the
     * line is disabled at the function, so this returns at once. */
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
    if (rxq_csum_ok[rxq_head]) {
        netdev_rx_csum_valid_set(1); /* the device verified this frame */
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
