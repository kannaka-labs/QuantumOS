/**
 * QuantumOS minimal PCI MSI-X implementation (#253).
 *
 * MSI-X (PCI 3.0 §6.8.2) replaces the shared, level-triggered INTx line
 * with per-vector memory writes: the device writes a 32-bit message to an
 * address in the local APIC's window and the APIC raises the IDT vector
 * in the message's low byte. The capability names a table of entries
 * (address, data, control) living in one of the function's memory BARs.
 *
 * What this file does, in order: walk the capability list for id 0x11,
 * decode table size and location, map the BAR window, fill `count`
 * entries pointing at this CPU's APIC with vectors MSIX_VECTOR_BASE + i,
 * unmask each, set the MSI-X enable bit, and set INTx-disable in the PCI
 * command register so the legacy line goes quiet.
 *
 * The local APIC is at its architectural default 0xFEE00000: nothing in
 * this kernel relocates it (IA32_APIC_BASE is left as firmware set it).
 * The window sits above the boot identity map, so it is mapped the way
 * PCI BARs are (mmio_identity_map, ADR-0024).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/pci_msix.h>
#include <kernel/mmio.h>
#include <kernel/boot.h>

#define PCI_STATUS_DWORD 0x04 /* status is the high half */
#define PCI_STATUS_CAP_LIST (1u << 20)
#define PCI_COMMAND 0x04
#define PCI_CMD_INTX_DISABLE (1u << 10)
#define PCI_CAP_PTR 0x34
#define PCI_CAP_ID_MSIX 0x11

/* MSI-X capability fields (§6.8.2) */
#define MSIX_MSG_CTRL 2 /* u16: table size - 1 in bits 0..10 */
#define MSIX_MC_SIZE_MASK 0x07FF
#define MSIX_MC_FUNC_MASK (1u << 14)
#define MSIX_MC_ENABLE (1u << 15)
#define MSIX_TABLE 4 /* u32: BIR in bits 0..2, offset in bits 3..31 */

/* Table entry (16 bytes each) */
#define MSIX_ENTRY_SIZE 16
#define MSIX_E_ADDR_LO 0
#define MSIX_E_ADDR_HI 4
#define MSIX_E_DATA 8
#define MSIX_E_CTRL 12
#define MSIX_E_CTRL_MASK 1u

/* Local APIC (SDM vol. 3 §10) */
#define LAPIC_BASE 0xFEE00000ULL
#define LAPIC_ID 0x020
#define LAPIC_TPR 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_SVR 0x0F0
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_SVR_ENABLE 0x100
#define LAPIC_SPURIOUS_VECTOR 0xFF /* interrupts.c installs a stub here */
#define LAPIC_LVT_EXTINT 0x700     /* delivery mode ExtINT, unmasked */

/* MSI message address/data encoding (SDM vol. 3 §10.11) */
#define MSI_ADDR_BASE 0xFEE00000u
#define MSI_ADDR_DEST_SHIFT 12 /* physical destination: APIC id in 19..12 */

static volatile uint8_t *lapic; /* 0 until pci_msix_lapic_enable() ran */

static inline uint32_t lr32(uint32_t off) {
    return *(volatile uint32_t *)(lapic + off);
}

static inline void lw32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(lapic + off) = v;
}

static uint8_t cfg8(const pci_device_t *d, uint8_t off) {
    uint32_t v = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off & 0xFC));
    return (uint8_t)(v >> ((off & 3) * 8));
}

static uint16_t cfg16(const pci_device_t *d, uint8_t off) {
    uint32_t v = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off & 0xFC));
    return (uint16_t)(v >> ((off & 2) * 8));
}

static uint32_t cfg32(const pci_device_t *d, uint8_t off) {
    return pci_config_read32(d->bus, d->slot, d->func, off);
}

/* Read-modify-write one 16-bit half of a config dword. */
static void cfg16_write(const pci_device_t *d, uint8_t off, uint16_t val) {
    uint8_t dw = (uint8_t)(off & 0xFC);
    uint32_t v = pci_config_read32(d->bus, d->slot, d->func, dw);
    uint32_t shift = (uint32_t)(off & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_config_write32(d->bus, d->slot, d->func, dw, v);
}

int pci_msix_lapic_enable(void) {
    if (lapic) {
        return 1;
    }
    if (!mmio_identity_map(LAPIC_BASE, 0x1000)) {
        return 0;
    }
    lapic = (volatile uint8_t *)(uintptr_t)LAPIC_BASE;
    /* Enable, and give spurious interrupts a vector that has a stub. */
    lw32(LAPIC_SVR, (lr32(LAPIC_SVR) & ~0xFFu) | LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    /* Virtual-wire mode: the 8259's INTR keeps arriving on LINT0 as ExtINT.
     * Firmware leaves it so on the boot CPU; written explicitly because a
     * software-disabled APIC may have had its LVT masks forced on. */
    lw32(LAPIC_LVT_LINT0, LAPIC_LVT_EXTINT);
    lw32(LAPIC_TPR, 0); /* accept every priority class */
    return 1;
}

void pci_msix_eoi(void) {
    if (lapic) {
        lw32(LAPIC_EOI, 0);
    }
}

int pci_msix_setup(const pci_device_t *dev, uint8_t count) {
    if (!dev || !dev->found || count == 0 || count > MSIX_VECTOR_COUNT) {
        return 0;
    }
    if (!(cfg32(dev, PCI_STATUS_DWORD) & PCI_STATUS_CAP_LIST)) {
        return 0;
    }
    uint8_t cap = 0;
    uint8_t ptr = cfg8(dev, PCI_CAP_PTR) & 0xFC;
    for (int guard = 0; ptr && guard < 48; guard++) {
        if (cfg8(dev, ptr) == PCI_CAP_ID_MSIX) {
            cap = ptr;
            break;
        }
        ptr = cfg8(dev, (uint8_t)(ptr + 1)) & 0xFC;
    }
    if (!cap) {
        return 0; /* no MSI-X: INTx is what this function has */
    }
    uint16_t mc = cfg16(dev, (uint8_t)(cap + MSIX_MSG_CTRL));
    uint16_t table_size = (uint16_t)((mc & MSIX_MC_SIZE_MASK) + 1);
    if (table_size < count) {
        return 0;
    }
    uint32_t tbl = cfg32(dev, (uint8_t)(cap + MSIX_TABLE));
    uint64_t base = pci_bar_mem_base(dev, (uint8_t)(tbl & 7));
    if (base == 0) {
        return 0; /* an I/O BAR, or none: cannot be a table */
    }
    uint64_t table = base + (tbl & ~7u);
    uint32_t table_len = (uint32_t)count * MSIX_ENTRY_SIZE;
    if (!mmio_identity_map(table, table_len)) {
        return 0;
    }
    if (!pci_msix_lapic_enable()) {
        return 0;
    }
    volatile uint8_t *t = (volatile uint8_t *)(uintptr_t)table;
    uint32_t apic_id = lr32(LAPIC_ID) >> 24;
    uint32_t addr = MSI_ADDR_BASE | (apic_id << MSI_ADDR_DEST_SHIFT);
    for (uint32_t i = 0; i < count; i++) {
        volatile uint8_t *e = t + i * MSIX_ENTRY_SIZE;
        /* Mask while writing: the spec forbids changing a live entry. */
        *(volatile uint32_t *)(e + MSIX_E_CTRL) = MSIX_E_CTRL_MASK;
        *(volatile uint32_t *)(e + MSIX_E_ADDR_LO) = addr;
        *(volatile uint32_t *)(e + MSIX_E_ADDR_HI) = 0;
        /* Fixed delivery, edge, vector in the low byte. */
        *(volatile uint32_t *)(e + MSIX_E_DATA) = (uint32_t)(MSIX_VECTOR_BASE + i);
        *(volatile uint32_t *)(e + MSIX_E_CTRL) = 0; /* unmask */
    }
    __asm__ volatile("" ::: "memory");
    /* Enable MSI-X with the function mask clear, then silence INTx. */
    cfg16_write(dev, (uint8_t)(cap + MSIX_MSG_CTRL),
                (uint16_t)((mc & ~MSIX_MC_FUNC_MASK) | MSIX_MC_ENABLE));
    uint32_t cmd = cfg32(dev, PCI_COMMAND);
    pci_config_write32(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd | PCI_CMD_INTX_DISABLE);
    return 1;
}
