# QuantumOS Kernel Bootstrap (x86_64)
#
# Multiboot v1 loaders (GRUB "multiboot", QEMU -kernel) enter at _start in
# 32-bit protected mode with paging off, EAX = 0x2BADB002 and EBX = the
# physical address of the multiboot info structure. This stub saves those,
# clears BSS, identity-maps the first 1 GB with 2 MB pages, switches to
# long mode, enables SSE (the resonance subsystem is compiled with -msse),
# and calls kernel_main(magic, info_addr).

/* Boot/idle stack for kernel_main and everything kernel_init calls. It was
 * 16 KB, and the init chain (core_services_init -> service_selftest ->
 * service_list, with a 32-entry service_info_t array as a local) runs about
 * 23 KB deep: the overflow landed in the page tables that used to sit just
 * below it, silently zeroing fb_map_pd[0] and boot_pd[187..] on every boot.
 * Invisible on BIOS/-kernel boots (VGA text never uses fb_map_pd; a 128 MB
 * machine never touches boot_pd entries above 374 MB); fatal under UEFI,
 * whose framebuffer at 2 GB maps through fb_map_pd[0] (#264). 64 KB now, the
 * tables live ABOVE the stack (see .bss below), and main.c reports the
 * high-water mark and checks the identity map at the end of init. */
#define BOOT_STACK_SIZE 65536

#define MB1_MAGIC 0x1BADB002
#ifdef MB1_TEXT_ONLY
/* Console-image build (epic #101): no video request, so GRUB leaves the
 * machine in VGA text mode and the kernel takes the screen-console path.
 * GRUB honours the header's video request over gfxpayload (verified
 * empirically: gfxpayload=text/keep still produced a linear FB), so a
 * separate image is the only reliable way to get a text-mode GRUB boot. */
#define MB1_FLAGS 0x00000003
#else
/* bit0 page-align modules, bit1 memory info, bit2 request video mode */
#define MB1_FLAGS 0x00000007
#endif

/* Multiboot v1 header — must be in the first 8 KB of the image; link.ld
 * places .multiboot_header first. With the video bit set we also supply
 * the address fields (zero; we are ELF so they are ignored) and the
 * preferred graphics mode. The loader gives us a linear framebuffer if
 * it can, and reports it back in the multiboot info; the kernel falls
 * back to VGA text if no usable framebuffer is provided. */
.section .multiboot_header, "a"
.align 4
/* Exported so the kernel can read back what THIS image asked for (#264):
 * a framebuffer arriving although bit 2 was never set means the firmware
 * had no text mode to give (UEFI/GOP), and the screen console must render
 * on the framebuffer instead of the VGA text buffer. */
.global mb1_header
mb1_header:
    .long MB1_MAGIC
    .long MB1_FLAGS
    .long -(MB1_MAGIC + MB1_FLAGS)
    .long 0        /* header_addr   (unused: ELF) */
    .long 0        /* load_addr */
    .long 0        /* load_end_addr */
    .long 0        /* bss_end_addr */
    .long 0        /* entry_addr */
    .long 0        /* mode_type: 0 = linear graphics */
    .long 1024     /* width  (preferred) */
    .long 768      /* height */
    .long 32       /* depth  (bpp) */


/* PVH boot (Firecracker, QEMU -kernel with an ELF64): the loader enters at the
 * address in this ELF note in 32-bit protected mode, paging off, flat segments,
 * EBX = physical address of hvm_start_info -- the same machine state the
 * Multiboot1 path expects, minus the info block. _pvh_start builds a Multiboot1
 * info block (memory map, cmdline, mem_lower/mem_upper) from hvm_start_info and
 * jumps to _start, so the kernel stays Multiboot1-only (ADR-0021). */
.section .note.Xen, "a", @note
.align 4
    .long 4                 /* namesz ("Xen\0") */
    .long 4                 /* descsz */
    .long 18                /* XEN_ELFNOTE_PHYS32_ENTRY */
    .asciz "Xen"
    .align 4
    .long _pvh_start        /* desc: 32-bit physical entry */
.align 4

.section .text
.global _start
.extern kernel_main

# ============================================================================
# 32-bit entry (protected mode, flat segments, paging off)
# ============================================================================
.code32

# ============================================================================
# PVH entry (32-bit, paging off, EBX = hvm_start_info). Synthesizes a
# Multiboot1 info block in .data (survives the BSS clear) and joins _start.
# hvm_start_info v1: magic +0 (0x336ec578), cmdline_paddr +24 (u64),
# memmap_paddr +40 (u64), memmap_entries +48. Memmap entry = {addr u64,
# size u64, type u32, reserved u32} (24 bytes); RAM = type 1, same numbering
# as Multiboot1 for types 1-4.
# ============================================================================
.global _pvh_start
_pvh_start:
    cli
    movl $boot_stack_top, %esp
    cmpl $0x336ec578, (%ebx)
    jne .pvh_bad

    /* zero the synthetic info block (128 bytes) */
    pushl %ebx
    movl $pvh_mbinfo, %edi
    movl $32, %ecx
    xorl %eax, %eax
    rep stosl
    popl %ebx

    movl $0x45, pvh_mbinfo              /* flags: mem | cmdline | mmap */
    movl 24(%ebx), %eax                 /* cmdline_paddr (low 32) */
    movl %eax, pvh_mbinfo+16

    movl 40(%ebx), %esi                 /* memmap_paddr (low 32) */
    movl 48(%ebx), %ecx                 /* memmap_entries */
    movl $pvh_mmap, %edi
    movl %edi, pvh_mbinfo+48            /* mmap_addr */
    xorl %edx, %edx                     /* usable RAM in KB */
    cmpl $32, %ecx
    jbe 1f
    movl $32, %ecx
1:  testl %ecx, %ecx
    jz .pvh_mmap_done
.pvh_mmap_loop:
    movl $20, (%edi)                    /* MB1 entry size (excludes itself) */
    movl 0(%esi), %eax
    movl %eax, 4(%edi)                  /* base lo */
    movl 4(%esi), %eax
    movl %eax, 8(%edi)                  /* base hi */
    movl 8(%esi), %eax
    movl %eax, 12(%edi)                 /* len lo */
    movl 12(%esi), %eax
    movl %eax, 16(%edi)                 /* len hi */
    movl 16(%esi), %eax                 /* type */
    cmpl $4, %eax
    jbe 2f
    movl $2, %eax                       /* anything exotic -> reserved */
2:  movl %eax, 20(%edi)
    cmpl $1, %eax
    jne 3f
    movl 8(%esi), %eax
    shrl $10, %eax
    addl %eax, %edx
    movl 12(%esi), %eax
    shll $22, %eax
    addl %eax, %edx
3:  addl $24, %esi
    addl $24, %edi
    loop .pvh_mmap_loop
.pvh_mmap_done:
    movl %edi, %eax
    subl $pvh_mmap, %eax
    movl %eax, pvh_mbinfo+44            /* mmap_length */
    movl $640, pvh_mbinfo+4             /* mem_lower KB */
    movl %edx, %eax
    subl $1024, %eax
    jae 4f
    xorl %eax, %eax
4:  movl %eax, pvh_mbinfo+8             /* mem_upper KB (fallback only; mmap wins) */

    movl $0x2BADB002, %eax
    movl $pvh_mbinfo, %ebx
    jmp _start
.pvh_bad:
    hlt
    jmp .pvh_bad

_start:
    cli
    movl $boot_stack_top, %esp

    /* Save multiboot registers to .data (survives the BSS clear) */
    movl %eax, multiboot_magic
    movl %ebx, multiboot_info

    /* Clear BSS now — the boot page tables live there */
    movl $__bss_start, %edi
    movl $__bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    /* Identity-map the first 1 GB: PML4[0] -> PDPT[0] -> PD with 512
     * 2 MB entries. Non-zero entries only at index 0; the rest are zero
     * from the BSS clear. */
    movl $boot_pdpt, %eax
    orl $0x3, %eax                  /* present | writable */
    movl %eax, boot_pml4

    movl $boot_pd, %eax
    orl $0x3, %eax
    movl %eax, boot_pdpt

    movl $boot_pd, %edi
    movl $0x83, %eax                /* present | writable | 2MB page */
    movl $512, %ecx
.fill_pd:
    movl %eax, (%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd

    /* Enable PAE */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    /* Load the page tables */
    movl $boot_pml4, %eax
    movl %eax, %cr3

    /* Enable long mode (EFER.LME) */
    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    /* Enable paging — CPU enters compatibility mode */
    movl %cr0, %eax
    orl $0x80000001, %eax           /* PG | PE */
    movl %eax, %cr0

    /* Jump into 64-bit code via the 64-bit GDT */
    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_entry

# ============================================================================
# 64-bit entry
# ============================================================================
.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movq $boot_stack_top, %rsp
    movq %rsp, %rbp

    # Enable SSE — the resonance subsystem is compiled with -msse/-msse2
    # (double-precision math), so XMM instructions must be legal before
    # kernel_main runs.
    mov %cr0, %rax
    and $~(1 << 2), %rax           # clear CR0.EM (no x87 emulation)
    or $(1 << 1), %rax             # set CR0.MP (monitor coprocessor)
    mov %rax, %cr0
    mov %cr4, %rax
    or $(3 << 9), %rax             # set CR4.OSFXSR | CR4.OSXMMEXCPT
    mov %rax, %cr4

    # kernel_main(uint32_t magic, uint32_t info_addr)
    movl multiboot_magic, %edi
    movl multiboot_info, %esi
    call kernel_main

    # Should never return
    cli
.halt:
    hlt
    jmp .halt

# Boot panic routine — C ABI: message in %rdi
.global boot_panic
boot_panic:
    push %rbx         # keep message across calls (also 16-aligns %rsp)
    mov %rdi, %rbx

    # Print panic message
    call early_console_write

    # Print newline
    mov $panic_newline, %rdi
    call early_console_write

    # Paint the on-screen panic banner (vga.c) — on a machine with no
    # serial port this is the only visible trace of an early failure
    mov %rbx, %rdi
    call vga_panic_banner

    # Halt system
    cli
.panic_loop:
    hlt
    jmp .panic_loop

# Early console output — C ABI: string in %rdi
# Interrupts are disabled for the duration so strings stay atomic on the
# serial line (timer-tick logs would otherwise interleave mid-string).
.global early_console_write
early_console_write:
    pushfq
    cli
    push %rax
    push %rdi
    push %rsi
    push %rcx
    push %rdx

    mov %rdi, %rsi    # keep string pointer; strlen takes %rdi
    call strlen
    mov %rax, %rcx    # length
    test %rcx, %rcx
    jz .write_done

    # Write to COM1 (0x3F8), waiting for the transmit holding register
    # to drain before each byte (LSR bit 5) so bytes are not dropped
.write_loop:
    mov $0x3FD, %dx   # Line Status Register
.wait_thr:
    in %dx, %al
    test $0x20, %al   # THR empty?
    jz .wait_thr

    mov $0x3F8, %dx
    mov (%rsi), %al
    out %al, %dx
    inc %rsi
    loop .write_loop

.write_done:
    pop %rdx
    pop %rcx
    pop %rsi
    pop %rdi
    pop %rax
    popfq             # restore caller's IF
    ret

# Write hex value
.global early_console_write_hex
early_console_write_hex:
    push %rax
    push %rdi
    push %rsi
    push %rdx
    push %rcx

    mov %rdi, %rax    # value to print
    mov $hex_buffer, %rsi
    mov $16, %ecx     # 16 hex digits

.hex_loop:
    rol $4, %rax      # rotate left 4 bits
    mov %rax, %rdx
    and $0xF, %rdx    # get low nibble

    cmp $10, %rdx
    jl .digit
    add $('A' - 10), %rdx
    jmp .write_digit
.digit:
    add $'0', %rdx

.write_digit:
    mov %dl, (%rsi)
    inc %rsi
    loop .hex_loop

    # Null terminate
    movb $0, (%rsi)

    # Print the hex string
    mov $hex_buffer, %rdi
    call early_console_write

    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax
    ret

# ============================================================================
# Data
# ============================================================================
.section .data
multiboot_magic: .quad 0
multiboot_info: .quad 0
panic_newline: .asciz "\r\n*** KERNEL PANIC ***\r\n"
hex_buffer: .space 17  # 16 hex digits + null

# 64-bit GDT: null, code (L=1), data. The pointer layout works for both
# the 32-bit lgdt (word + dword) and 64-bit reloads (word + qword) since
# the GDT sits below 4 GB.
.align 8
gdt64:
    .quad 0
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

# ============================================================================
# BSS: boot stack and boot page tables
# ============================================================================
.section .bss
.align 4096
/* The stack comes FIRST (lowest address): a stack grows down, so an
 * overflow runs into whatever precedes it, and that must never be a page
 * table again (#264). The bottom 4 KB is a guard zone main.c checks. */
.global boot_stack
.global boot_stack_top
boot_stack:
    .space BOOT_STACK_SIZE
boot_stack_top:
.align 4096
.global boot_pml4
.global boot_pdpt
.global boot_pd
.global fb_map_pd
boot_pml4:
    .space 4096
boot_pdpt:
    .space 4096
boot_pd:
    .space 4096
/* Spare page directory used to identity-map a high framebuffer region
 * (set up at runtime by fb_init when a linear framebuffer is present) */
fb_map_pd:
    .space 4096

# Non-executable stack marker
.section .note.GNU-stack, "", @progbits

/* PVH: synthetic Multiboot1 info block + memory map (in .data: the BSS clear in
 * _start must not wipe them). 32 entries is far more than any microVM reports. */
.section .data
.align 8
.global pvh_mbinfo
pvh_mbinfo:
    .fill 128, 1, 0
.global pvh_mmap
pvh_mmap:
    .fill 32*24, 1, 0
