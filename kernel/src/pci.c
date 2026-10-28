/**
 * QuantumOS minimal PCI enumeration implementation (epic #73 phase 1).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/pci.h>
#include <kernel/boot.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_VENDOR_ID 0x00
#define PCI_COMMAND 0x04
#define PCI_BAR0 0x10
#define PCI_INTERRUPT_LINE 0x3C

#define PCI_CMD_IO 0x0001
#define PCI_CMD_MEM 0x0002
#define PCI_CMD_BUS_MASTER 0x0004

static inline void io_outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t io_inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t config_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint32_t)0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(slot & 0x1F) << 11) |
           ((uint32_t)(func & 0x07) << 8) | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    io_outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, func, offset));
    return io_inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    io_outl(PCI_CONFIG_ADDRESS, config_address(bus, slot, func, offset));
    io_outl(PCI_CONFIG_DATA, value);
}

pci_device_t pci_find(uint16_t vendor, uint16_t device) {
    pci_device_t d;
    d.found = false;

    /* Bus 0 only, function 0 only (the rtl8139 is single-function and
     * QEMU places it on bus 0). Enough for one NIC. */
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_config_read32(0, slot, 0, PCI_VENDOR_ID);
        uint16_t vid = (uint16_t)(id & 0xFFFF);
        uint16_t did = (uint16_t)(id >> 16);
        if (vid == 0xFFFF) {
            continue; /* no device in this slot */
        }
        if (vid == vendor && did == device) {
            d.bus = 0;
            d.slot = slot;
            d.func = 0;
            d.vendor = vid;
            d.device = did;
            d.bar0 = pci_config_read32(0, slot, 0, PCI_BAR0);
            /* Bit 0 set => I/O-space BAR; the base is the top bits. */
            d.io_base = (uint16_t)(d.bar0 & ~0x3u);
            d.irq_line = (uint8_t)(pci_config_read32(0, slot, 0, PCI_INTERRUPT_LINE) & 0xFF);
            d.found = true;
            return d;
        }
    }
    return d;
}

void pci_enable_bus_master_io(const pci_device_t *dev) {
    if (!dev || !dev->found) {
        return;
    }
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (PCI_CMD_IO | PCI_CMD_BUS_MASTER);
    pci_config_write32(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

void pci_enable_bus_master_mem(const pci_device_t *dev) {
    if (!dev || !dev->found) {
        return;
    }
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (PCI_CMD_MEM | PCI_CMD_BUS_MASTER);
    pci_config_write32(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

uint64_t pci_bar_mem_base(const pci_device_t *dev, uint8_t bar) {
    if (!dev || !dev->found || bar > 5) {
        return 0;
    }
    uint32_t lo = pci_config_read32(dev->bus, dev->slot, dev->func, (uint8_t)(PCI_BAR0 + 4 * bar));
    if (lo & 1) {
        return 0; /* I/O-space BAR */
    }
    uint64_t base = lo & ~0xFULL;
    if (((lo >> 1) & 3) == 2 && bar < 5) { /* 64-bit: the next BAR is the high half */
        base |= (uint64_t)pci_config_read32(dev->bus, dev->slot, dev->func,
                                            (uint8_t)(PCI_BAR0 + 4 * (bar + 1)))
                << 32;
    }
    return base;
}
