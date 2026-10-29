#include <kernel/types.h>
#include <kernel/memory.h>
#include <kernel/boot.h>

// External symbols. The heap bounds are declared as unbounded arrays so
// the block header can be placed at __heap_start without the optimiser's
// -Warray-bounds treating the symbol as a 1-byte object.
extern uint8_t __end;
extern uint8_t __heap_start[];
extern uint8_t __heap_end[];

// Global memory managers
static physical_memory_t pmm;
static virtual_memory_t vmm;
static memory_allocator_t kernel_heap;

// Frame allocator search rover: the index pmm_alloc_frame starts its next scan
// from. Without it every allocation re-scanned the bitmap from frame 0 — O(total
// frames) per call, and the low frames are permanently reserved (kernel image +
// bitmap), so the scan always paid to skip them first. The rover advances past
// the last frame handed out so a run of sequential allocations (page tables, a
// spawn's segments+stack) is amortized O(1); pmm_free_frame rewinds it to a
// freed frame so holes are refilled before the rover marches on. Guarded by the
// same irq_save bracket as the bitmap it indexes. Invariant: 0 <= hint < total.
static uint32_t pmm_alloc_hint;

// Interrupt save/restore (single CPU). The PMM bitmap+counters and the heap
// free-list are shared tables that the IF=1 process-teardown path mutates
// (health_monitor_thread -> process_destroy -> vmspace_destroy -> pmm_free_frame,
// and kfree of the kernel stack) while a cli'd SYS_SPAWN concurrently runs
// pmm_alloc_frame / kmalloc. A non-atomic load-modify-store of a bitmap byte or
// counter, timer-preempted mid-update, loses the interleaved write and marks an
// in-use frame free -> it is re-handed-out, aliasing one physical frame across
// two address spaces. Bracket every such mutation cli'd, exactly as kmalloc/
// kfree already do.
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" ::"r"(f) : "memory", "cc");
}

// Clamp a raw physical frame count to the 1 GB identity-map ceiling (ADR-0021).
// Factored out of pmm_init so pmm_clamp_selftest can prove the clamp on a
// synthetic > 1 GB input without a machine that actually has > 1 GB RAM.
static uint64_t pmm_clamp_frames(uint64_t frames) {
    return frames > PMM_MAX_FRAMES ? PMM_MAX_FRAMES : frames;
}

// ADR-0021 clamp teeth-check: a frame count derived from a region larger than the
// 1 GB identity map MUST clamp to exactly PMM_MAX_FRAMES, a count within the
// ceiling MUST pass through unchanged, and the boundary is exact. Uses synthetic
// inputs, so the -m 128M boot leg proves the clamp is load-bearing without any
// > 1 GB RAM; reverting the clamp fails this before "QuantumOS ready".
int pmm_clamp_selftest(void) {
    uint64_t over = 0x60000000ULL / PAGE_SIZE; // 1.5 GB of frames = 393216
    if (pmm_clamp_frames(over) != PMM_MAX_FRAMES) {
        return 1;
    }
    if (pmm_clamp_frames(0x8000) != 0x8000) { // 128 MB passes through unchanged
        return 2;
    }
    if (pmm_clamp_frames(PMM_MAX_FRAMES) != PMM_MAX_FRAMES) { // boundary exact
        return 3;
    }
    return 0;
}

// Physical memory management
mem_result_t pmm_init(uint64_t total_memory) {
    boot_log("Initializing physical memory manager...");

    // Calculate total frames
    uint64_t total_frames = total_memory / PAGE_SIZE;
    // Clamp to the 1 GB identity-map ceiling BEFORE narrowing to uint32 and
    // before the bitmap is sized: every frame the allocator can ever hand out
    // must be mappable through boot.S's identity map (ADR-0021).
    if (total_frames == 0) {
        boot_panic("PMM: zero usable RAM");
    }
    total_frames = pmm_clamp_frames(total_frames);
    // Floor: the kernel heap's backing frames [__heap_start, __heap_end) are a
    // fixed static-layout range; a machine reporting less RAM than that would let
    // kheap_init build the heap on unbacked physical memory. Fail loudly.
    if (total_frames < ((uintptr_t)__heap_end + PAGE_SIZE - 1) / PAGE_SIZE) {
        boot_panic("PMM: RAM below the static kernel-layout floor");
    }
    pmm.total_frames = (uint32_t)total_frames;
    pmm.free_frames = (uint32_t)total_frames;
    pmm.used_frames = 0;
    pmm.highest_frame = total_frames - 1;

    // Frame bitmap sized for the 1 GB MAXIMUM (fixed 32 KB), not detected RAM: the
    // buffer then always covers the fixed physical reservation ranges (kernel
    // image, heap) no matter what the untrusted mmap reported, structurally ruling
    // out a small-RAM overrun. Frames total_frames..PMM_MAX_FRAMES-1 stay marked
    // free but unreachable (the allocator scan is bounded by total_frames). Round
    // UP retained (exact here) as the #187 hardening against a future ceiling bump.
    size_t bitmap_size = (PMM_MAX_FRAMES + 7) / 8;
    pmm.frame_bitmap = (uint8_t *)ALIGN_UP((uintptr_t)&__end, PAGE_SIZE);

    // The bitmap sits just past the kernel image, inside the 16 MB kernel region
    // below the stack/heap origin at 0x01100000 (link.ld). Assert it cannot grow
    // into that region, so the unstated "__end + bitmap < ram-origin" invariant
    // fails loudly rather than silently corrupting the stack.
    if ((uintptr_t)pmm.frame_bitmap + bitmap_size > 0x01100000UL) {
        boot_panic("PMM: frame bitmap overruns the kernel region");
    }

    // Initialize bitmap (all frames initially free)
    memset(pmm.frame_bitmap, 0, bitmap_size);

    // Reserve frames used by kernel (image + this bitmap, which self-reserves via
    // kernel_end below regardless of its size).
    uint64_t kernel_end = (uint64_t)pmm.frame_bitmap + bitmap_size;
    uint64_t kernel_frames = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < kernel_frames && i < total_frames; i++) {
        uint32_t frame = i;
        uint32_t byte_index = frame / 8;
        uint8_t bit_index = frame % 8;
        pmm.frame_bitmap[byte_index] |= (1 << bit_index);
        pmm.free_frames--;
        pmm.used_frames++;
    }

    // Reserve the kernel heap's backing frames. kheap_init (below) builds the
    // heap DIRECTLY on link.ld's `ram` region [__heap_start, __heap_end) — those
    // physical frames are identity-mapped and live, but they sit far above the
    // kernel image, so the loop above never covered them. Left free, the low-first
    // allocator rover would eventually hand a live-heap frame to a page table or
    // ELF segment and the caller's memset would shred a kblock header (reachable
    // under a large-residency spawn storm). Reserve [heap_lo, heap_hi) explicitly.
    // The loop is bounded by total_frames so it can never write a bit past the
    // bitmap — the exact out-of-bounds class this reservation exists to prevent.
    uint64_t heap_lo = (uintptr_t)__heap_start / PAGE_SIZE;
    uint64_t heap_hi = ((uintptr_t)__heap_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = heap_lo; i < heap_hi && i < total_frames; i++) {
        uint32_t byte_index = (uint32_t)i / 8;
        uint8_t bit_index = (uint32_t)i % 8;
        // Guard the counters so free+used==total holds even if this range ever
        // overlapped an already-reserved frame (it does not at 128MB).
        if (!(pmm.frame_bitmap[byte_index] & (1 << bit_index))) {
            pmm.frame_bitmap[byte_index] |= (1 << bit_index);
            pmm.free_frames--;
            pmm.used_frames++;
        }
    }

    boot_log("Physical memory manager initialized");
    // Dedicated ADR-0021 size marker on ONE line: the exact 16-wide hex frame
    // count derived from the multiboot memory map. Emitted with the raw console
    // writer (not boot_log, which appends its newline BEFORE the hex and would
    // split label from value) so the whole token stays greppable. The ci-smoke
    // mem legs grep this to prove the PMM is sized from real RAM, not a constant.
    early_console_write("PMMSIZE=");
    early_console_write_hex(pmm.total_frames);
    early_console_write("\r\n");
    boot_log("Total frames: ");
    early_console_write_hex(pmm.total_frames);
    boot_log("Free frames: ");
    early_console_write_hex(pmm.free_frames);

    return MEM_SUCCESS;
}

void *pmm_alloc_frame(void) {
    // Atomic vs the IF=1 teardown path (see irq_save note): the scan+mark of the
    // bitmap byte and the counter updates must not be split by a timer IRQ that
    // preempts into pmm_free_frame on the same byte. The rover (pmm_alloc_hint)
    // is part of that shared state, so it is read/written inside the bracket.
    uint64_t flags = irq_save();
    uint32_t total = pmm.total_frames;
    // Scan from the rover, wrapping once, so every frame is examined AT MOST once
    // — the allocator still returns a free frame iff one exists, but a run of
    // sequential allocations no longer re-scans the reserved low frames each call.
    for (uint32_t n = 0; n < total; n++) {
        uint32_t i = pmm_alloc_hint + n;
        if (i >= total) {
            i -= total; // single wrap: pmm_alloc_hint < total and n < total, so i < 2*total
        }
        uint32_t byte_index = i / 8;
        uint8_t bit_index = i % 8;

        if (!(pmm.frame_bitmap[byte_index] & (1 << bit_index))) {
            // Mark frame as used
            pmm.frame_bitmap[byte_index] |= (1 << bit_index);
            pmm.free_frames--;
            pmm.used_frames++;

            // Advance the rover past this frame (wrap to 0 at the end).
            pmm_alloc_hint = (i + 1 < total) ? (i + 1) : 0;

            void *frame_addr = (void *)((uintptr_t)i * PAGE_SIZE);
            irq_restore(flags);
            return frame_addr;
        }
    }

    irq_restore(flags);
    return NULL; // Out of memory
}

mem_result_t pmm_free_frame(void *frame_addr) {
    uint64_t frame_num = (uint64_t)frame_addr / PAGE_SIZE;

    if (frame_num >= pmm.total_frames) {
        return MEM_ERROR_INVALID_ADDRESS;
    }

    uint32_t byte_index = frame_num / 8;
    uint8_t bit_index = frame_num % 8;

    // Atomic check-then-clear of the bitmap byte + counters (see irq_save note).
    uint64_t flags = irq_save();
    if (!(pmm.frame_bitmap[byte_index] & (1 << bit_index))) {
        irq_restore(flags);
        return MEM_ERROR_INVALID_ADDRESS; // Already free
    }

    // Mark frame as free
    pmm.frame_bitmap[byte_index] &= ~(1 << bit_index);
    pmm.free_frames++;
    pmm.used_frames--;

    // Rewind the rover so a freed hole below it is refilled before the rover
    // marches further — keeps allocations compact and the scan short.
    if ((uint32_t)frame_num < pmm_alloc_hint) {
        pmm_alloc_hint = (uint32_t)frame_num;
    }

    irq_restore(flags);
    return MEM_SUCCESS;
}

uint32_t pmm_get_free_frames(void) {
    return pmm.free_frames;
}

uint32_t pmm_get_total_frames(void) {
    return pmm.total_frames;
}

// Frame-allocator correctness self-test (the rover optimization's gate). The
// rover changes WHICH free frame is returned, so its risk is a hint/wrap bug
// that double-hands a live frame or loses one. This asserts, deterministically:
//   A. a batch of allocations are all DISTINCT (no double-hand),
//   B. after freeing one, the next allocation never returns a still-live frame
//      (the rewind/wrap picks a genuinely free slot),
//   C. freeing the batch restores the exact free-frame count (no leak, no loss).
// Returns 0 on pass. The boot itself allocates thousands of frames (wrapping the
// rover many times), so a broken wrap ALSO fails the boot's later gates. Runs at
// init time on one CPU; the temporary allocations are all released before return.
int pmm_alloc_selftest(void) {
    uint32_t base = pmm_get_free_frames();
    void *f[32];

    // A. distinct allocations
    for (int i = 0; i < 32; i++) {
        f[i] = pmm_alloc_frame();
        if (!f[i]) {
            for (int j = 0; j < i; j++) {
                pmm_free_frame(f[j]);
            }
            return -1;
        }
    }
    for (int i = 0; i < 32; i++) {
        for (int j = i + 1; j < 32; j++) {
            if (f[i] == f[j]) {
                return -2; /* double-hand (boot panics -> memory not restored, fine) */
            }
        }
    }

    // B. free one, realloc must not collide with a still-live frame
    pmm_free_frame(f[0]);
    void *g = pmm_alloc_frame();
    if (!g) {
        return -3;
    }
    for (int i = 1; i < 32; i++) {
        if (g == f[i]) {
            return -4;
        }
    }
    f[0] = g;

    // C. free all; the count must return exactly to baseline
    for (int i = 0; i < 32; i++) {
        pmm_free_frame(f[i]);
    }
    if (pmm_get_free_frames() != base) {
        return -5;
    }

    // D. WRAP correctness — the invariant a from-the-bottom test would miss. The
    // modulo-to-0 wrap only fires when a scan finds NOTHING free from `hint` to
    // the end, so mark the top 4 frames used (direct bitmap poke — restored
    // below; single-CPU, pre-scheduler) and point the rover at them. The next
    // allocation MUST wrap past total to reach a free low frame: a correct wrap
    // returns a valid in-range frame; a broken one yields addr >= RAM (or NULL).
    uint32_t total = pmm_get_total_frames();
    if (total > 16) {
        for (uint32_t k = total - 4; k < total; k++) {
            pmm.frame_bitmap[k / 8] |= (uint8_t)(1u << (k % 8)); /* occupy top */
        }
        pmm_alloc_hint = total - 4;
        uint64_t limit = (uint64_t)total * PAGE_SIZE;
        void *wrapped = pmm_alloc_frame(); /* forced to wrap to 0 to find a frame */
        int ok = wrapped != NULL && (uint64_t)(uintptr_t)wrapped < limit;
        if (wrapped) {
            pmm_free_frame(wrapped);
        }
        for (uint32_t k = total - 4; k < total; k++) {
            pmm.frame_bitmap[k / 8] &= (uint8_t)~(1u << (k % 8)); /* release top */
        }
        pmm_alloc_hint = 0;
        if (!ok || pmm_get_free_frames() != base) {
            return -6;
        }
    }
    return 0;
}

// Heap-reservation gate (ADR-0021): pmm_init must reserve the kernel heap's
// backing frames [__heap_start, __heap_end) so the low-first allocator rover can
// never hand a live-heap frame to a page table / ELF segment (whose memset would
// shred a kblock header). Proven three ways, all at the shipped -m 128M:
//   1. positive — every heap frame's bitmap bit is SET;
//   2. vacuity  — the frames JUST below and AT the heap end are CLEAR, so the
//      reservation is bounded to the heap, not a constant mark-everything (and the
//      exclusive end / round-up did not over-reserve);
//   3. behavioral — aim the rover straight into the heap and allocate one frame;
//      a correct reservation makes the allocator SKIP the reserved run and return
//      a frame outside [heap_lo, heap_hi). Reverting the reservation flips 1 and 3.
// Returns 0 on pass, negative on the first failed invariant.
int pmm_heap_reservation_selftest(void) {
    uint64_t heap_lo = (uintptr_t)__heap_start / PAGE_SIZE;
    uint64_t heap_hi = ((uintptr_t)__heap_end + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t total = pmm_get_total_frames();
    if (heap_lo >= heap_hi || heap_hi > total || heap_lo == 0) {
        return -1; /* geometry sanity: the heap must be a real range below RAM top */
    }

    // 1. positive: every heap frame is reserved.
    for (uint64_t i = heap_lo; i < heap_hi; i++) {
        if (!(pmm.frame_bitmap[i / 8] & (1u << (i % 8)))) {
            return -2;
        }
    }
    // 2. vacuity: the reservation is BOUNDED — the frame just below the heap and
    // the frame at the (exclusive) heap end are still free.
    if (pmm.frame_bitmap[(heap_lo - 1) / 8] & (1u << ((heap_lo - 1) % 8))) {
        return -3;
    }
    if (heap_hi < total && (pmm.frame_bitmap[heap_hi / 8] & (1u << (heap_hi % 8)))) {
        return -4;
    }

    // 3. behavioral: point the rover at the first heap frame and allocate. The
    // allocator must step over the whole reserved run and return a frame outside
    // the heap; without the reservation it would return heap_lo itself.
    uint32_t base = pmm_get_free_frames();
    uint32_t saved_hint = pmm_alloc_hint;
    pmm_alloc_hint = (uint32_t)heap_lo;
    void *probe = pmm_alloc_frame();
    int ok = 0;
    if (probe) {
        uint64_t pf = (uint64_t)(uintptr_t)probe / PAGE_SIZE;
        ok = (pf < heap_lo || pf >= heap_hi);
        pmm_free_frame(probe);
    }
    pmm_alloc_hint = saved_hint;
    if (!ok || pmm_get_free_frames() != base) {
        return -5;
    }
    return 0;
}

// High-memory reachability proof (ADR-0021 PR-4). See memory.h. This is the
// load-bearing proof that dynamic sizing is SAFE above 128 MB: it is not enough
// that the allocator's bookkeeping counts a high frame free -- the frame must be
// reachable through the supervisor identity map (boot.S maps the full 1 GB) or a
// page table / ELF segment placed there faults on first touch. So the test WRITES
// two distinct sentinels through the returned identity pointer and reads them
// back. Guarded on RAM actually extending past 128 MB; the -m 128M ci-smoke leg
// takes the skip branch (which proves the guard ran, so the pass is not vacuous).
int pmm_highmem_selftest(void) {
    uint32_t total = pmm_get_total_frames();
    // A "high" frame sits at physical >= 128 MB (frame index >= 0x8000). With no
    // such frame there is nothing to prove; report the skip so the gate can see
    // the guard ran rather than a silent always-pass.
    if (total <= 0x8000) {
        boot_log("PMMHIGH: skipped (no RAM above 128 MB on this boot)");
        return 0;
    }

    uint32_t base = pmm_get_free_frames();
    uint32_t saved_hint = pmm_alloc_hint;
    // Aim the rover near the top so the allocation comes from high RAM AND, on the
    // 256M/512M legs, from the actual top of RAM (the strong differential proof).
    // Clamp to the first high frame (index 0x8000 = 128 MB): when total is only a
    // few frames above 0x8000, (total - 4) would fall BELOW 0x8000, the rover would
    // return a LOW frame, and the >= 128 MB assertion below would spuriously fail —
    // boot-panicking a legitimate memory size (total_frames in {0x8001,0x8002,0x8003}).
    pmm_alloc_hint = ((total - 4) < 0x8000) ? 0x8000 : (total - 4);
    void *frame = pmm_alloc_frame();
    pmm_alloc_hint = saved_hint;
    if (!frame) {
        return -1;
    }
    uint64_t phys = (uint64_t)(uintptr_t)frame;
    // Must be a genuinely high frame, and below the 1 GB identity ceiling (the
    // clamp guarantees the upper bound; assert it rather than trust it).
    if (phys < 0x8000000ULL || phys >= PMM_PHYS_CEIL) {
        pmm_free_frame(frame);
        return -2;
    }
    // The real proof: the identity map reaches this high physical frame. A boot.S
    // that mapped only 128 MB would #PF here; a wrong/stale map would read back
    // garbage. Two distinct sentinels rule out a stuck line aliasing a low frame.
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)phys;
    const uint64_t s1 = 0xA5A5C0DE5AA5F00DULL;
    const uint64_t s2 = 0x5A5A3F21A55A0FF2ULL;
    *p = s1;
    uint64_t r1 = *p;
    *p = s2;
    uint64_t r2 = *p;
    pmm_free_frame(frame);
    if (r1 != s1 || r2 != s2) {
        return -3;
    }
    if (pmm_get_free_frames() != base) {
        return -4;
    }
    boot_log("PMMHIGH: high-frame alloc + identity writeback verified");
    return 0;
}

// Residency-storm proof (ADR-0021 PR-4). See memory.h. The heap-reservation
// self-test proves ONE allocation aimed at the heap skips the reserved run; this
// proves the invariant holds under a bulk drain -- no frame in a large batch,
// pulled with the rover aimed straight at the heap, ever lands in the reserved
// [heap_lo, heap_hi). The pointer array is file-scope static (8 KB) because the
// 8 KB boot stack cannot hold it.
#define PMM_STORM_FRAMES 1024
static void *pmm_storm_batch[PMM_STORM_FRAMES];
int pmm_residency_storm_selftest(void) {
    uint64_t heap_lo = (uintptr_t)__heap_start / PAGE_SIZE;
    uint64_t heap_hi = ((uintptr_t)__heap_end + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t total = pmm_get_total_frames();
    if (heap_lo >= heap_hi || heap_hi > total) {
        return -1; // geometry sanity
    }
    uint32_t base = pmm_get_free_frames();
    // Keep the drain from exhausting the pmm (can't happen at any supported -m).
    if (base < PMM_STORM_FRAMES + 64) {
        return -2;
    }
    uint32_t saved_hint = pmm_alloc_hint;
    // Aim the rover straight at the heap so the very first allocation must skip
    // the reserved run.
    pmm_alloc_hint = (uint32_t)heap_lo;
    int n = 0;
    int bad = 0;
    for (; n < PMM_STORM_FRAMES; n++) {
        void *f = pmm_alloc_frame();
        if (!f) {
            bad = -3;
            break;
        }
        pmm_storm_batch[n] = f;
        uint64_t fn = (uint64_t)(uintptr_t)f / PAGE_SIZE;
        if (fn >= heap_lo && fn < heap_hi) {
            bad = -4; // a reserved heap frame was handed out under pressure
            n++;      // include it in the free loop below
            break;
        }
    }
    for (int i = 0; i < n; i++) {
        pmm_free_frame(pmm_storm_batch[i]);
    }
    pmm_alloc_hint = saved_hint;
    if (bad != 0) {
        return bad;
    }
    if (pmm_get_free_frames() != base) {
        return -5;
    }
    boot_log("PMMSTORM: heap survives a 1024-frame drain (no reserved frame handed out)");
    return 0;
}

// Virtual memory management
mem_result_t vmm_init(void) {
    boot_log("Initializing virtual memory manager...");

    // Allocate PML4 table
    vmm.pml4_table = (pml4e_t *)pmm_alloc_frame();
    if (!vmm.pml4_table) {
        return MEM_ERROR_OUT_OF_MEMORY;
    }

    // Clear PML4 table
    memset(vmm.pml4_table, 0, PAGE_SIZE);

    // Kernel heap setup is done by kheap_init() (called from memory_init)

    // Map kernel space
    // TODO: Map kernel code, data, and heap

    boot_log("Virtual memory manager initialized");
    return MEM_SUCCESS;
}

mem_result_t memory_map_page(void *virt_addr, void *phys_addr, uint32_t permissions) {
    // Extract indices from virtual address
    uint64_t pml4_index = ((uint64_t)virt_addr >> 39) & 0x1FF;
    uint64_t pdp_index = ((uint64_t)virt_addr >> 30) & 0x1FF;
    uint64_t pd_index = ((uint64_t)virt_addr >> 21) & 0x1FF;
    uint64_t pt_index = ((uint64_t)virt_addr >> 12) & 0x1FF;

    // Get PML4 entry
    pml4e_t *pml4_entry = &vmm.pml4_table[pml4_index];

    // Allocate PDPT if not present
    if (!pml4_entry->present) {
        pdpe_t *pdpt = (pdpe_t *)pmm_alloc_frame();
        if (!pdpt) {
            return MEM_ERROR_OUT_OF_MEMORY;
        }

        memset(pdpt, 0, PAGE_SIZE);

        pml4_entry->present = 1;
        pml4_entry->read_write = 1;
        pml4_entry->user = 0; // Kernel space
        pml4_entry->frame = (uint64_t)pdpt >> PAGE_SHIFT;
    }

    // Get PDPT entry
    pdpe_t *pdpt = (pdpe_t *)((uint64_t)pml4_entry->frame << PAGE_SHIFT);
    pdpe_t *pdp_entry = &pdpt[pdp_index];

    // Allocate PD if not present
    if (!pdp_entry->present) {
        pde_t *pd = (pde_t *)pmm_alloc_frame();
        if (!pd) {
            return MEM_ERROR_OUT_OF_MEMORY;
        }

        memset(pd, 0, PAGE_SIZE);

        pdp_entry->present = 1;
        pdp_entry->read_write = 1;
        pdp_entry->user = 0;
        pdp_entry->frame = (uint64_t)pd >> PAGE_SHIFT;
    }

    // Get PD entry
    pde_t *pd = (pde_t *)((uint64_t)pdp_entry->frame << PAGE_SHIFT);
    pde_t *pd_entry = &pd[pd_index];

    // Allocate PT if not present
    if (!pd_entry->present) {
        pte_t *pt = (pte_t *)pmm_alloc_frame();
        if (!pt) {
            return MEM_ERROR_OUT_OF_MEMORY;
        }

        memset(pt, 0, PAGE_SIZE);

        pd_entry->present = 1;
        pd_entry->read_write = 1;
        pd_entry->user = 0;
        pd_entry->frame = (uint64_t)pt >> PAGE_SHIFT;
    }

    // Get PT entry
    pte_t *pt = (pte_t *)((uint64_t)pd_entry->frame << PAGE_SHIFT);
    pte_t *pt_entry = &pt[pt_index];

    // Set page table entry
    pt_entry->present = 1;
    pt_entry->read_write = (permissions & MEM_WRITE) ? 1 : 0;
    pt_entry->user = (permissions & MEM_USER) ? 1 : 0;
    pt_entry->nx = (permissions & MEM_EXECUTE) ? 0 : 1;
    pt_entry->frame = (uint64_t)phys_addr >> PAGE_SHIFT;

    // Invalidate TLB
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");

    return MEM_SUCCESS;
}

mem_result_t memory_unmap_page(void *virt_addr) {
    // Extract indices from virtual address
    uint64_t pml4_index = ((uint64_t)virt_addr >> 39) & 0x1FF;
    uint64_t pdp_index = ((uint64_t)virt_addr >> 30) & 0x1FF;
    uint64_t pd_index = ((uint64_t)virt_addr >> 21) & 0x1FF;
    uint64_t pt_index = ((uint64_t)virt_addr >> 12) & 0x1FF;

    // Navigate to page table entry
    pml4e_t *pml4_entry = &vmm.pml4_table[pml4_index];
    if (!pml4_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }

    pdpe_t *pdpt = (pdpe_t *)((uint64_t)pml4_entry->frame << PAGE_SHIFT);
    pdpe_t *pdp_entry = &pdpt[pdp_index];
    if (!pdp_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }

    pde_t *pd = (pde_t *)((uint64_t)pdp_entry->frame << PAGE_SHIFT);
    pde_t *pd_entry = &pd[pd_index];
    if (!pd_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }

    pte_t *pt = (pte_t *)((uint64_t)pd_entry->frame << PAGE_SHIFT);
    pte_t *pt_entry = &pt[pt_index];
    if (!pt_entry->present) {
        return MEM_ERROR_INVALID_ADDRESS;
    }

    // Clear page table entry
    pt_entry->present = 0;
    pt_entry->frame = 0;

    // Invalidate TLB
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");

    return MEM_SUCCESS;
}

// ============================================================================
// Kernel heap — first-fit free-list allocator over the .heap region.
//
// Every block carries a header and blocks form an address-ordered doubly
// linked list. kmalloc splits a large free block; kfree marks a block
// free and coalesces with its immediate neighbours, so repeated
// alloc/free (e.g. kernel-thread stacks across service restarts) does
// not fragment the heap away.
// ============================================================================

#define KBLOCK_MAGIC 0x4B424C4Bu /* "KBLK" */

typedef struct kblock {
    size_t size;         /* payload size in bytes */
    struct kblock *next; /* next block in address order */
    struct kblock *prev;
    uint32_t magic;
    uint8_t free;
} kblock_t;

#define KHDR ALIGN_UP(sizeof(kblock_t), 16)

static kblock_t *heap_head = NULL;

/* Save RFLAGS + disable interrupts (nestable), and restore. The heap is
 * touched from concurrent kernel contexts (idle-loop reaper, health
 * monitor spawning threads), so allocations are made atomic. */
mem_result_t kheap_init(void) {
    boot_log("Initializing kernel heap...");

    // Use the .heap region from link.ld — identity-mapped by the boot
    // page tables. (KERNEL_HEAP_START is higher-half virtual space that
    // is not mapped yet; using it would fault on first kmalloc.)
    /* uintptr_t arithmetic: the two linker symbols are distinct C objects,
     * so direct pointer subtraction between them is UB. */
    size_t heap_size = (size_t)((uintptr_t)__heap_end - (uintptr_t)__heap_start);

    kernel_heap.start = (void *)__heap_start;
    kernel_heap.end = (void *)__heap_end;
    kernel_heap.current = kernel_heap.start;
    kernel_heap.total_size = heap_size;
    kernel_heap.used_size = 0;
    kernel_heap.free_size = heap_size;

    heap_head = (kblock_t *)__heap_start;
    heap_head->size = heap_size - KHDR;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_head->magic = KBLOCK_MAGIC;
    heap_head->free = 1;

    boot_log("Kernel heap initialized");
    return MEM_SUCCESS;
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size = ALIGN_UP(size, 16);

    uint64_t flags = irq_save();
    for (kblock_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < size) {
            continue;
        }
        /* Split if the leftover can hold a header plus a usable payload */
        if (b->size >= size + KHDR + 16) {
            kblock_t *nb = (kblock_t *)((uint8_t *)b + KHDR + size);
            nb->size = b->size - size - KHDR;
            nb->free = 1;
            nb->magic = KBLOCK_MAGIC;
            nb->prev = b;
            nb->next = b->next;
            if (b->next) {
                b->next->prev = nb;
            }
            b->next = nb;
            b->size = size;
        }
        b->free = 0;
        irq_restore(flags);
        return (uint8_t *)b + KHDR;
    }
    irq_restore(flags);
    return NULL; /* out of heap */
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }
    kblock_t *b = (kblock_t *)((uint8_t *)ptr - KHDR);
    if (b->magic != KBLOCK_MAGIC || b->free) {
        return; /* not ours / double free */
    }

    uint64_t flags = irq_save();
    b->free = 1;

    /* Coalesce with the following block if it is free (adjacent by
     * construction in address order) */
    if (b->next && b->next->free) {
        b->size += KHDR + b->next->size;
        b->next = b->next->next;
        if (b->next) {
            b->next->prev = b;
        }
    }
    /* Coalesce with the preceding block if it is free */
    if (b->prev && b->prev->free) {
        b->prev->size += KHDR + b->size;
        b->prev->next = b->next;
        if (b->next) {
            b->next->prev = b->prev;
        }
    }
    irq_restore(flags);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    kblock_t *b = (kblock_t *)((uint8_t *)ptr - KHDR);
    if (b->magic != KBLOCK_MAGIC) {
        return NULL;
    }
    if (b->size >= new_size) {
        return ptr; /* already big enough */
    }
    void *np = kmalloc(new_size);
    if (np) {
        memcpy(np, ptr, b->size);
        kfree(ptr);
    }
    return np;
}

size_t kheap_free_bytes(void) {
    size_t total = 0;
    uint64_t flags = irq_save();
    for (kblock_t *b = heap_head; b; b = b->next) {
        if (b->free) {
            total += b->size;
        }
    }
    irq_restore(flags);
    return total;
}

// Memory utilities
bool is_aligned(void *ptr, size_t alignment) {
    return ((uint64_t)ptr & (alignment - 1)) == 0;
}

bool is_user_address(void *addr) {
    return (uint64_t)addr < KERNEL_BASE_ADDR;
}

bool is_kernel_address(void *addr) {
    return (uint64_t)addr >= KERNEL_BASE_ADDR;
}

void *align_up(void *ptr, size_t alignment) {
    return (void *)ALIGN_UP((uint64_t)ptr, alignment);
}

void *align_down(void *ptr, size_t alignment) {
    return (void *)ALIGN_DOWN((uint64_t)ptr, alignment);
}

// Main memory initialization
mem_result_t memory_init(uint64_t total_memory) {
    boot_log("Initializing memory management...");

    // Physical memory manager, sized from the multiboot memory map by the caller
    // (ADR-0021). pmm_init clamps to the 1 GB identity-map ceiling.
    mem_result_t result = pmm_init(total_memory);
    if (result != MEM_SUCCESS) {
        return result;
    }

    // Initialize virtual memory manager
    result = vmm_init();
    if (result != MEM_SUCCESS) {
        return result;
    }

    // Initialize kernel heap
    result = kheap_init();
    if (result != MEM_SUCCESS) {
        return result;
    }

    boot_log("Memory management initialization complete");
    return MEM_SUCCESS;
}
