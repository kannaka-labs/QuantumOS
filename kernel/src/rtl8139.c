/**
 * QuantumOS RTL8139 NIC driver implementation (epic #73 phase 1).
 *
 * The RTL8139 receive path is a single linear ring buffer (not a
 * descriptor ring): the chip DMAs each incoming frame, prefixed by a
 * 4-byte {status, length} header, into the buffer and advances CBR; the
 * driver consumes from CAPR. With the WRAP bit set the chip may write up
 * to 1500 bytes PAST the nominal end rather than splitting a frame, so
 * the buffer is oversized accordingly and no manual wrap-stitching is
 * needed. Transmit uses the 4 legacy TX descriptors round-robin.
 *
 * All hardware waits are bounded (the console_write / ATA lesson).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/rtl8139.h>
#include <kernel/pci.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* Register offsets from the I/O base */
#define REG_MAC0 0x00    /* 6 bytes */
#define REG_TSD0 0x10    /* TX status of descriptor 0 (4 regs, stride 4) */
#define REG_TSAD0 0x20   /* TX start address of descriptor 0 (stride 4) */
#define REG_RBSTART 0x30 /* RX buffer physical start */
#define REG_CR 0x37      /* Command register */
#define REG_CAPR 0x38    /* Current address of packet read */
#define REG_CBR 0x3A     /* Current buffer address (chip write ptr) */
#define REG_IMR 0x3C     /* Interrupt mask */
#define REG_ISR 0x3E     /* Interrupt status */
#define REG_TCR 0x40     /* TX configuration */
#define REG_RCR 0x44     /* RX configuration */
#define REG_CONFIG1 0x52 /* Config register 1 (power) */

/* Command register bits */
#define CR_BUFE 0x01 /* RX buffer empty */
#define CR_TE 0x04   /* transmitter enable */
#define CR_RE 0x08   /* receiver enable */
#define CR_RST 0x10  /* software reset */

/* Interrupt bits (ISR/IMR) */
#define INT_ROK 0x0001 /* receive OK */
#define INT_TOK 0x0004 /* transmit OK */

/* RX config: accept broadcast + multicast + physical-match + all(promisc
 * off), WRAP bit, unlimited RX DMA burst, max RX FIFO threshold. */
#define RCR_AAP 0x01             /* accept all (promiscuous) */
#define RCR_APM 0x02             /* accept physical match */
#define RCR_AM 0x04              /* accept multicast */
#define RCR_AB 0x08              /* accept broadcast */
#define RCR_WRAP 0x80            /* WRAP: don't split a frame at the buffer end */
#define RCR_RBLEN_8K (0 << 11)   /* 8192 + 16 byte RX buffer */
#define RCR_MXDMA_UNL (7 << 8)   /* unlimited RX DMA burst */
#define RCR_RXFTH_NONE (7 << 13) /* no RX FIFO threshold (whole frame) */

/* TX config: default DMA burst. */
#define TCR_MXDMA_2048 (7 << 8)

/* TX descriptor status bits */
#define TSD_OWN 0x2000 /* set by driver, cleared by chip when sent */
#define TSD_TOK 0x8000 /* transmit OK */

/* RX ring: 8192 + 16 (header slack) + 1500 (WRAP overflow room). Placed
 * in the low identity map so its kernel VA == its physical/DMA address. */
#define RX_RING_SIZE (8192 + 16 + 1600)

#define RTL_SPIN_MAX 1000000u

static uint16_t io_base;
static int nic_present;
static uint8_t nic_irq;
static uint8_t mac[ETH_ADDR_LEN];

static uint8_t *rx_ring;   /* RX_RING_SIZE bytes, DMA target */
static uint32_t rx_offset; /* our read cursor into the ring */
static uint8_t *tx_buf[4]; /* one bounce buffer per TX descriptor */
static int tx_cur;         /* next descriptor to use */

/* ---- port I/O ---- */
static inline void outb_(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p));
}
static inline void outw_(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p));
}
static inline void outl_(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p));
}
static inline uint8_t inb_(uint16_t p) {
    uint8_t r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}
static inline uint16_t inw_(uint16_t p) {
    uint16_t r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}
static inline uint32_t inl_(uint16_t p) {
    uint32_t r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(p));
    return r;
}

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory");
}

/* ---- received-frame queue (IRQ producer, net layer consumer) ---- */
#define RXQ_SLOTS 16
typedef struct {
    uint8_t data[RTL_FRAME_MAX];
    uint16_t len;
} rxq_entry_t;
static rxq_entry_t rxq[RXQ_SLOTS];
static volatile uint32_t rxq_head; /* producer (IRQ) */
static volatile uint32_t rxq_tail; /* consumer */
static volatile uint32_t rxq_dropped;

/* Push a frame into the queue (called from the IRQ handler, IF=0). */
static void rxq_push(const uint8_t *data, uint16_t len) {
    if (len == 0 || len > RTL_FRAME_MAX) {
        return;
    }
    uint32_t next = (rxq_head + 1) % RXQ_SLOTS;
    if (next == rxq_tail) {
        rxq_dropped++;
        return; /* full — drop newest */
    }
    for (uint16_t i = 0; i < len; i++) {
        rxq[rxq_head].data[i] = data[i];
    }
    rxq[rxq_head].len = len;
    rxq_head = next;
}

uint16_t rtl8139_receive(uint8_t *buf, uint16_t max) {
    uint64_t f = irq_save();
    uint16_t n = 0;
    if (rxq_tail != rxq_head) {
        rxq_entry_t *e = &rxq[rxq_tail];
        n = e->len < max ? e->len : max;
        for (uint16_t i = 0; i < n; i++) {
            buf[i] = e->data[i];
        }
        rxq_tail = (rxq_tail + 1) % RXQ_SLOTS;
    }
    irq_restore(f);
    return n;
}

/* ---- transmit ---- */
int rtl8139_transmit(const uint8_t *frame, uint16_t len) {
    if (!nic_present || !frame || len == 0 || len > RTL_FRAME_MAX) {
        return -1;
    }
    /* Pad runt frames to the 60-byte Ethernet minimum (chip appends CRC). */
    uint16_t txlen = len < 60 ? 60 : len;

    uint64_t f = irq_save();
    int d = tx_cur;
    tx_cur = (tx_cur + 1) & 3;
    irq_restore(f);

    uint8_t *buf = tx_buf[d];
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = frame[i];
    }
    for (uint16_t i = len; i < txlen; i++) {
        buf[i] = 0; /* zero the runt padding */
    }

    /* Program this descriptor: address then status (size + clear OWN by
     * writing len; the chip owns it until it clears OWN / sets TOK). */
    outl_(io_base + REG_TSAD0 + d * 4, (uint32_t)(uintptr_t)buf);
    /* Writing TSD with the size (low 13 bits) and a clear OWN bit starts
     * the send; OWN(bit13) is set to 0 by us and the chip sets TOK when
     * done. The early-TX threshold lives in bits 16-21; 0 is fine. */
    outl_(io_base + REG_TSD0 + d * 4, (uint32_t)txlen);

    /* Wait (bounded) for the chip to finish this descriptor. */
    for (uint32_t s = 0; s < RTL_SPIN_MAX; s++) {
        uint32_t tsd = inl_(io_base + REG_TSD0 + d * 4);
        if (tsd & TSD_TOK) {
            return 0;
        }
    }
    return -1;
}

/* ---- receive (IRQ) ---- */
void rtl8139_irq(void) {
    if (!nic_present) {
        return;
    }
    uint16_t status = inw_(io_base + REG_ISR);
    /* Acknowledge by writing the bits back (write-1-to-clear). Do this
     * up front so a frame arriving during processing re-raises cleanly. */
    outw_(io_base + REG_ISR, status);

    if (!(status & INT_ROK)) {
        return; /* only RX is interesting here (TX is polled) */
    }

    /* Drain every complete frame the chip has delivered. CR.BUFE set
     * means the ring is empty. `rx_offset` is a FREE-RUNNING cursor (like
     * Linux's 8139too cur_rx): the buffer is indexed by rx_offset % 8192,
     * and CAPR is written as the low 16 bits of (rx_offset - 16). The
     * chip tracks the low 16 bits of the ring position, and 8192 divides
     * 65536 evenly, so the u16 truncation stays aligned across wraps —
     * where a per-iteration modulo of rx_offset would drift. */
    int guard = 0;
    while (!(inb_(io_base + REG_CR) & CR_BUFE) && guard++ < RXQ_SLOTS * 2) {
        uint8_t *p = rx_ring + (rx_offset % 8192);
        uint16_t rxstatus = (uint16_t)(p[0] | (p[1] << 8));
        uint16_t rxlen = (uint16_t)(p[2] | (p[3] << 8)); /* includes 4-byte CRC */

        /* A good frame has the ROK bit (bit0) and a sane length. */
        if ((rxstatus & 0x1) && rxlen >= 4 && rxlen <= RTL_FRAME_MAX + 4) {
            uint16_t framelen = rxlen - 4; /* strip the trailing CRC */
            rxq_push(p + 4, framelen);
        }

        /* Advance past the 4-byte header + frame, rounded up to a 4-byte
         * boundary; CAPR trails the cursor by 16 (the chip's quirk). */
        rx_offset = (rx_offset + rxlen + 4 + 3) & ~3u;
        outw_(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));
    }
}

uint8_t rtl8139_irq_line(void) {
    return nic_irq;
}

int rtl8139_present(void) {
    return nic_present;
}

void rtl8139_get_mac(uint8_t out[ETH_ADDR_LEN]) {
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out[i] = mac[i];
    }
}

int rtl8139_init(void) {
    nic_present = 0;

    pci_device_t dev = pci_find(0x10EC, 0x8139);
    if (!dev.found) {
        boot_log("NET: no rtl8139 NIC found — networking disabled");
        return 0;
    }
    pci_enable_bus_master_io(&dev);
    io_base = dev.io_base;
    nic_irq = dev.irq_line;

    /* The IRQ line must be a real 8259 line (0-15). PCI reports 0xFF for
     * "not connected"; without a usable line the RX interrupt can never
     * fire, so degrade honestly rather than unmask a bogus vector. */
    if (nic_irq >= 16) {
        boot_log("NET: rtl8139 has no usable IRQ line — networking disabled");
        return 0;
    }

    /* Allocate DMA buffers in the low identity map (VA == phys). */
    rx_ring = kmalloc(RX_RING_SIZE);
    if (!rx_ring) {
        boot_log("NET: rtl8139 RX buffer alloc failed — networking disabled");
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        tx_buf[i] = kmalloc(RTL_FRAME_MAX);
        if (!tx_buf[i]) {
            boot_log("NET: rtl8139 TX buffer alloc failed — networking disabled");
            return 0;
        }
    }

    /* Power on, then soft reset and wait (bounded) for it to clear. */
    outb_(io_base + REG_CONFIG1, 0x00);
    outb_(io_base + REG_CR, CR_RST);
    int ok = 0;
    for (uint32_t s = 0; s < RTL_SPIN_MAX; s++) {
        if (!(inb_(io_base + REG_CR) & CR_RST)) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        boot_log("NET: rtl8139 reset timeout — networking disabled");
        return 0;
    }

    /* Read the MAC. */
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        mac[i] = inb_(io_base + REG_MAC0 + i);
    }

    /* Program the RX buffer and cursor. */
    rx_offset = 0;
    outl_(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_ring);
    outw_(io_base + REG_CAPR, (uint16_t)(0 - 16)); /* 0xFFF0 */

    /* Enable ROK + TOK interrupts. */
    outw_(io_base + REG_IMR, INT_ROK | INT_TOK);

    /* RX config: broadcast + physical-match + multicast, WRAP, 8K buffer,
     * no FIFO threshold, unlimited DMA burst. */
    outl_(io_base + REG_RCR,
          RCR_AB | RCR_APM | RCR_AM | RCR_WRAP | RCR_RBLEN_8K | RCR_MXDMA_UNL | RCR_RXFTH_NONE);

    /* TX config. */
    outl_(io_base + REG_TCR, TCR_MXDMA_2048);

    /* Enable receiver + transmitter. */
    outb_(io_base + REG_CR, CR_RE | CR_TE);

    tx_cur = 0;
    nic_present = 1;

    boot_log("NET: rtl8139 up — MAC:");
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        early_console_write_hex(mac[i]);
    }
    boot_log("NET: rtl8139 IRQ line:");
    early_console_write_hex(nic_irq);
    return 1;
}

/* ---- netdev binding -------------------------------------------------
 * Every entry point above already matches the netdev contract, so this is
 * the whole port: no behaviour changes, only late binding. */
const netdev_ops_t rtl8139_netdev = {
    .name = "rtl8139",
    .init = rtl8139_init,
    .present = rtl8139_present,
    .get_mac = rtl8139_get_mac,
    .transmit = rtl8139_transmit,
    .irq = rtl8139_irq,
    .irq_line = rtl8139_irq_line,
    .receive = rtl8139_receive,
};
