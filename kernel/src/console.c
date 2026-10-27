/**
 * QuantumOS Console Input implementation (epic #62 phase 1, #63).
 *
 * A single input ring with two interrupt-driven producers (COM1 RX,
 * PS/2 keyboard) and one consumer (SYS_CONS reads). Both producers run
 * inside interrupt handlers (IF=0); the consumer and console_write()
 * take an explicit IRQ-save guard so they are safe from any context.
 *
 * The COM1 transmit path stays in boot.S (early_console_write) — this
 * file only ever ADDS the receive side (IER bit 0). The one delicate
 * moment is console_init(): CI pipes shell commands into QEMU's stdin
 * from t=0, so a byte may already sit in the receiver before the kernel
 * gets here. The init therefore drains the receiver into the ring FIRST,
 * so the FIFO-clear that follows discards nothing; QEMU's chardev flow
 * control holds the rest of the pipe until the guest actually reads.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/console.h>
#include <kernel/quantum.h>
#include <kernel/boot.h>
#include <kernel/vga.h>
#include <kernel/virtio_vsock.h>

/* 16550 register offsets (same layout as com2_uart.c) */
#define UART_THR 0 /* Transmit Holding Register (write, DLAB=0) */
#define UART_RBR 0 /* Receive Buffer Register  (read,  DLAB=0) */
#define UART_DLL 0 /* Divisor Latch Low        (DLAB=1) */
#define UART_IER 1 /* Interrupt Enable Register (DLAB=0) */
#define UART_DLH 1 /* Divisor Latch High       (DLAB=1) */
#define UART_FCR 2 /* FIFO Control Register (write) */
#define UART_LCR 3 /* Line Control Register */
#define UART_MCR 4 /* Modem Control Register */
#define UART_LSR 5 /* Line Status Register */
#define UART_SCR 7 /* Scratch Register (presence detection) */

#define LSR_DATA_READY 0x01
#define LSR_THR_EMPTY 0x20

#define LCR_DLAB 0x80
#define LCR_8N1 0x03

#define IER_RX_AVAIL 0x01 /* interrupt when a received byte is ready */

/* PS/2 controller ports */
#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_STATUS_OUTPUT_FULL 0x01

static inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t io_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

/* ============================================================================
 * Input ring buffer
 * ============================================================================ */

#define CONSOLE_RING_SIZE 1024 /* power of two */

static uint8_t ring[CONSOLE_RING_SIZE];
static volatile uint32_t ring_head; /* producer index (IRQ context) */
static volatile uint32_t ring_tail; /* consumer index */
static volatile uint32_t ring_dropped;

/* Producer side. Called with interrupts off (IRQ handler or init).
 * Drops the newest byte when full — honest backpressure, and the
 * counter makes the loss observable. */
static void ring_push(uint8_t byte) {
    uint32_t next = (ring_head + 1) & (CONSOLE_RING_SIZE - 1);
    if (next == ring_tail) {
        ring_dropped++;
        return;
    }
    ring[ring_head] = byte;
    ring_head = next;
}

uint32_t console_read(uint8_t *buf, uint32_t len) {
    if (!buf || len == 0) {
        return 0;
    }
    uint64_t flags = irq_save();
    uint32_t n = 0;
    while (n < len && ring_tail != ring_head) {
        buf[n++] = ring[ring_tail];
        ring_tail = (ring_tail + 1) & (CONSOLE_RING_SIZE - 1);
    }
    irq_restore(flags);
    return n;
}

int console_rx_pending(void) {
    return ring_tail != ring_head;
}

/* ============================================================================
 * COM1 receive
 * ============================================================================ */

/* Set at init if a real 16550 answers the scratch-register test. On a
 * machine with no UART behind 0x3F8 every port read floats to 0xFF —
 * which makes LSR look like "data ready, forever". The previously
 * unguarded drain loop below hung the first real-hardware boot at the
 * 45% splash stage (reproduced exactly in QEMU with -serial none). */
static uint8_t com1_present;

int console_com1_present(void) {
    return com1_present;
}

/* Session reseed escape (ADR-0025). A host that restores this VM from a
 * snapshot sends   ESC ] q s e e d ;  <16 hex digits>  BEL   on COM1 before
 * handing the console to a user. The sequence is consumed here (it never
 * reaches the shell) and its value is mixed into the quantum generator, so
 * every restored copy of one snapshot draws its own stream. Any byte that does
 * not continue the sequence flushes what was held, in order, as ordinary input
 * (arrow keys, ESC [ ..., are unaffected). Typing the sequence by hand only
 * mixes more entropy in, which is harmless. Bounded: at most 25 bytes held. */
static const char RESEED_PREFIX[] = "\x1b]qseed;";
#define RESEED_PREFIX_LEN (sizeof(RESEED_PREFIX) - 1)
#define RESEED_HEX_LEN 16
static uint8_t reseed_buf[RESEED_PREFIX_LEN + RESEED_HEX_LEN];
static uint32_t reseed_len;

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static void reseed_flush(void) {
    for (uint32_t i = 0; i < reseed_len; i++) {
        ring_push(reseed_buf[i]);
    }
    reseed_len = 0;
}

/* One received COM1 byte: either part of the reseed escape, or input. */
static void com1_rx_byte(uint8_t b) {
    if (reseed_len < RESEED_PREFIX_LEN) {
        if (b == (uint8_t)RESEED_PREFIX[reseed_len]) {
            reseed_buf[reseed_len++] = b;
            return;
        }
        reseed_flush();
        if (b == (uint8_t)RESEED_PREFIX[0]) { /* a fresh ESC may start a new one */
            reseed_buf[reseed_len++] = b;
            return;
        }
        ring_push(b);
        return;
    }
    if (reseed_len < RESEED_PREFIX_LEN + RESEED_HEX_LEN) {
        if (hex_val(b) >= 0) {
            reseed_buf[reseed_len++] = b;
            return;
        }
        reseed_flush();
        ring_push(b);
        return;
    }
    /* prefix + 16 hex held: BEL completes it, anything else is input */
    if (b == 0x07) {
        uint64_t seed = 0;
        for (uint32_t i = 0; i < RESEED_HEX_LEN; i++) {
            seed = (seed << 4) | (uint64_t)hex_val(reseed_buf[RESEED_PREFIX_LEN + i]);
        }
        reseed_len = 0;
        quantum_session_reseed(seed);
        return;
    }
    reseed_flush();
    ring_push(b);
}

void console_input_bytes(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        com1_rx_byte(buf[i]);
    }
}

void console_com1_irq(void) {
    if (!com1_present) {
        return;
    }
    /* Bounded drain: a floating or wedged LSR must never spin the
     * kernel. 64 bytes far exceeds any real 16550 FIFO. */
    for (int n = 0; n < 64; n++) {
        uint8_t lsr = io_inb(COM1_PORT_BASE + UART_LSR);
        if (lsr == 0xFF || !(lsr & LSR_DATA_READY)) {
            break;
        }
        com1_rx_byte(io_inb(COM1_PORT_BASE + UART_RBR));
    }
}

/* Upper bound on how long console_write waits for the transmit holding
 * register to drain, per byte. The wait runs with interrupts disabled (so a
 * whole line prints without a timer-tick log splicing into it), which means
 * an unbounded spin on a wedged UART — e.g. a stopped host-side reader of
 * QEMU -serial stdio leaving THRE clear forever — would freeze the entire
 * kernel, not just the writer. Capping it trades a dropped byte on a truly
 * stuck port for guaranteed liveness. ~2M iterations is far longer than a
 * real 115200-baud byte time yet still a small bounded stall. */
#define THR_DRAIN_SPINS 2000000u

/* Sticky: set once COM1's THR fails to drain within the spin cap. A
 * machine with no UART behind 0x3F8 must not pay the full spin per byte
 * forever (with interrupts disabled) when the screen console can carry
 * the output alone. */
static uint8_t com1_dead;

uint32_t console_write(const uint8_t *buf, uint32_t len) {
    if (!buf) {
        return 0;
    }
    uint64_t flags = irq_save();
    /* ADR-0029: while a host peer holds the vsock shell channel, the shell's
     * output goes there, a whole buffer per packet, and not to COM1 (~0.9 ms
     * per byte under nested Firecracker). Whatever vsock could not take (no
     * credit in time, device gone) falls through to COM1 below, so output is
     * never lost. The kernel log never comes here: it stays on COM1. */
    uint32_t via_vsock = virtio_vsock_connected() ? virtio_vsock_send(buf, len) : 0;
    for (uint32_t i = 0; i < via_vsock; i++) {
        vga_console_putc((char)buf[i]);
    }
    uint32_t written = via_vsock;
    for (uint32_t i = via_vsock; i < len; i++) {
        /* Screen first — plain memory writes, cannot wedge. No-op until
         * the VGA console is enabled; on a serial-less machine it IS the
         * shell's display. */
        vga_console_putc((char)buf[i]);
        if (!com1_dead) {
            uint32_t spins = 0;
            while (!(io_inb(COM1_PORT_BASE + UART_LSR) & LSR_THR_EMPTY)) {
                if (++spins >= THR_DRAIN_SPINS) {
                    /* UART wedged — stop paying for it rather than hang
                     * the kernel with interrupts disabled. The screen
                     * tee above keeps the console alive. */
                    com1_dead = 1;
                    break;
                }
            }
            if (!com1_dead) {
                io_outb(COM1_PORT_BASE + UART_THR, buf[i]);
            }
        }
        written++;
    }
    vga_console_sync(); /* one HW-cursor move per write, not per byte */
    irq_restore(flags);
    return written;
}

/* ============================================================================
 * PS/2 keyboard (scancode set 1, US layout)
 * ============================================================================ */

static uint8_t kbd_e0;    /* last byte was the 0xE0 extended prefix */
static uint8_t kbd_shift; /* either shift key currently held */

/* Make-code -> ASCII, unshifted. 0 = no printable mapping. */
static const char kbd_map[0x40] = {
    [0x02] = '1', [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-', [0x0D] = '=',
    [0x0E] = 8,   [0x0F] = '\t', [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',  [0x1C] = '\n', [0x1E] = 'a',  [0x1F] = 's', [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'', [0x29] = '`',  [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x',
    [0x2E] = 'c', [0x2F] = 'v',  [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/',  [0x39] = ' ',
};

/* Make-code -> ASCII with shift held. */
static const char kbd_map_shift[0x40] = {
    [0x02] = '!', [0x03] = '@',  [0x04] = '#',  [0x05] = '$', [0x06] = '%', [0x07] = '^',
    [0x08] = '&', [0x09] = '*',  [0x0A] = '(',  [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = 8,   [0x0F] = '\t', [0x10] = 'Q',  [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1A] = '{', [0x1B] = '}',  [0x1C] = '\n', [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D',
    [0x21] = 'F', [0x22] = 'G',  [0x23] = 'H',  [0x24] = 'J', [0x25] = 'K', [0x26] = 'L',
    [0x27] = ':', [0x28] = '"',  [0x29] = '~',  [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X',
    [0x2E] = 'C', [0x2F] = 'V',  [0x30] = 'B',  [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?',  [0x39] = ' ',
};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

void console_kbd_irq(void) {
    uint8_t sc = io_inb(PS2_DATA);

    if (sc == 0xE0) { /* extended prefix: swallow it and the next byte */
        kbd_e0 = 1;
        return;
    }
    if (kbd_e0) { /* extended keys (arrows etc.) unmapped for now */
        kbd_e0 = 0;
        return;
    }
    if (sc & 0x80) { /* break (release) — only shift state cares */
        uint8_t make = sc & 0x7F;
        if (make == SC_LSHIFT || make == SC_RSHIFT) {
            kbd_shift = 0;
        }
        return;
    }
    if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
        kbd_shift = 1;
        return;
    }
    if (sc < sizeof(kbd_map)) {
        char c = kbd_shift ? kbd_map_shift[sc] : kbd_map[sc];
        if (c) {
            ring_push((uint8_t)c);
        }
    }
}

/* ============================================================================
 * Init
 * ============================================================================ */

void console_init(void) {
    /* 0. Does a UART actually exist at 0x3F8? Scratch-register test: a
     *    real 16550 echoes back what we store; a machine with no COM1
     *    floats every read to 0xFF. Touching a phantom UART is not just
     *    useless — the pre-fix RX drain spun forever on a floating LSR
     *    and hung the first real-hardware boot at the 45% splash. */
    io_outb(COM1_PORT_BASE + UART_SCR, 0x5A);
    uint8_t scr1 = io_inb(COM1_PORT_BASE + UART_SCR);
    io_outb(COM1_PORT_BASE + UART_SCR, 0xA5);
    uint8_t scr2 = io_inb(COM1_PORT_BASE + UART_SCR);
    com1_present = (scr1 == 0x5A && scr2 == 0xA5);

    if (com1_present) {
        /* 1. Quiesce the receiver's interrupt while we set up. */
        io_outb(COM1_PORT_BASE + UART_IER, 0x00);

        /* 2. Line settings: 115200 8N1 (matches the COM2 bridge; QEMU's
         *    stdio chardev ignores baud, real hardware wants it explicit). */
        io_outb(COM1_PORT_BASE + UART_LCR, LCR_DLAB);
        io_outb(COM1_PORT_BASE + UART_DLL, 0x01);
        io_outb(COM1_PORT_BASE + UART_DLH, 0x00);
        io_outb(COM1_PORT_BASE + UART_LCR, LCR_8N1);

        /* 3. FCR is deliberately NOT touched. CI pipes shell input from t=0
         *    and QEMU refills the receive register asynchronously the moment
         *    the guest drains it, so there is NO point in this sequence where
         *    a FIFO clear is safe — QEMU also forces a clear on ANY change of
         *    the FIFO-enable bit (a real byte was lost to exactly this race:
         *    'help' arrived as 'hep'). Per-byte receive interrupts are ample
         *    for a console, and the flow-controlled chardev never overruns a
         *    1-byte receiver. */

        /* 4. DTR + RTS + OUT2 — OUT2 gates the UART's IRQ line to the PIC. */
        io_outb(COM1_PORT_BASE + UART_MCR, 0x0B);

        /* 5. Rescue any byte the receiver already holds into the ring, THEN
         *    enable the receive interrupt. A byte landing between these two
         *    writes still raises the IRQ once IER is set (Data-Ready is
         *    level-evaluated on the IER write), so there is no loss window. */
        console_com1_irq();
        io_outb(COM1_PORT_BASE + UART_IER, IER_RX_AVAIL);
    } else {
        /* No UART: silence the transmit path too (early_console_write
         * keeps working — a floating LSR reads as THR-empty — but the
         * bytes go nowhere; the screen console is the real display). */
        com1_dead = 1;
    }

    /* 6. Reset keyboard translation state and drain stale PS/2 bytes so
     *    the first real keystroke starts clean. Bounded, and a floating
     *    0xFF status bails: no i8042 must not hang the boot either. */
    kbd_e0 = 0;
    kbd_shift = 0;
    for (int n = 0; n < 32; n++) {
        uint8_t st = io_inb(PS2_STATUS);
        if (st == 0xFF || !(st & PS2_STATUS_OUTPUT_FULL)) {
            break;
        }
        (void)io_inb(PS2_DATA);
    }

    if (com1_present) {
        boot_log("Console input online (COM1 RX IRQ4 + PS/2 IRQ1 -> 1K ring)");
    } else {
        boot_log("CONS: no COM1 UART detected — screen/keyboard console only");
    }
}
