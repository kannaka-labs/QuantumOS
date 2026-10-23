/**
 * QuantumOS minimal PCI enumeration (epic #73 phase 1)
 *
 * Just enough PCI to find one device on bus 0 and read its I/O BAR and
 * interrupt line — the rtl8139 NIC. Configuration space is accessed
 * through the classic 0xCF8 (address) / 0xCFC (data) port pair.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PCI_H
#define PCI_H

#include <kernel/types.h>

/* A located PCI function's essentials. */
typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor;
    uint16_t device;
    uint32_t bar0;    /* raw BAR0 */
    uint16_t io_base; /* BAR0 & ~3 when it is an I/O BAR */
    uint8_t irq_line; /* config offset 0x3C */
    bool found;
} pci_device_t;

/* 32-bit config read/write (offset must be 4-byte aligned). */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* Scan bus 0 for the first function matching vendor:device. Returns a
 * device with .found set, or .found == false. */
pci_device_t pci_find(uint16_t vendor, uint16_t device);

/* Set the bus-master + I/O-space enable bits in the command register so
 * the device can DMA and answer port I/O. */
void pci_enable_bus_master_io(const pci_device_t *dev);

/* Set the bus-master + memory-space enable bits, for a device driven
 * through memory BARs (virtio-pci modern). */
void pci_enable_bus_master_mem(const pci_device_t *dev);

#endif /* PCI_H */
