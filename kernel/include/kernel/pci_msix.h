/**
 * QuantumOS minimal PCI MSI-X (#253)
 *
 * Just enough MSI-X for one virtio-pci function: find the capability
 * (id 0x11), map the vector table it names, point `count` entries at the
 * boot CPU's local APIC with consecutive IDT vectors, and switch the
 * function from INTx to MSI-X. There is no vector allocator: the caller
 * owns a fixed block of IDT vectors (MSIX_VECTOR_BASE..) and says which it
 * wants, so two devices cannot be given MSI-X until a second block exists.
 *
 * An MSI is a memory write to the local APIC, so the APIC must be
 * software-enabled before the first one lands (it is hardware-enabled
 * and left software-disabled by firmware). pci_msix_lapic_enable() sets
 * the spurious-vector register's enable bit, keeps LINT0 as the ExtINT
 * path the 8259 already delivers through (so the timer, the console and
 * every INTx device are untouched), and maps the APIC window. A handler
 * on an MSI vector ends with pci_msix_eoi(): the APIC, not the 8259,
 * tracks these in-service.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PCI_MSIX_H
#define PCI_MSIX_H

#include <kernel/types.h>
#include <kernel/pci.h>

/* The IDT vectors MSI-X devices may use: a small block above the 8259's
 * 32..47 and below the syscall gate at 128. interrupts.c installs stubs
 * for exactly these; a device asking for more than MSIX_VECTOR_COUNT
 * entries is refused. */
#define MSIX_VECTOR_BASE 64
#define MSIX_VECTOR_COUNT 4

/* Program the function's MSI-X table so entry i (0 <= i < count) raises
 * IDT vector MSIX_VECTOR_BASE + i on the boot CPU, then enable MSI-X and
 * disable INTx. Returns 1 on success; 0 when the function has no MSI-X
 * capability, its table is smaller than `count`, `count` exceeds the
 * vector block, or the table's BAR cannot be mapped. On 0 nothing was
 * changed: INTx is still the function's interrupt. */
int pci_msix_setup(const pci_device_t *dev, uint8_t count);

/* Software-enable the local APIC (idempotent). Returns 1 when MSIs can be
 * delivered, 0 if the APIC window could not be mapped. */
int pci_msix_lapic_enable(void);

/* End-of-interrupt for an MSI-X vector: write the local APIC's EOI. */
void pci_msix_eoi(void);

#endif /* PCI_MSIX_H */
