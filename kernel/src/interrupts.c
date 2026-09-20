#include <kernel/types.h>
#include <kernel/interrupts.h>
#include <kernel/boot.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/console.h>
#include <kernel/netdev.h>

// Forward declarations for I/O port functions
static inline void __outb(uint16_t port, uint8_t value);
static inline uint8_t __inb(uint16_t port);

// IDT table
static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t idt_ptr;

// Interrupt handlers
static interrupt_handler_info_t interrupt_handlers[IDT_ENTRIES];

// Statistics
static uint64_t interrupt_counts[IDT_ENTRIES];
static uint64_t total_interrupts;

// External assembly handlers
extern void isr0(void);  // Divide error
extern void isr1(void);  // Debug
extern void isr2(void);  // Non-maskable interrupt
extern void isr3(void);  // Breakpoint
extern void isr4(void);  // Overflow
extern void isr5(void);  // Bound range exceeded
extern void isr6(void);  // Invalid opcode
extern void isr7(void);  // Device not available
extern void isr8(void);  // Double fault
extern void isr10(void); // Invalid TSS
extern void isr11(void); // Segment not present
extern void isr12(void); // Stack segment fault
extern void isr13(void); // General protection fault
extern void isr14(void); // Page fault
extern void isr16(void); // x87 FPU error
extern void isr17(void); // Alignment check
extern void isr18(void); // Machine check
extern void isr19(void); // SIMD FP exception
extern void isr20(void); // Virtualization
extern void isr30(void); // Security exception
extern void isr31(void); // Reserved

extern void irq0(void);  // Timer
extern void irq1(void);  // Keyboard
extern void irq2(void);  // Cascade
extern void irq3(void);  // COM2
extern void irq4(void);  // COM1
extern void irq5(void);  // LPT2
extern void irq6(void);  // Floppy
extern void irq7(void);  // LPT1
extern void irq8(void);  // CMOS RTC
extern void irq9(void);  // Free
extern void irq10(void); // Free
extern void irq11(void); // Free
extern void irq12(void); // Mouse
extern void irq13(void); // Math coprocessor
extern void irq14(void); // Primary ATA
extern void irq15(void); // Secondary ATA

// Exception handler array
static void (*exception_handlers[32])(cpu_state_t *state) = {
    divide_error_handler,             // 0
    NULL,                             // 1 (debug)
    NULL,                             // 2 (NMI)
    NULL,                             // 3 (breakpoint)
    NULL,                             // 4 (overflow)
    NULL,                             // 5 (bound range)
    NULL,                             // 6 (invalid opcode)
    NULL,                             // 7 (device not available)
    double_fault_handler,             // 8
    NULL,                             // 9 (reserved)
    NULL,                             // 10 (invalid TSS)
    NULL,                             // 11 (segment not present)
    NULL,                             // 12 (stack segment fault)
    general_protection_fault_handler, // 13
    page_fault_handler,               // 14
    NULL,                             // 15 (reserved)
    NULL,                             // 16 (x87 FPU error)
    NULL,                             // 17 (alignment check)
    NULL,                             // 18 (machine check)
    NULL,                             // 19 (SIMD FP exception)
    NULL,                             // 20 (virtualization)
    NULL,                             // 21-29 (reserved)
    NULL,                             // 30 (security)
    NULL                              // 31 (reserved)
};

// Initialize interrupt system
irq_result_t interrupts_init(void) {
    boot_log("Initializing interrupt system...");

    // Clear IDT
    memset(&idt, 0, sizeof(idt));

    // Setup exception handlers
    idt_set_gate(EXC_DIVIDE_ERROR, (uint64_t)isr0, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_DEBUG, (uint64_t)isr1, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_NMI, (uint64_t)isr2, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_BREAKPOINT, (uint64_t)isr3, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_OVERFLOW, (uint64_t)isr4, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_BOUND_RANGE, (uint64_t)isr5, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_INVALID_OPCODE, (uint64_t)isr6, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_DEVICE_NOT_AVAILABLE, (uint64_t)isr7, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_DOUBLE_FAULT, (uint64_t)isr8, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_INVALID_TSS, (uint64_t)isr10, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_SEGMENT_NOT_PRESENT, (uint64_t)isr11, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_STACK_SEGMENT_FAULT, (uint64_t)isr12, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_GENERAL_PROTECTION, (uint64_t)isr13, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_PAGE_FAULT, (uint64_t)isr14, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_X87_FPU_ERROR, (uint64_t)isr16, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_ALIGNMENT_CHECK, (uint64_t)isr17, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_MACHINE_CHECK, (uint64_t)isr18, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_SIMD_FP_EXCEPTION, (uint64_t)isr19, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_VIRTUALIZATION, (uint64_t)isr20, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_SECURITY, (uint64_t)isr30, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    idt_set_gate(EXC_RESERVED, (uint64_t)isr31, 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);

    // Setup IRQ handlers (each stub is a distinct symbol — pointer
    // arithmetic on &irq0 lands inside irq0's code, not on the stubs)
    static void (*const irq_stubs[16])(void) = {irq0,  irq1,  irq2,  irq3, irq4,  irq5,
                                                irq6,  irq7,  irq8,  irq9, irq10, irq11,
                                                irq12, irq13, irq14, irq15};
    for (int i = 0; i < 16; i++) {
        idt_set_gate(IRQ_BASE + i, (uint64_t)irq_stubs[i], 0x08, GATE_TYPE_INTERRUPT | DPL_KERNEL);
    }

    // Install IDT
    idt_install();

    // Initialize PIC
    pic_init();

    // Clear interrupt statistics
    memset(interrupt_counts, 0, sizeof(interrupt_counts));
    total_interrupts = 0;

    boot_log("Interrupt system initialized");
    return IRQ_SUCCESS;
}

// Set IDT gate
void idt_set_gate(uint8_t vector, uint64_t handler_addr, uint16_t selector, uint8_t type_attr) {
    idt_entry_t *entry = &idt[vector];

    entry->offset_low = handler_addr & 0xFFFF;
    entry->selector = selector;
    entry->ist = 0;
    entry->type_attr = type_attr | GATE_PRESENT; // a registered gate is always present
    entry->offset_mid = (handler_addr >> 16) & 0xFFFF;
    entry->offset_high = (handler_addr >> 32) & 0xFFFFFFFF;
    entry->reserved = 0;
}

// Install IDT
void idt_install(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;

    load_idt(&idt_ptr);
}

// Load IDT (assembly function)
void load_idt(idt_ptr_t *idtp) {
    __asm__ volatile("lidt (%0)" : : "r"(idtp));
}

// Register interrupt handler
// Note: vector is uint8_t (0-255), which covers all valid IDT entries
irq_result_t interrupt_register(uint8_t vector, interrupt_handler_t handler, void *context) {
    if (interrupt_handlers[vector].handler != NULL) {
        return IRQ_ERROR_ALREADY_REGISTERED;
    }

    interrupt_handlers[vector].handler = handler;
    interrupt_handlers[vector].context = context;
    interrupt_handlers[vector].flags = 0;

    return IRQ_SUCCESS;
}

// Unregister interrupt handler
// Note: vector is uint8_t (0-255), which covers all valid IDT entries
irq_result_t interrupt_unregister(uint8_t vector) {
    interrupt_handlers[vector].handler = NULL;
    interrupt_handlers[vector].context = NULL;
    interrupt_handlers[vector].flags = 0;

    return IRQ_SUCCESS;
}

// Enable interrupt
irq_result_t interrupt_enable(uint8_t vector) {
    if (vector >= IRQ_BASE) {
        pic_unmask_irq(vector - IRQ_BASE);
    }

    return IRQ_SUCCESS;
}

// Disable interrupt
irq_result_t interrupt_disable(uint8_t vector) {
    if (vector >= IRQ_BASE) {
        pic_mask_irq(vector - IRQ_BASE);
    }

    return IRQ_SUCCESS;
}

// Enable all interrupts
irq_result_t interrupt_enable_all(void) {
    __asm__ volatile("sti");
    return IRQ_SUCCESS;
}

// Disable all interrupts
irq_result_t interrupt_disable_all(void) {
    __asm__ volatile("cli");
    return IRQ_SUCCESS;
}

// Common interrupt handler
void interrupt_handler(cpu_state_t *state) {
    uint8_t vector = state->int_no;

    // Update statistics
    interrupt_counts[vector]++;
    total_interrupts++;

    // Handle exceptions
    if (vector < 32) {
        if (exception_handlers[vector] != NULL) {
            exception_handlers[vector](state);
        } else {
            boot_log("Unhandled exception");
            dump_cpu_state(state);
            boot_panic("Unhandled exception");
        }
        return;
    }

    // Handle IRQs
    if (vector >= IRQ_BASE && vector < IRQ_BASE + 16) {
        irq_handler(state);
        return;
    }

    // Handle software interrupts
    if (interrupt_handlers[vector].handler != NULL) {
        interrupt_handlers[vector].handler(state);
    } else {
        boot_log("Unhandled interrupt");
        dump_cpu_state(state);
    }
}

/**
 * Contain a fault raised from ring 3: kill the offending process and
 * reschedule instead of panicking the kernel. Returns 1 if the fault
 * was contained (caller should return), 0 if it came from ring 0.
 */
static int contain_user_fault(cpu_state_t *state, const char *what) {
    if ((state->cs & 3) == 0) {
        return 0; /* kernel fault — fatal */
    }

    process_t *cur = process_get_current();
    boot_log("FAULT CONTAINED: ring-3 fault, killing offending process:");
    boot_log(what);
    if (cur) {
        boot_log(cur->name);
        process_set_state(cur->pid, PROCESS_STATE_TERMINATED);
    }
    scheduler_kill_current(state);
    return 1;
}

// Exception handlers
void divide_error_handler(cpu_state_t *state) {
    if (contain_user_fault(state, "divide error")) {
        return;
    }
    boot_log("Divide by zero exception");
    dump_cpu_state(state);
    boot_panic("Divide by zero");
}

void page_fault_handler(cpu_state_t *state) {
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

    if (contain_user_fault(state, "page fault")) {
        return;
    }

    boot_log("Page fault at address: ");
    early_console_write_hex(fault_addr);
    boot_log("Error code: ");
    early_console_write_hex(state->err_code);

    dump_cpu_state(state);
    boot_panic("Page fault");
}

void general_protection_fault_handler(cpu_state_t *state) {
    if (contain_user_fault(state, "general protection fault")) {
        return;
    }
    boot_log("General protection fault");
    boot_log("Error code: ");
    early_console_write_hex(state->err_code);

    dump_cpu_state(state);
    boot_panic("General protection fault");
}

void double_fault_handler(cpu_state_t *state) {
    boot_log("Double fault");
    dump_cpu_state(state);
    boot_panic("Double fault");
}

// IRQ handler
void irq_handler(cpu_state_t *state) {
    uint8_t irq = state->int_no - IRQ_BASE;

    // Call specific IRQ handler
    switch (irq) {
    case IRQ_TIMER:
        timer_irq_handler(state);
        break;
    case IRQ_KEYBOARD:
        keyboard_irq_handler(state);
        break;
    case IRQ_COM1:
        /* Console serial input: drain received bytes into the input ring */
        console_com1_irq();
        break;
    default:
        /* The NIC's IRQ line is assigned dynamically (by PCI for the
         * rtl8139, by the cmdline for virtio-mmio), so it can't be a
         * compile-time case. Route it when it matches the bound device. */
        if (netdev_present() && irq == netdev_irq_line()) {
            netdev_irq();
        } else {
            boot_log("Unhandled IRQ: ");
            early_console_write_hex(irq);
        }
        break;
    }

    // Send EOI to PIC
    pic_send_eoi(irq);
}

// Timer state
static volatile uint64_t timer_ticks_count = 0;
static timer_callback_t timer_callback = NULL;

uint64_t timer_get_ticks(void) {
    return timer_ticks_count;
}

void timer_set_callback(timer_callback_t callback) {
    timer_callback = callback;
}

// Timer IRQ handler
void timer_irq_handler(cpu_state_t *state) {
    timer_ticks_count++;

    /* Proof-of-life on first tick, then once per second (at 100 Hz). The
     * periodic heartbeat is silenced under a `quiet` boot so it can't bury
     * the interactive shell on the shared console; the one-time first-tick
     * line always prints. */
    if (timer_ticks_count == 1) {
        boot_log("Timer tick 1 received — interrupts are live");
    } else if (!boot_is_quiet() && timer_ticks_count % TIMER_DEFAULT_HZ == 0) {
        boot_log("Timer tick: ");
        early_console_write_hex(timer_ticks_count);
    }

    /* Scheduler (or other subscriber) hook */
    if (timer_callback) {
        timer_callback(state);
    }
}

// Keyboard IRQ handler: translate the scancode and feed the console
// input ring (console.c owns the PS/2 state machine and reads 0x60).
void keyboard_irq_handler(cpu_state_t *state) {
    (void)state;
    console_kbd_irq();
}

// PIC initialization
void pic_init(void) {
    // Start initialization sequence
    __outb(0x20, 0x11); // ICW1: init + ICW4
    __outb(0xA0, 0x11);

    // ICW2: vector offset
    __outb(0x21, IRQ_BASE);     // Master PIC vector offset
    __outb(0xA1, IRQ_BASE + 8); // Slave PIC vector offset

    // ICW3: cascade/hierarchy
    __outb(0x21, 0x04); // Master PIC: IRQ2 is cascade
    __outb(0xA1, 0x02); // Slave PIC: cascade identity

    // ICW4: mode
    __outb(0x21, 0x01); // 8086 mode
    __outb(0xA1, 0x01);

    // Mask everything except the cascade line; individual IRQs are
    // unmasked explicitly via interrupt_enable() when a handler is ready
    __outb(0x21, 0xFB); // Master: only IRQ2 (cascade) unmasked
    __outb(0xA1, 0xFF); // Slave: all masked
}

// Program the PIT (channel 0, mode 2 rate generator) for a periodic tick
void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) {
        frequency_hz = TIMER_DEFAULT_HZ;
    }

    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    __outb(0x43, 0x34);                             // channel 0, lobyte/hibyte, mode 2
    __outb(0x40, (uint8_t)(divisor & 0xFF));        // divisor low byte
    __outb(0x40, (uint8_t)((divisor >> 8) & 0xFF)); // divisor high byte
}

// Send EOI to PIC
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        __outb(0xA0, 0x20); // Send EOI to slave
    }
    __outb(0x20, 0x20); // Send EOI to master
}

// Mask IRQ
void pic_mask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }

    value = __inb(port) | (1 << irq);
    __outb(port, value);
}

// Unmask IRQ
void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }

    value = __inb(port) & ~(1 << irq);
    __outb(port, value);
}

// I/O port functions
static inline void __outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t __inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Debug functions
void dump_cpu_state(cpu_state_t *state) {
    boot_log("=== CPU State ===");
    boot_log("RAX: ");
    early_console_write_hex(state->rax);
    boot_log("RBX: ");
    early_console_write_hex(state->rbx);
    boot_log("RCX: ");
    early_console_write_hex(state->rcx);
    boot_log("RDX: ");
    early_console_write_hex(state->rdx);
    boot_log("RSI: ");
    early_console_write_hex(state->rsi);
    boot_log("RDI: ");
    early_console_write_hex(state->rdi);
    boot_log("RSP: ");
    early_console_write_hex(state->rsp);
    boot_log("RBP: ");
    early_console_write_hex(state->rbp);
    boot_log("RIP: ");
    early_console_write_hex(state->rip);
    boot_log("CS:  ");
    early_console_write_hex(state->cs);
    boot_log("SS:  ");
    early_console_write_hex(state->ss);
    boot_log("RFLAGS: ");
    early_console_write_hex(state->eflags);
    boot_log("Interrupt: ");
    early_console_write_hex(state->int_no);
    boot_log("Error Code: ");
    early_console_write_hex(state->err_code);
}

void interrupt_stats(void) {
    boot_log("=== Interrupt Statistics ===");
    boot_log("Total interrupts: ");
    early_console_write_hex(total_interrupts);

    for (int i = 0; i < IDT_ENTRIES; i++) {
        if (interrupt_counts[i] > 0) {
            boot_log("IRQ ");
            early_console_write_hex(i);
            boot_log(": ");
            early_console_write_hex(interrupt_counts[i]);
        }
    }
}
