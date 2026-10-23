/**
 * QuantumOS MMIO identity mapping
 *
 * The boot page tables (kernel/src/boot.S) identity-map only the low 1 GB.
 * Device registers live far above that: Firecracker places virtio-mmio at
 * 0xd0000000, and PCI firmware places memory BARs in the 3-4 GB hole or a
 * 64-bit window above RAM. Touching any of them without a mapping is a page
 * fault, so every MMIO driver maps its window here first.
 *
 * Mappings are identity (VA == PA), 1 GB at a time with 2 MB supervisor
 * pages marked uncached (PCD|PWT), built from a small static pool of page
 * tables. They are added to the KERNEL tables (boot_pml4 / boot_pdpt);
 * vmspace_create() copies the kernel's high supervisor entries into every
 * new address space, so an interrupt that arrives while a user process's
 * CR3 is loaded still reaches the device. Map before the first user
 * process is created (the NIC and framebuffer probes run at boot, long
 * before process_create); a late mapping is logged.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef MMIO_H
#define MMIO_H

#include <kernel/types.h>

/* Identity-map [base, base+len) for supervisor MMIO access. Returns 1 when
 * the whole range is mapped (already-present 1 GB regions count as mapped),
 * 0 when the range is unsupported (the user half 1-2 GB, at or above
 * 128 TiB) or the static table pool is exhausted. */
int mmio_identity_map(uint64_t base, uint64_t len);

#endif /* MMIO_H */
