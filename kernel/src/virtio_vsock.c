/**
 * QuantumOS virtio-vsock — the shell channel (ADR-0029, #259).
 *
 * Why: under nested Firecracker every byte written to the emulated 16550
 * costs ~0.9 ms on the host side (#259 measured `help` at ~1.4 s). vsock
 * moves a whole buffer per notification. Firecracker has no virtio-console;
 * vsock is its one paravirtual stream device.
 *
 * Scope (virtio 1.2 §5.10, the stream subset): one listener on
 * VSOCK_SHELL_PORT, at most one connection, credit-based flow control,
 * and the transport-reset event a snapshot restore delivers. No socket API
 * for ring 3, no guest-initiated connections.
 *
 * Shape: the virtio-blk pattern. Polled, avail rings ask for no interrupts,
 * bounded waits. RX is drained from the timer tick (a memory read when idle,
 * no VM exit) and from SYS_CONS_READ; TX is one request in flight, polled to
 * completion. Every entry point runs with interrupts off (the timer IRQ, or
 * console_write's irq_save), on one CPU, so there is no locking.
 *
 * The host writes every header this file parses. The host is the hypervisor
 * and already trusted with memory and COM1, but the parser still bounds `len`
 * against the posted buffer, checks `type` and `op`, and answers anything
 * not addressed to the one listener or connection with RST.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio_vsock.h>
#include <kernel/virtio.h>
#include <kernel/console.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

#define RX_Q 0
#define TX_Q 1
#define EV_Q 2

/* Feature bits (§5.10.3) */
#define F_VSOCK_STREAM 0

/* Device config (§5.10.4): guest_cid, u64 at 0 */
#define CFG_GUEST_CID 0

/* Header (§5.10.6) */
#define TYPE_STREAM 1
#define OP_REQUEST 1
#define OP_RESPONSE 2
#define OP_RST 3
#define OP_SHUTDOWN 4
#define OP_RW 5
#define OP_CREDIT_UPDATE 6
#define OP_CREDIT_REQUEST 7

#define EVENT_TRANSPORT_RESET 0
#define HOST_CID 2

#define QSZ 8
#define RX_BUFS 4 /* two descriptors each: header, payload */
#define RX_PAYLOAD 4096
#define TX_PAYLOAD 4096
#define EV_BUFS 2

/* What we advertise: the console ring takes received bytes at once, so the
 * buffer is notional; fwd_cnt advances as bytes are handed to the console. */
#define OUR_BUF_ALLOC 65536u

#define VSOCK_SPIN_MAX 20000000u    /* one TX completion */
#define CREDIT_WAIT_PASSES 2000000u /* waiting for the peer to grant credit */

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
    uint64_t src_cid;
    uint64_t dst_cid;
    uint32_t src_port;
    uint32_t dst_port;
    uint32_t len;
    uint16_t type;
    uint16_t op;
    uint32_t flags;
    uint32_t buf_alloc;
    uint32_t fwd_cnt;
} vsock_hdr_t;

#define HDR_LEN ((uint32_t)sizeof(vsock_hdr_t)) /* 44 */

typedef struct {
    volatile vring_desc_t *desc;
    volatile vring_avail_t *avail;
    volatile vring_used_t *used;
    uint16_t last_used;
} vq_t;

static virtio_transport_t tp;
static int present;
static uint64_t guest_cid;

static vq_t rxq, txq, evq;
static volatile vsock_hdr_t *rx_hdr[RX_BUFS];
static volatile uint8_t *rx_data[RX_BUFS];
static volatile uint32_t *ev_buf[EV_BUFS];
static volatile vsock_hdr_t *tx_hdr;
static volatile uint8_t *tx_data;

/* The one connection. */
static int connected;
static uint32_t peer_port;
static uint32_t peer_buf_alloc;
static uint32_t peer_fwd_cnt;
static uint32_t tx_cnt;       /* payload bytes we have sent */
static uint32_t fwd_cnt;      /* payload bytes we have consumed */
static uint32_t fwd_cnt_told; /* fwd_cnt last reported to the peer */

static inline void vbarrier(void) {
    __asm__ volatile("" ::: "memory");
}

static void vzero(volatile void *p, uint32_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = 0;
    }
}

static void *alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t *raw = (uint8_t *)kmalloc(size + align);
    if (!raw) {
        return 0;
    }
    uintptr_t a = (uintptr_t)raw;
    return (void *)((a + (align - 1)) & ~(uintptr_t)(align - 1));
}

static int vq_alloc(vq_t *q, uint16_t index) {
    q->desc = (volatile vring_desc_t *)alloc_aligned((uint32_t)sizeof(vring_desc_t) * QSZ, 16);
    q->avail = (volatile vring_avail_t *)alloc_aligned((uint32_t)sizeof(vring_avail_t), 16);
    q->used = (volatile vring_used_t *)alloc_aligned((uint32_t)sizeof(vring_used_t), 16);
    if (!q->desc || !q->avail || !q->used) {
        return 0;
    }
    vzero(q->desc, (uint32_t)sizeof(vring_desc_t) * QSZ);
    vzero(q->avail, (uint32_t)sizeof(vring_avail_t));
    vzero(q->used, (uint32_t)sizeof(vring_used_t));
    q->avail->flags = VRING_AVAIL_F_NO_INTERRUPT; /* polled */
    q->last_used = 0;
    vbarrier();
    return tp.queue_setup(&tp, index, QSZ, (uint64_t)(uintptr_t)q->desc,
                          (uint64_t)(uintptr_t)q->avail, (uint64_t)(uintptr_t)q->used);
}

static void vq_offer(vq_t *q, uint16_t head) {
    q->avail->ring[q->avail->idx % QSZ] = head;
    vbarrier();
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    vbarrier();
}

/* RX buffer i uses descriptors 2i (header) and 2i+1 (payload). */
static void rx_post(uint32_t i) {
    uint16_t h = (uint16_t)(2 * i);
    rxq.desc[h].addr = (uint64_t)(uintptr_t)rx_hdr[i];
    rxq.desc[h].len = HDR_LEN;
    rxq.desc[h].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
    rxq.desc[h].next = (uint16_t)(h + 1);
    rxq.desc[h + 1].addr = (uint64_t)(uintptr_t)rx_data[i];
    rxq.desc[h + 1].len = RX_PAYLOAD;
    rxq.desc[h + 1].flags = VRING_DESC_F_WRITE;
    rxq.desc[h + 1].next = 0;
    vq_offer(&rxq, h);
}

static void ev_post(uint32_t i) {
    evq.desc[i].addr = (uint64_t)(uintptr_t)ev_buf[i];
    evq.desc[i].len = 4;
    evq.desc[i].flags = VRING_DESC_F_WRITE;
    evq.desc[i].next = 0;
    vq_offer(&evq, (uint16_t)i);
}

static uint64_t read_guest_cid(void) {
    uint64_t cid = 0;
    for (int i = 7; i >= 0; i--) {
        cid = (cid << 8) | tp.config_read8(&tp, (uint32_t)(CFG_GUEST_CID + i));
    }
    return cid;
}

/* Send one packet: a header and `len` payload bytes (0 for control ops).
 * One request in flight, polled to completion. Returns 0, or -1 when the
 * device never completed it (the device is then dropped). */
static int tx_packet(uint16_t op, uint32_t src_port, uint32_t dst_port, const uint8_t *data,
                     uint32_t len) {
    tx_hdr->src_cid = guest_cid;
    tx_hdr->dst_cid = HOST_CID;
    tx_hdr->src_port = src_port;
    tx_hdr->dst_port = dst_port;
    tx_hdr->len = len;
    tx_hdr->type = TYPE_STREAM;
    tx_hdr->op = op;
    tx_hdr->flags = 0;
    tx_hdr->buf_alloc = OUR_BUF_ALLOC;
    tx_hdr->fwd_cnt = fwd_cnt;
    fwd_cnt_told = fwd_cnt;

    txq.desc[0].addr = (uint64_t)(uintptr_t)tx_hdr;
    txq.desc[0].len = HDR_LEN;
    txq.desc[0].flags = len ? VRING_DESC_F_NEXT : 0;
    txq.desc[0].next = 1;
    if (len) {
        for (uint32_t i = 0; i < len; i++) {
            tx_data[i] = data[i];
        }
        txq.desc[1].addr = (uint64_t)(uintptr_t)tx_data;
        txq.desc[1].len = len;
        txq.desc[1].flags = 0;
        txq.desc[1].next = 0;
    }
    vq_offer(&txq, 0);
    tp.notify(&tp, TX_Q);

    for (uint32_t s = 0; s < VSOCK_SPIN_MAX; s++) {
        if (txq.used->idx != txq.last_used) {
            txq.last_used = txq.used->idx;
            (void)tp.isr_ack(&tp);
            return 0;
        }
        __asm__ volatile("pause");
    }
    (void)tp.isr_ack(&tp);
    boot_log("VSOCK: transmit timed out - shell channel dropped, COM1 resumes");
    present = 0;
    connected = 0;
    return -1;
}

static void disconnect(void) {
    connected = 0;
    peer_port = 0;
}

static void note_credit(volatile vsock_hdr_t *h) {
    peer_buf_alloc = h->buf_alloc;
    peer_fwd_cnt = h->fwd_cnt;
}

/* Handle one received packet. Returns 1 when it carried console input. */
static int rx_handle(volatile vsock_hdr_t *h, volatile uint8_t *data, uint32_t written) {
    if (written < HDR_LEN) {
        return 0; /* short write: nothing parseable */
    }
    uint32_t len = h->len;
    if (len > RX_PAYLOAD || len > written - HDR_LEN) {
        len = 0; /* never trust a length beyond what the device wrote */
    }
    uint32_t sport = h->src_port;
    int ours = connected && sport == peer_port && h->src_cid == HOST_CID;

    if (h->type != TYPE_STREAM || h->dst_port != VSOCK_SHELL_PORT) {
        if (h->op != OP_RST) {
            (void)tx_packet(OP_RST, h->dst_port, sport, 0, 0);
        }
        return 0;
    }
    switch (h->op) {
    case OP_REQUEST:
        if (connected) {
            (void)tx_packet(OP_RST, h->dst_port, sport, 0, 0); /* one shell at a time */
            return 0;
        }
        peer_port = sport;
        tx_cnt = 0;
        fwd_cnt = 0;
        note_credit(h);
        if (tx_packet(OP_RESPONSE, VSOCK_SHELL_PORT, sport, 0, 0) == 0) {
            connected = 1;
        }
        return 0;
    case OP_RW:
        if (!ours) {
            (void)tx_packet(OP_RST, h->dst_port, sport, 0, 0);
            return 0;
        }
        note_credit(h);
        console_input_bytes((const uint8_t *)data, len);
        fwd_cnt += len;
        if (fwd_cnt - fwd_cnt_told > OUR_BUF_ALLOC / 2) {
            (void)tx_packet(OP_CREDIT_UPDATE, VSOCK_SHELL_PORT, peer_port, 0, 0);
        }
        return len != 0;
    case OP_CREDIT_UPDATE:
        if (ours) {
            note_credit(h);
        }
        return 0;
    case OP_CREDIT_REQUEST:
        if (ours) {
            note_credit(h);
            (void)tx_packet(OP_CREDIT_UPDATE, VSOCK_SHELL_PORT, peer_port, 0, 0);
        }
        return 0;
    case OP_SHUTDOWN:
        if (ours) {
            (void)tx_packet(OP_RST, VSOCK_SHELL_PORT, peer_port, 0, 0);
            disconnect();
        }
        return 0;
    case OP_RST:
        if (ours) {
            disconnect();
        }
        return 0;
    default:
        if (h->op != OP_RESPONSE) { /* we never connect out, so no RESPONSE is ours */
            (void)tx_packet(OP_RST, h->dst_port, sport, 0, 0);
        }
        return 0;
    }
}

static int bring_up(void) {
    tp.set_status(&tp, 0);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE);
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER);

    uint64_t offered = tp.get_features(&tp);
    if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) {
        boot_log("VSOCK: virtio-vsock is not VERSION_1 - unsupported");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    tp.set_features(&tp, (1ULL << VIRTIO_F_VERSION_1) | (offered & (1ULL << F_VSOCK_STREAM)));
    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK);
    if (!(tp.get_status(&tp) & VIRTIO_ST_FEATURES_OK)) {
        boot_log("VSOCK: virtio-vsock refused our feature set");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    guest_cid = read_guest_cid();

    for (uint32_t i = 0; i < RX_BUFS; i++) {
        rx_hdr[i] = (volatile vsock_hdr_t *)alloc_aligned(HDR_LEN, 16);
        rx_data[i] = (volatile uint8_t *)kmalloc(RX_PAYLOAD);
        if (!rx_hdr[i] || !rx_data[i]) {
            goto nomem;
        }
    }
    for (uint32_t i = 0; i < EV_BUFS; i++) {
        ev_buf[i] = (volatile uint32_t *)alloc_aligned(4, 16);
        if (!ev_buf[i]) {
            goto nomem;
        }
    }
    tx_hdr = (volatile vsock_hdr_t *)alloc_aligned(HDR_LEN, 16);
    tx_data = (volatile uint8_t *)kmalloc(TX_PAYLOAD);
    if (!tx_hdr || !tx_data) {
        goto nomem;
    }
    if (!vq_alloc(&rxq, RX_Q) || !vq_alloc(&txq, TX_Q) || !vq_alloc(&evq, EV_Q)) {
        boot_log("VSOCK: virtio queue setup refused");
        tp.set_status(&tp, VIRTIO_ST_FAILED);
        return 0;
    }
    for (uint32_t i = 0; i < RX_BUFS; i++) {
        rx_post(i);
    }
    for (uint32_t i = 0; i < EV_BUFS; i++) {
        ev_post(i);
    }

    tp.set_status(&tp, VIRTIO_ST_ACKNOWLEDGE | VIRTIO_ST_DRIVER | VIRTIO_ST_FEATURES_OK |
                           VIRTIO_ST_DRIVER_OK);
    tp.notify(&tp, RX_Q);
    tp.notify(&tp, EV_Q);
    return 1;

nomem:
    boot_log("VSOCK: virtio-vsock buffer alloc failed");
    tp.set_status(&tp, VIRTIO_ST_FAILED);
    return 0;
}

int virtio_vsock_init(void) {
    if (present) {
        return 1;
    }
    if (!virtio_mmio_probe(VIRTIO_ID_VSOCK, &tp) && !virtio_pci_probe(VIRTIO_ID_VSOCK, &tp)) {
        return 0;
    }
    if (!bring_up()) {
        return 0;
    }
    present = 1;
    boot_log(tp.name[0] == 'p' ? "VSOCK: virtio-vsock up (pci), shell listener on port 1024"
                               : "VSOCK: virtio-vsock up (mmio), shell listener on port 1024");
    boot_log("VSOCK: guest cid:");
    early_console_write_hex(guest_cid);
    return 1;
}

int virtio_vsock_connected(void) {
    return present && connected;
}

int virtio_vsock_poll(void) {
    if (!present) {
        return 0;
    }
    int input = 0;

    /* Events first: a transport reset (snapshot restore) ends the connection
     * and may change our cid; the listener stays armed. */
    while (evq.used->idx != evq.last_used) {
        vbarrier();
        uint16_t slot = evq.last_used % QSZ;
        uint32_t id = evq.used->ring[slot].id;
        evq.last_used = (uint16_t)(evq.last_used + 1);
        if (id < EV_BUFS) {
            if (*ev_buf[id] == EVENT_TRANSPORT_RESET) {
                disconnect();
                guest_cid = read_guest_cid();
            }
            ev_post(id);
        }
        tp.notify(&tp, EV_Q);
    }

    while (present && rxq.used->idx != rxq.last_used) {
        vbarrier();
        uint16_t slot = rxq.last_used % QSZ;
        uint32_t head = rxq.used->ring[slot].id;
        uint32_t written = rxq.used->ring[slot].len;
        rxq.last_used = (uint16_t)(rxq.last_used + 1);
        if (head % 2 != 0 || head / 2 >= RX_BUFS) {
            continue; /* not a chain we posted */
        }
        uint32_t i = head / 2;
        input |= rx_handle(rx_hdr[i], rx_data[i], written);
        rx_post(i);
        tp.notify(&tp, RX_Q);
    }
    (void)tp.isr_ack(&tp);
    return input;
}

static uint32_t peer_credit(void) {
    uint32_t in_flight = tx_cnt - peer_fwd_cnt;
    return peer_buf_alloc > in_flight ? peer_buf_alloc - in_flight : 0;
}

uint32_t virtio_vsock_send(const uint8_t *buf, uint32_t len) {
    uint32_t sent = 0;
    while (sent < len && virtio_vsock_connected()) {
        uint32_t credit = peer_credit();
        if (credit == 0) {
            /* Ask, then wait (bounded) for the peer to report progress. */
            if (tx_packet(OP_CREDIT_REQUEST, VSOCK_SHELL_PORT, peer_port, 0, 0) != 0) {
                break;
            }
            for (uint32_t s = 0; s < CREDIT_WAIT_PASSES && peer_credit() == 0 && connected; s++) {
                (void)virtio_vsock_poll();
                __asm__ volatile("pause");
            }
            if (peer_credit() == 0) {
                break;
            }
            continue;
        }
        uint32_t n = len - sent;
        if (n > TX_PAYLOAD) {
            n = TX_PAYLOAD;
        }
        if (n > credit) {
            n = credit;
        }
        if (tx_packet(OP_RW, VSOCK_SHELL_PORT, peer_port, buf + sent, n) != 0) {
            break;
        }
        tx_cnt += n;
        sent += n;
    }
    return sent;
}
