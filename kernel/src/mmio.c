/**
 * QuantumOS MMIO identity mapping — implementation (see kernel/mmio.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/mmio.h>
#include <kernel/boot.h>

extern uint64_t boot_pml4[];
extern uint64_t boot_pdpt[];
extern uint64_t get_cr3(void);
extern void set_cr3(uint64_t cr3);

/* Number of user address spaces created so far (kernel/src/vmspace.c).
 * A mapping added after the first one is not in that space's tables. */
extern int vmspace_created_count(void);

#define GB (1ULL << 30)
#define PT_P 0x001ULL
#define PT_RW 0x002ULL
#define PT_PWT 0x008ULL
#define PT_PCD 0x010ULL
#define PT_PS 0x080ULL
#define PT_ADDR 0x000FFFFFFFFFF000ULL

/* Page-table pool. Static, so it lives in the low identity-mapped BSS and
 * a table's kernel VA is its physical address. Four 1 GB MMIO regions and
 * one extra PDPT (for a 64-bit BAR placed above 512 GB) are far more than
 * a NIC and a framebuffer need. */
#define POOL_PD 4
#define POOL_PDPT 1
static uint64_t pool_pd[POOL_PD][512] __attribute__((aligned(4096)));
static uint64_t pool_pdpt[POOL_PDPT][512] __attribute__((aligned(4096)));
static int pd_used, pdpt_used;

static void zero_table(uint64_t *t) {
    for (int i = 0; i < 512; i++) {
        t[i] = 0;
    }
}

/* Map the one 1 GB region whose index (address >> 30) is `gb`. */
static int map_region(uint64_t gb) {
    uint64_t pml4i = gb >> 9;  /* 512 GB per PML4 entry */
    uint64_t pdpti = gb & 511; /* 1 GB per PDPT entry   */

    if (pml4i == 0 && pdpti == 1) {
        /* 1-2 GB is the private user half (vmspace.h USER_VBASE): never
         * a kernel identity map, in any address space. */
        return 0;
    }
    if (pml4i >= 256) {
        return 0; /* the canonical upper half is not used by QuantumOS */
    }

    uint64_t *pdpt;
    if (pml4i == 0) {
        pdpt = boot_pdpt;
    } else if (boot_pml4[pml4i] & PT_P) {
        pdpt = (uint64_t *)(boot_pml4[pml4i] & PT_ADDR);
    } else {
        if (pdpt_used >= POOL_PDPT) {
            return 0;
        }
        pdpt = pool_pdpt[pdpt_used++];
        zero_table(pdpt);
        boot_pml4[pml4i] = ((uint64_t)(uintptr_t)pdpt & PT_ADDR) | PT_P | PT_RW;
    }

    if (pdpt[pdpti] & PT_P) {
        return 1; /* already mapped (the low GB, the framebuffer, a sibling BAR) */
    }
    if (pd_used >= POOL_PD) {
        return 0;
    }
    uint64_t *pd = pool_pd[pd_used++];
    uint64_t region = gb * GB;
    for (int i = 0; i < 512; i++) {
        pd[i] = (region + (uint64_t)i * 0x200000ULL) | PT_P | PT_RW | PT_PWT | PT_PCD | PT_PS;
    }
    pdpt[pdpti] = ((uint64_t)(uintptr_t)pd & PT_ADDR) | PT_P | PT_RW;
    return 1;
}

int mmio_identity_map(uint64_t base, uint64_t len) {
    if (len == 0) {
        len = 1;
    }
    uint64_t end = base + len - 1;
    if (end < base || end >= (1ULL << 47)) {
        return 0; /* wrap, or outside the lower canonical half */
    }
    for (uint64_t gb = base >> 30; gb <= (end >> 30); gb++) {
        if (!map_region(gb)) {
            boot_log("MMIO: cannot map device window - unsupported range or table pool exhausted");
            return 0;
        }
    }
    set_cr3(get_cr3()); /* flush the TLB so the new entries take effect */
    if (vmspace_created_count() > 0) {
        boot_log("MMIO: window mapped after user address spaces exist - not visible to them");
    }
    return 1;
}
