# QuantumOS Kernel Linker Script

ENTRY(_start)

/* Define memory regions */
MEMORY
{
    /* Kernel loaded at 1MB. Image is well under 1MB now that the IPC
     * entry pool is heap-allocated (was 96M to hold ~67MB of BSS). */
    kernel (rwx) : ORIGIN = 0x00100000, LENGTH = 16M
    /* Stack/heap directly after the kernel region; ends at 113MB so it
     * fits inside the 128MB the kernel assumes (memory_init). */
    ram (rwx)    : ORIGIN = 0x01100000, LENGTH = 96M
}

/* Sections */
SECTIONS
{
    /* Kernel starts at 1MB */
    . = 0x00100000;
    
    /* Multiboot header must be in first 8KB */
    .multiboot :
    {
        KEEP(*(.multiboot_header))
        . = ALIGN(4096);
    } > kernel

    /* PVH entry note (Firecracker / QEMU ELF64 boot); ld emits a PT_NOTE for it */
    .note.Xen :
    {
        KEEP(*(.note.Xen))
    } > kernel
    
    /* Code section */
    .text :
    {
        *(.text .text.*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4096);
    } > kernel
    
    /* Read-only data */
    .rodata :
    {
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4096);
    } > kernel
    
    /* Data section */
    .data :
    {
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(4096);
    } > kernel
    
    /* BSS section */
    .bss :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(4096);
    } > kernel
    
    /* Symbol for end of kernel */
    __end = .;
    
    /* Stack and heap: pure address-space reservations in the `ram` region,
     * defined as symbols only. They used to be (NOLOAD) output sections,
     * but ld still emits a PT_LOAD for an allocated NOLOAD section (filesz 0,
     * memsz 64 MB), and a Multiboot loader must then reserve that exact
     * physical range before it can hand over. GRUB on BIOS finds the range
     * free; GRUB on UEFI asks the firmware for it page by page, and with
     * firmware allocations scattered through 17-81 MB the request fails:
     * "error: out of memory" at the menu, kernel never entered (#264).
     * The kernel never needed the loader's help here — kheap_init writes
     * its own block header and pmm_init reserves the backing frames — so
     * the segment carried no content, only a demand on the loader. */
    __stack_top = ORIGIN(ram) + 8192;  /* 8KB stack */
    __heap_start = __stack_top;
    __heap_end = __heap_start + 64M;   /* 64MB heap */

    /* Discard sections we don't need */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note.gnu.build-id)
        *(.note.GNU-stack)
        *(.gnu_debuglink)
        *(.gnu_debugdata)
    }
}

/* Symbols for C code */
__kernel_start = 0x00100000;
__kernel_size = __end - __kernel_start;
__kernel_end = __end;

/* Stack symbols */
__stack_bottom = __stack_top - 8192;
__stack_pointer = __stack_top;

/* Heap symbols */
__heap_pointer = __heap_start;

/* Page table symbols */
__page_tables_start = __heap_end;
__page_tables_end = __page_tables_start + 1M;  /* 1MB for page tables */

/* Frame buffer symbols (if needed) */
__framebuffer_start = 0xB8000;
__framebuffer_end = 0xB8FA0;
