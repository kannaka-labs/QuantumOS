/**
 * QuantumOS Per-Process Address Spaces Implementation
 *
 * Physical frames are identity-mapped in the kernel half, so a frame's
 * physical address doubles as the kernel VA used to fill its entries.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/vmspace.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

/* Boot page tables (kernel/src/boot.S) — the shared kernel half */
extern uint64_t boot_pml4[];
extern uint64_t boot_pdpt[];
extern uint64_t boot_pd[];

/* How many address spaces vmspace_create has built (kernel/src/mmio.c logs
 * a device mapping added after the first one, which that space lacks). */
static int created_count;

int vmspace_created_count(void) {
    return created_count;
}

/* CR3 helpers (kernel/src/interrupts.S) */
extern uint64_t get_cr3(void);
extern void set_cr3(uint64_t cr3);

/* Paging entry flags */
#define PG_PRESENT 0x001
#define PG_RW 0x002
#define PG_USER 0x004
#define PG_ADDR 0x000FFFFFFFFFF000ULL

static uint64_t *alloc_table(void) {
    void *frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    /* Frame is physical == identity-mapped kernel VA */
    memset(frame, 0, PAGE_SIZE);
    return (uint64_t *)frame;
}

uint64_t vmspace_kernel_cr3(void) {
    return (uint64_t)boot_pml4;
}

address_space_t vmspace_create(void) {
    address_space_t as = {NULL, 0};

    uint64_t *pml4 = alloc_table();
    if (!pml4) {
        return as;
    }
    uint64_t *pdpt = alloc_table();
    if (!pdpt) {
        return as; /* frame leaks; kfree is a no-op anyway */
    }

    /* PML4[0] -> our PDPT (user bit on so the walker reaches user
     * pages; leaf pages enforce supervisor/user individually) */
    pml4[0] = ((uint64_t)pdpt & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;

    /* PDPT[0] -> the shared boot page directory: identity map of the
     * low 1 GB with supervisor-only 2 MB pages (the kernel half) */
    pdpt[0] = ((uint64_t)boot_pd & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;

    /* PDPT[1] (user half, 1–2 GB) created lazily in map_page */

    /* Kernel device windows above the low GB (MMIO NIC registers, a GRUB
     * framebuffer — kernel/src/mmio.c, fb.c) live in the boot tables. Share
     * them, supervisor-only (no U bit on the entry), so an interrupt that
     * arrives while this space's CR3 is loaded can still reach the device.
     * vmspace_destroy never frees an entry identical to the kernel's. */
    for (int i = 2; i < 512; i++) {
        if (boot_pdpt[i] & PG_PRESENT) {
            pdpt[i] = boot_pdpt[i] & ~PG_USER;
        }
    }
    for (int i = 1; i < 256; i++) {
        if (boot_pml4[i] & PG_PRESENT) {
            pml4[i] = boot_pml4[i] & ~PG_USER;
        }
    }

    created_count++;
    as.pml4 = pml4;
    as.cr3 = (uint64_t)pml4;
    return as;
}

bool vmspace_map_page(address_space_t *as, uint64_t uvaddr, uint64_t paddr, bool writable) {
    if (!as || !as->pml4) {
        return false;
    }

    uint64_t pdpt_index = (uvaddr >> 30) & 0x1FF;
    uint64_t pd_index = (uvaddr >> 21) & 0x1FF;
    uint64_t pt_index = (uvaddr >> 12) & 0x1FF;

    uint64_t *pdpt = (uint64_t *)(as->pml4[0] & PG_ADDR);

    /* Page directory for this PDPT slot */
    if (!(pdpt[pdpt_index] & PG_PRESENT)) {
        uint64_t *pd = alloc_table();
        if (!pd) {
            return false;
        }
        pdpt[pdpt_index] = ((uint64_t)pd & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;
    }
    uint64_t *pd = (uint64_t *)(pdpt[pdpt_index] & PG_ADDR);

    /* Page table for this PD slot */
    if (!(pd[pd_index] & PG_PRESENT)) {
        uint64_t *pt = alloc_table();
        if (!pt) {
            return false;
        }
        pd[pd_index] = ((uint64_t)pt & PG_ADDR) | PG_PRESENT | PG_RW | PG_USER;
    }
    uint64_t *pt = (uint64_t *)(pd[pd_index] & PG_ADDR);

    uint64_t flags = PG_PRESENT | PG_USER | (writable ? PG_RW : 0);
    pt[pt_index] = (paddr & PG_ADDR) | flags;
    return true;
}

bool vmspace_user_ok(const uint64_t *pml4, uint64_t uvaddr, uint64_t len, bool need_write) {
    if (len == 0) {
        return true; /* nothing accessed (also guards the uvaddr+len-1 underflow) */
    }
    if (uvaddr + len < uvaddr) {
        return false; /* wrap */
    }
    /* Confine to the user half (PDPT[1], all 4 KB pages). Below USER_VBASE lies
     * the shared kernel identity map (boot_pd, supervisor 2 MB PS pages); a walk
     * there would misread a 2 MB frame as a page table (no PS-bit handling here)
     * and could falsely PASS — an escalation strictly worse than the DoS. This
     * also kills any high address that aliases pml4[0]. Restores the exact
     * [USER_VBASE, 0x80000000) bound the replaced in_user_range enforced. */
    if (uvaddr < USER_VBASE || uvaddr + len > 0x80000000UL) {
        return false;
    }
    if (!pml4) {
        return false;
    }

    uint64_t last = uvaddr + len - 1; /* last BYTE touched, never uvaddr+len */
    for (uint64_t p = uvaddr & ~0xFFFULL; p <= (last & ~0xFFFULL); p += 0x1000) {
        /* Present-check BEFORE forming each child pointer: a non-present entry
         * masks to phys 0, which boot_pd identity-maps, so dereferencing it
         * would silently read garbage rather than fault. */
        if (!(pml4[0] & PG_PRESENT)) {
            return false;
        }
        const uint64_t *pdpt = (const uint64_t *)(pml4[0] & PG_ADDR);
        uint64_t e = pdpt[(p >> 30) & 0x1FF];
        if (!(e & PG_PRESENT)) {
            return false;
        }
        const uint64_t *pd = (const uint64_t *)(e & PG_ADDR);
        e = pd[(p >> 21) & 0x1FF];
        if (!(e & PG_PRESENT)) {
            return false; /* the range check already excludes the PS 2 MB region */
        }
        const uint64_t *pt = (const uint64_t *)(e & PG_ADDR);
        uint64_t pte = pt[(p >> 12) & 0x1FF];
        /* User accessibility is enforced at the LEAF only (map_page sets USER on
         * intermediates so the walker reaches user pages). */
        if ((pte & (PG_PRESENT | PG_USER)) != (PG_PRESENT | PG_USER)) {
            return false;
        }
        if (need_write && !(pte & PG_RW)) {
            return false;
        }
    }
    return true;
}

void vmspace_switch(uint64_t cr3) {
    if (cr3 && cr3 != get_cr3()) {
        set_cr3(cr3);
    }
}

void vmspace_destroy(uint64_t *pml4) {
    if (!pml4) {
        return;
    }

    uint64_t *pdpt = (uint64_t *)(pml4[0] & PG_ADDR);
    if (pdpt) {
        /* Walk the user half only. PDPT[0] points at the SHARED boot
         * page directory (the kernel half) — never free it. */
        for (int pi = 1; pi < 512; pi++) {
            if (!(pdpt[pi] & PG_PRESENT)) {
                continue;
            }
            if (!(pdpt[pi] & PG_USER)) {
                /* A shared kernel device window (copied supervisor-only in
                 * vmspace_create). User tables always carry PG_USER (see
                 * vmspace_map_page), so this never skips one of ours. */
                continue;
            }
            uint64_t *pd = (uint64_t *)(pdpt[pi] & PG_ADDR);
            for (int di = 0; di < 512; di++) {
                if (!(pd[di] & PG_PRESENT)) {
                    continue;
                }
                uint64_t *pt = (uint64_t *)(pd[di] & PG_ADDR);
                for (int ti = 0; ti < 512; ti++) {
                    if (pt[ti] & PG_PRESENT) {
                        pmm_free_frame((void *)(pt[ti] & PG_ADDR)); /* user page */
                    }
                }
                pmm_free_frame(pt); /* page table */
            }
            pmm_free_frame(pd); /* page directory */
        }
        pmm_free_frame(pdpt); /* PDPT */
    }
    pmm_free_frame(pml4); /* PML4 */
}
