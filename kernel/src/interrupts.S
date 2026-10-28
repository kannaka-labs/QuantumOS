# QuantumOS Interrupt Handling Assembly

.section .text

# Common interrupt handler stub
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    push $0          # Push dummy error code
    push $\num       # Push interrupt number
    jmp isr_common
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    push $\num       # Push interrupt number
    jmp isr_common
.endm

# Exception handlers (0-31)
ISR_NOERRCODE 0     # Divide error
ISR_NOERRCODE 1     # Debug
ISR_NOERRCODE 2     # Non-maskable interrupt
ISR_NOERRCODE 3     # Breakpoint
ISR_NOERRCODE 4     # Overflow
ISR_NOERRCODE 5     # Bound range exceeded
ISR_NOERRCODE 6     # Invalid opcode
ISR_NOERRCODE 7     # Device not available
ISR_ERRCODE   8     # Double fault
ISR_NOERRCODE 9     # Reserved
ISR_ERRCODE   10    # Invalid TSS
ISR_ERRCODE   11    # Segment not present
ISR_ERRCODE   12    # Stack segment fault
ISR_ERRCODE   13    # General protection fault
ISR_ERRCODE   14    # Page fault
ISR_NOERRCODE 15    # Reserved
ISR_NOERRCODE 16    # x87 FPU error
ISR_ERRCODE   17    # Alignment check
ISR_NOERRCODE 18    # Machine check
ISR_NOERRCODE 19    # SIMD FP exception
ISR_NOERRCODE 20    # Virtualization
ISR_NOERRCODE 21    # Reserved
ISR_NOERRCODE 22    # Reserved
ISR_NOERRCODE 23    # Reserved
ISR_NOERRCODE 24    # Reserved
ISR_NOERRCODE 25    # Reserved
ISR_NOERRCODE 26    # Reserved
ISR_NOERRCODE 27    # Reserved
ISR_NOERRCODE 28    # Reserved
ISR_NOERRCODE 29    # Reserved
ISR_ERRCODE   30    # Security exception
ISR_NOERRCODE 31    # Reserved

# System call entry (int 0x80, gate installed with DPL 3)
ISR_NOERRCODE 128

# MSI-X vectors (#253): the block MSIX_VECTOR_BASE..+3 in pci_msix.h, one
# stub each, dispatched through interrupt_handlers[] like a software vector.
ISR_NOERRCODE 64
ISR_NOERRCODE 65
ISR_NOERRCODE 66
ISR_NOERRCODE 67

# Local APIC spurious vector (#253): set in SVR when the APIC is enabled
# for MSI-X; must have a gate so a spurious interrupt is a no-op, not a #GP.
ISR_NOERRCODE 255

# IRQ handlers (32-47)
.macro IRQ num
.global irq\num
irq\num:
    push $0          # Push dummy error code
    push $(32 + \num) # Push interrupt number
    jmp irq_common
.endm

IRQ 0   # Timer
IRQ 1   # Keyboard
IRQ 2   # Cascade
IRQ 3   # COM2
IRQ 4   # COM1
IRQ 5   # LPT2
IRQ 6   # Floppy
IRQ 7   # LPT1
IRQ 8   # CMOS RTC
IRQ 9   # Free
IRQ 10  # Free
IRQ 11  # Free
IRQ 12  # Mouse
IRQ 13  # Math coprocessor
IRQ 14  # Primary ATA
IRQ 15  # Secondary ATA

# Common interrupt handler for exceptions
isr_common:
    # Save all registers (order matches cpu_state_t: memory low->high is
    # r15..r8, rdi, rsi, rbp, rbx, rdx, rcx, rax)
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    
    # Save data segment
    mov %ds, %ax
    push %rax
    
    # Load kernel data segment
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    
    # Call C handler
    mov %rsp, %rdi    # Pass CPU state pointer
    add $8, %rdi      # Skip saved DS
    call interrupt_handler
    
    # Restore data segment
    pop %rax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    
    # Restore all registers
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    
    # Clean up error code and interrupt number
    add $16, %rsp
    
    # Return from interrupt
    iretq

# Common interrupt handler for IRQs
irq_common:
    # Save all registers (order matches cpu_state_t: memory low->high is
    # r15..r8, rdi, rsi, rbp, rbx, rdx, rcx, rax)
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    
    # Save data segment
    mov %ds, %ax
    push %rax
    
    # Load kernel data segment
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    
    # Call C handler
    mov %rsp, %rdi    # Pass CPU state pointer
    add $8, %rdi      # Skip saved DS
    call interrupt_handler
    
    # Restore data segment
    pop %rax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    
    # Restore all registers
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    
    # Clean up error code and interrupt number
    add $16, %rsp
    
    # Return from interrupt
    iretq

# Enable interrupts
.global enable_interrupts
enable_interrupts:
    sti
    ret

# Disable interrupts
.global disable_interrupts
disable_interrupts:
    cli
    ret

# Halt CPU
.global halt_cpu
halt_cpu:
    hlt
    ret

# Load GDT
.global load_gdt
load_gdt:
    lgdt (%rdi)
    ret

# Load TSS
.global load_tss
load_tss:
    ltr %di
    ret

# Get current CR2 (page fault address)
.global get_cr2
get_cr2:
    mov %cr2, %rax
    ret

# Get current CR3 (page table base)
.global get_cr3
get_cr3:
    mov %cr3, %rax
    ret

# Set CR3
.global set_cr3
set_cr3:
    mov %rdi, %cr3
    ret

# Invalidate TLB entry
.global invlpg
invlpg:
    invlpg (%rdi)
    ret

# Get CPU flags
.global get_flags
get_flags:
    pushf
    pop %rax
    ret

# Port I/O functions
.global inb
inb:
    mov %dx, %ax
    in %dx, %al
    ret

.global outb
outb:
    mov %dx, %ax
    out %al, %dx
    ret

.global inw
inw:
    mov %dx, %ax
    in %dx, %ax
    ret

.global outw
outw:
    mov %dx, %ax
    out %ax, %dx
    ret

.global inl
inl:
    mov %dx, %ax
    in %dx, %eax
    ret

.global outl
outl:
    mov %dx, %ax
    out %eax, %dx
    ret

# CPUID instruction
.global cpuid
cpuid:
    mov %eax, %eax
    cpuid
    ret

# Read MSR
.global rdmsr
rdmsr:
    mov %ecx, %ecx
    rdmsr
    ret

# Write MSR
.global wrmsr
wrmsr:
    mov %ecx, %ecx
    mov %edx, %edx
    mov %eax, %eax
    wrmsr
    ret

# Read control registers
.global read_cr0
read_cr0:
    mov %cr0, %rax
    ret

.global read_cr4
read_cr4:
    mov %cr4, %rax
    ret

# Write control registers
.global write_cr0
write_cr0:
    mov %rdi, %cr0
    ret

.global write_cr4
write_cr4:
    mov %rdi, %cr4
    ret

# Non-executable stack marker
.section .note.GNU-stack, "", @progbits
