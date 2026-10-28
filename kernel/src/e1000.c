/**
 * QuantumOS Intel e1000 (82540EM) NIC driver implementation (#252).
 *
 * The 8254x is a descriptor-ring NIC: software hands the chip a ring of
 * 16-byte legacy descriptors each pointing at a buffer, and head/tail
 * registers (RDH/RDT, TDH/TDT) say who owns which. The chip advances the
 * head as it fills (RX) or sends (TX); software advances the tail as it
 * hands descriptors back (RX) or queues frames (TX). A descriptor's DD
 * (descriptor done) status bit is the chip's completion mark.
 *
 * RX: 32 descriptors x 2048-byte buffers. The IRQ walks the ring from our
 * cursor while DD is set, copies each complete frame into the software
 * queue, clears the status and moves RDT to the slot just consumed.
 * TX: 8 descriptors, one frame in flight at a time, polled to completion
 * (the rtl8139 discipline). The chip appends the CRC (IFCS); we pad runts.
 *
 * Registers are MMIO behind BAR0, 128 KB, which PCI firmware places above
 * the boot identity map; mmio_identity_map() (#246) maps the window first.
 * Descriptor rings and buffers come from kmalloc, whose kernel VA is its
 * own physical address (the assumption rtl8139.c documents).
 *
 * All hardware waits are bounded. Reading ICR clears it, so the IRQ reads
 * it exactly once.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/e1000.h>
#include <kernel/pci.h>
#include <kernel/mmio.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

#define E1000_VENDOR 0x8086
#define E1000_DEV_82540EM 0x100E
#define E1000_BAR0_LEN 0x20000 /* 128 KB register window */

/* Register offsets (8254x SDM §13) */
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_ICR 0x00C0
#define REG_IMS 0x00D0
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200 /* 128 dwords: the multicast table array */
#define REG_RAL 0x5400 /* receive address 0, low 32 bits */
#define REG_RAH 0x5404 /* receive address 0, high 16 bits + AV */

/* CTRL bits */
#define CTRL_FD (1u << 0)       /* full duplex */
#define CTRL_LRST (1u << 3)     /* link reset */
#define CTRL_ASDE (1u << 5)     /* auto speed detection enable */
#define CTRL_SLU (1u << 6)      /* set link up */
#define CTRL_ILOS (1u << 7)     /* invert loss-of-signal */
#define CTRL_RST (1u << 26)     /* device reset (self-clearing) */
#define CTRL_PHY_RST (1u << 31) /* PHY reset */

/* EERD (EEPROM read, 82540 layout): START bit0, DONE bit4, ADDR 8-15,
 * DATA 16-31. */
#define EERD_START (1u << 0)
#define EERD_DONE (1u << 4)
#define EERD_ADDR_SHIFT 8
#define EERD_DATA_SHIFT 16

/* Interrupt bits (ICR/IMS/IMC) */
#define INT_TXDW (1u << 0)   /* transmit descriptor written back */
#define INT_LSC (1u << 2)    /* link status change */
#define INT_RXSEQ (1u << 3)  /* receive sequence error */
#define INT_RXDMT0 (1u << 4) /* RX descriptors at minimum threshold */
#define INT_RXO (1u << 6)    /* receiver overrun */
#define INT_RXT0 (1u << 7)   /* receiver timer: a frame landed */
#define INT_RX_ANY (INT_RXT0 | INT_RXDMT0 | INT_RXO | INT_RXSEQ)

/* RCTL bits */
#define RCTL_EN (1u << 1)          /* receiver enable */
#define RCTL_BAM (1u << 15)        /* accept broadcast */
#define RCTL_BSIZE_2048 (0u << 16) /* buffer size 2048 (BSEX clear) */
#define RCTL_SECRC (1u << 26)      /* strip the Ethernet CRC */

/* TCTL bits */
#define TCTL_EN (1u << 1)  /* transmitter enable */
#define TCTL_PSP (1u << 3) /* pad short packets */
#define TCTL_CT_SHIFT 4    /* collision threshold */
#define TCTL_COLD_SHIFT 12 /* collision distance */
#define TIPG_DEFAULT (10u | (8u << 10) | (6u << 20))

/* RAH */
#define RAH_AV (1u << 31) /* address valid */

/* Legacy descriptor status / command bits */
#define RX_ST_DD (1u << 0)  /* descriptor done */
#define RX_ST_EOP (1u << 1) /* end of packet */
#define TX_CMD_EOP (1u << 0)
#define TX_CMD_IFCS (1u << 1) /* insert the frame checksum */
#define TX_CMD_RS (1u << 3)   /* report status (set DD when done) */
#define TX_ST_DD (1u << 0)

/* Ring geometry. RDLEN/TDLEN must be a multiple of 128 bytes, i.e. the
 * descriptor count a multiple of 8. */
#define NRX 32
#define NTX 8
#define RX_BUF_SIZE 2048

#define E1000_SPIN_MAX 1000000u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} tx_desc_t;

_Static_assert(sizeof(rx_desc_t) == 16, "e1000 legacy RX descriptor is 16 bytes");
_Static_assert(sizeof(tx_desc_t) == 16, "e1000 legacy TX descriptor is 16 bytes");

static volatile uint8_t *regs; /* BAR0 */
static int nic_present;
static uint8_t nic_irq;
static uint8_t mac[ETH_ADDR_LEN];

static volatile rx_desc_t *rx_ring;
static volatile tx_desc_t *tx_ring;
static uint8_t *rx_buf[NRX];
static uint8_t *tx_buf[NTX];
static uint32_t rx_cur; /* next RX descriptor we expect the chip to fill */
static uint32_t tx_cur; /* next TX descriptor to use (== TDT) */

/* ---- register access ---- */
static inline uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(regs + off);
}
static inline void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(regs + off) = v;
}
static inline void barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" : : "r"(f) : "memory");
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

/* ---- received-frame queue (IRQ producer, net layer consumer) ---- */
#define RXQ_SLOTS 16
typedef struct {
    uint8_t data[NET_FRAME_MAX];
    uint16_t len;
} rxq_entry_t;
static rxq_entry_t rxq[RXQ_SLOTS];
static volatile uint32_t rxq_head; /* producer (IRQ) */
static volatile uint32_t rxq_tail; /* consumer */
static volatile uint32_t rxq_dropped;

/* Push a frame into the queue (interrupts are off: IRQ, or receive()'s
 * polling fallback). */
static void rxq_push(const uint8_t *data, uint16_t len) {
    if (len == 0 || len > NET_FRAME_MAX) {
        return;
    }
    uint32_t next = (rxq_head + 1) % RXQ_SLOTS;
    if (next == rxq_tail) {
        rxq_dropped++;
        return; /* full - drop newest, never overwrite */
    }
    for (uint16_t i = 0; i < len; i++) {
        rxq[rxq_head].data[i] = data[i];
    }
    rxq[rxq_head].len = len;
    rxq_head = next;
}

/* Walk the RX ring from our cursor while the chip has marked descriptors
 * done, queue each complete good frame, and hand the slot back by moving
 * RDT onto it. Bounded to one lap so a stuck DD bit cannot spin forever. */
static void rx_drain(void) {
    for (uint32_t guard = 0; guard < NRX; guard++) {
        volatile rx_desc_t *d = &rx_ring[rx_cur];
        uint8_t st = d->status;
        if (!(st & RX_ST_DD)) {
            break;
        }
        barrier();
        uint16_t len = d->length; /* CRC already stripped (SECRC) */
        /* A frame larger than one buffer arrives split across descriptors
         * (EOP clear on all but the last); the stack never wants one, so
         * every fragment is dropped. Error bits mean a bad frame. */
        if ((st & RX_ST_EOP) && d->errors == 0 && len <= NET_FRAME_MAX) {
            rxq_push(rx_buf[rx_cur], len);
        }
        d->status = 0;
        barrier();
        wr(REG_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

uint16_t e1000_receive(uint8_t *buf, uint16_t max) {
    if (!nic_present) {
        return 0;
    }
    uint64_t f = irq_save();
    if (rxq_tail == rxq_head) {
        /* Polling fallback (the virtio-net pattern): if the INTx line is
         * mis-routed the ring still fills, so drain it here with interrupts
         * off - the IRQ path cannot run rx_drain concurrently. */
        rx_drain();
    }
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
int e1000_transmit(const uint8_t *frame, uint16_t len) {
    if (!nic_present || !frame || len == 0 || len > NET_FRAME_MAX) {
        return -1;
    }
    /* Pad runt frames to the 60-byte Ethernet minimum (chip appends CRC). */
    uint16_t txlen = len < 60 ? 60 : len;

    uint64_t f = irq_save();
    uint32_t d = tx_cur;
    tx_cur = (tx_cur + 1) % NTX;
    irq_restore(f);

    uint8_t *buf = tx_buf[d];
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = frame[i];
    }
    for (uint16_t i = len; i < txlen; i++) {
        buf[i] = 0; /* zero the runt padding */
    }

    volatile tx_desc_t *td = &tx_ring[d];
    td->addr = (uint64_t)(uintptr_t)buf;
    td->length = txlen;
    td->cso = 0;
    td->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    td->status = 0;
    td->css = 0;
    td->special = 0;
    barrier();
    /* Moving the tail past the descriptor hands it to the chip. */
    wr(REG_TDT, (d + 1) % NTX);

    /* Wait (bounded) for the chip to write back DD. */
    for (uint32_t s = 0; s < E1000_SPIN_MAX; s++) {
        if (td->status & TX_ST_DD) {
            return 0;
        }
    }
    return -1;
}

/* ---- receive (IRQ) ---- */
void e1000_irq(void) {
    if (!nic_present) {
        return;
    }
    uint32_t icr = rd(REG_ICR); /* read-to-clear: this also lowers INTx */
    if (icr == 0) {
        return; /* a shared line, not us */
    }
    if (icr & INT_RX_ANY) {
        rx_drain();
    }
    /* TXDW/LSC need no action: TX is polled and the stack retries. */
}

uint8_t e1000_irq_line(void) {
    return nic_irq;
}

int e1000_present(void) {
    return nic_present;
}

void e1000_get_mac(uint8_t out[ETH_ADDR_LEN]) {
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out[i] = mac[i];
    }
}

/* ---- bring-up ---- */

/* Read one EEPROM word through EERD. Returns 1 and the word in *out, or 0
 * when the chip never raised DONE (no EEPROM, or a part with the other
 * EERD layout) - the caller falls back to RAL/RAH. */
static int eeprom_read(uint8_t addr, uint16_t *out) {
    wr(REG_EERD, ((uint32_t)addr << EERD_ADDR_SHIFT) | EERD_START);
    for (uint32_t s = 0; s < E1000_SPIN_MAX; s++) {
        uint32_t v = rd(REG_EERD);
        if (v & EERD_DONE) {
            *out = (uint16_t)(v >> EERD_DATA_SHIFT);
            return 1;
        }
    }
    return 0;
}

/* The MAC lives in EEPROM words 0-2, little-endian per word. */
static int read_mac(void) {
    int from_eeprom = 1;
    for (int w = 0; w < 3; w++) {
        uint16_t v;
        if (!eeprom_read((uint8_t)w, &v)) {
            from_eeprom = 0;
            break;
        }
        mac[2 * w] = (uint8_t)(v & 0xFF);
        mac[2 * w + 1] = (uint8_t)(v >> 8);
    }
    if (!from_eeprom) {
        uint32_t lo = rd(REG_RAL);
        uint32_t hi = rd(REG_RAH);
        mac[0] = (uint8_t)lo;
        mac[1] = (uint8_t)(lo >> 8);
        mac[2] = (uint8_t)(lo >> 16);
        mac[3] = (uint8_t)(lo >> 24);
        mac[4] = (uint8_t)hi;
        mac[5] = (uint8_t)(hi >> 8);
    }
    /* All-zero or broadcast is not an address: refuse rather than send
     * frames nothing will answer. */
    int zero = 1, ones = 1;
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        zero &= (mac[i] == 0x00);
        ones &= (mac[i] == 0xFF);
    }
    return !(zero || ones);
}

/* BAR0 is a memory BAR, 32- or 64-bit. 0 means unusable. */
static uint64_t bar0_base(const pci_device_t *dev) {
    uint32_t lo = dev->bar0;
    if (lo & 1) {
        return 0; /* an I/O BAR: not the 8254x register window */
    }
    uint64_t base = lo & ~0xFULL;
    if (((lo >> 1) & 3) == 2) {
        base |= (uint64_t)pci_config_read32(dev->bus, dev->slot, dev->func, 0x14) << 32;
    }
    return base;
}

int e1000_init(void) {
    nic_present = 0;

    pci_device_t dev = pci_find(E1000_VENDOR, E1000_DEV_82540EM);
    if (!dev.found) {
        return 0; /* silent: not this NIC; the rtl8139 probe follows */
    }
    nic_irq = dev.irq_line;
    if (nic_irq >= 16) {
        boot_log("NET: e1000 has no usable IRQ line - networking disabled");
        return 0;
    }

    uint64_t base = bar0_base(&dev);
    if (base == 0 || !mmio_identity_map(base, E1000_BAR0_LEN)) {
        boot_log("NET: e1000 BAR0 is not a mappable memory window - networking disabled");
        return 0;
    }
    regs = (volatile uint8_t *)(uintptr_t)base;
    pci_enable_bus_master_mem(&dev);

    /* Allocate the rings (16-byte aligned) and buffers in the low identity
     * map (VA == phys) before touching the chip, so a failed alloc leaves
     * the device untouched. */
    rx_ring = (volatile rx_desc_t *)alloc_aligned(NRX * (uint32_t)sizeof(rx_desc_t), 16);
    tx_ring = (volatile tx_desc_t *)alloc_aligned(NTX * (uint32_t)sizeof(tx_desc_t), 16);
    if (!rx_ring || !tx_ring) {
        boot_log("NET: e1000 ring alloc failed - networking disabled");
        return 0;
    }
    for (int i = 0; i < NRX; i++) {
        rx_buf[i] = (uint8_t *)alloc_aligned(RX_BUF_SIZE, 16);
        if (!rx_buf[i]) {
            boot_log("NET: e1000 RX buffer alloc failed - networking disabled");
            return 0;
        }
    }
    for (int i = 0; i < NTX; i++) {
        tx_buf[i] = (uint8_t *)alloc_aligned(NET_FRAME_MAX, 16);
        if (!tx_buf[i]) {
            boot_log("NET: e1000 TX buffer alloc failed - networking disabled");
            return 0;
        }
    }

    /* Mask every interrupt, reset, wait (bounded) for RST to self-clear,
     * mask again (reset restores the defaults) and discard anything pending. */
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    int ok = 0;
    for (uint32_t s = 0; s < E1000_SPIN_MAX; s++) {
        if (!(rd(REG_CTRL) & CTRL_RST)) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        boot_log("NET: e1000 reset timeout - networking disabled");
        return 0;
    }
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);

    /* Link up: SLU + auto speed detect, no resets, no LOS inversion. */
    uint32_t ctrl = rd(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS);
    wr(REG_CTRL, ctrl);

    if (!read_mac()) {
        boot_log("NET: e1000 has no valid MAC (EEPROM and RAL/RAH) - networking disabled");
        return 0;
    }
    /* Program receive address 0 with it and mark it valid; the chip only
     * accepts unicast frames matching a valid RA entry. */
    wr(REG_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
                    ((uint32_t)mac[3] << 24));
    wr(REG_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | RAH_AV);
    /* No multicast: clear the whole table. */
    for (uint32_t i = 0; i < 128; i++) {
        wr(REG_MTA + 4 * i, 0);
    }

    /* RX ring: every descriptor points at its buffer and is chip-owned.
     * RDH = 0, RDT = NRX-1: the chip may fill 0..NRX-2 and stops at RDT. */
    for (uint32_t i = 0; i < NRX; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_buf[i];
        rx_ring[i].length = 0;
        rx_ring[i].checksum = 0;
        rx_ring[i].status = 0;
        rx_ring[i].errors = 0;
        rx_ring[i].special = 0;
    }
    rx_cur = 0;
    barrier();
    uint64_t rx_phys = (uint64_t)(uintptr_t)rx_ring;
    wr(REG_RDBAL, (uint32_t)rx_phys);
    wr(REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(REG_RDLEN, NRX * (uint32_t)sizeof(rx_desc_t));
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);

    /* TX ring: empty (TDH == TDT == 0). */
    for (uint32_t i = 0; i < NTX; i++) {
        tx_ring[i].addr = 0;
        tx_ring[i].length = 0;
        tx_ring[i].cso = 0;
        tx_ring[i].cmd = 0;
        tx_ring[i].status = 0;
        tx_ring[i].css = 0;
        tx_ring[i].special = 0;
    }
    tx_cur = 0;
    barrier();
    uint64_t tx_phys = (uint64_t)(uintptr_t)tx_ring;
    wr(REG_TDBAL, (uint32_t)tx_phys);
    wr(REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(REG_TDLEN, NTX * (uint32_t)sizeof(tx_desc_t));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);

    /* Transmitter: enable, pad short packets, standard collision settings. */
    wr(REG_TIPG, TIPG_DEFAULT);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));

    /* Receiver: enable, broadcast, 2048-byte buffers, strip CRC. Unicast is
     * filtered by RA0 above; no promiscuous mode. */
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    /* RX interrupts only; TX is polled. */
    rxq_head = 0;
    rxq_tail = 0;
    wr(REG_IMS, INT_RX_ANY);
    (void)rd(REG_ICR);

    nic_present = 1;

    /* One line the CI gates grep for (NET_UP="NET: e1000 up"). */
    boot_log("NET: e1000 up - MAC:");
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        early_console_write_hex(mac[i]);
    }
    boot_log("NET: e1000 IRQ line:");
    early_console_write_hex(nic_irq);
    return 1;
}

/* ---- netdev binding ---- */
const netdev_ops_t e1000_netdev = {
    .name = "e1000",
    .init = e1000_init,
    .present = e1000_present,
    .get_mac = e1000_get_mac,
    .transmit = e1000_transmit,
    .irq = e1000_irq,
    .irq_line = e1000_irq_line,
    .receive = e1000_receive,
};
