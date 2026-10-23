/**
 * QuantumOS COM2 Serial Swarm Bridge implementation (ghostd phase 4, #51).
 *
 * Polled UART on 0x2F8 — the mirror of the COM1 boot console's write loop
 * in boot.S, but a distinct port and exposed to ring 3 as a capability
 * resource rather than a kernel-only log sink. COM1 is never touched here.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/com2_uart.h>
#include <kernel/boot.h>

/* 16550 register offsets from the UART base */
#define UART_THR 0 /* Transmit Holding Register (write, DLAB=0) */
#define UART_RBR 0 /* Receive Buffer Register  (read,  DLAB=0) */
#define UART_DLL 0 /* Divisor Latch Low        (DLAB=1) */
#define UART_IER 1 /* Interrupt Enable Register (DLAB=0) */
#define UART_DLH 1 /* Divisor Latch High       (DLAB=1) */
#define UART_FCR 2 /* FIFO Control Register (write) */
#define UART_LCR 3 /* Line Control Register */
#define UART_MCR 4 /* Modem Control Register */
#define UART_LSR 5 /* Line Status Register */
#define UART_SCR 7 /* Scratch Register */

#define LSR_DATA_READY 0x01 /* a byte is waiting in the RBR */
#define LSR_THR_EMPTY 0x20  /* the THR can accept another byte */

#define LCR_DLAB 0x80 /* divisor-latch access bit */
#define LCR_8N1 0x03  /* 8 data bits, no parity, 1 stop bit */

static uint8_t com2_ready = 0;

/* Upper bound on the per-byte THR-drain wait, mirroring console.c's COM1
 * THR_DRAIN_SPINS. com2_write runs cli'd from SYS_COM2, so an UNBOUNDED spin on
 * a wedged port — a swarm peer that stops draining the serial link, leaving THRE
 * clear forever — would freeze the ENTIRE kernel (no timer tick, no preemption),
 * not just the writer. Capping it trades a dropped byte on a genuinely stuck
 * port for guaranteed liveness. ~2M iterations far exceeds a real 115200-baud
 * byte time yet is a small bounded stall. */
#define COM2_THR_DRAIN_SPINS 2000000u

/* Sticky: set once COM2's THR fails to drain within the spin cap, so a wedged
 * or absent peer is not paid for on every subsequent byte (cli'd) forever. */
static uint8_t com2_dead = 0;

/* Set by com2_init when the scratch register does not hold a written value:
 * no UART decodes 0x2F8 at all. Unbacked ports do not all read 0xFF �
 * Firecracker has no COM2 and returns 0 for every read, with an expensive
 * VM exit (and a host log line) each time. LSR=0 means THR never empties, so
 * the first SYS_COM2 write spun COM2_THR_DRAIN_SPINS exits with interrupts
 * off and froze the guest for tens of seconds, and every read cost an exit.
 * With no device, writes go nowhere (as on an unbacked QEMU port) and reads
 * return nothing, without touching the port. */
static uint8_t com2_nodev = 0;

static inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t io_inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void com2_init(void) {
    /* Disable interrupts — this port is polled, mirroring the COM1 console. */
    io_outb(COM2_PORT_BASE + UART_IER, 0x00);

    /* 115200 baud: DLAB on, divisor 1, DLAB off + 8N1. */
    io_outb(COM2_PORT_BASE + UART_LCR, LCR_DLAB);
    io_outb(COM2_PORT_BASE + UART_DLL, 0x01);
    io_outb(COM2_PORT_BASE + UART_DLH, 0x00);
    io_outb(COM2_PORT_BASE + UART_LCR, LCR_8N1);

    /* Enable + clear the FIFOs (14-byte RX trigger). */
    io_outb(COM2_PORT_BASE + UART_FCR, 0xC7);

    /* DTR + RTS asserted; OUT2 set (harmless while polling). */
    io_outb(COM2_PORT_BASE + UART_MCR, 0x0B);

    io_outb(COM2_PORT_BASE + UART_SCR, 0x5A);
    uint8_t a = io_inb(COM2_PORT_BASE + UART_SCR);
    io_outb(COM2_PORT_BASE + UART_SCR, 0xA5);
    uint8_t b = io_inb(COM2_PORT_BASE + UART_SCR);
    com2_nodev = (a != 0x5A || b != 0xA5) ? 1 : 0;

    com2_ready = 1;
    boot_log("COM2 serial swarm bridge online (0x2F8, polled, cap-guarded)");
}

uint32_t com2_write(const uint8_t *buf, uint32_t len) {
    if (!com2_ready || !buf || com2_dead) {
        return 0;
    }
    if (com2_nodev) {
        return len;
    }
    for (uint32_t i = 0; i < len; i++) {
        uint32_t spins = 0;
        while (!(io_inb(COM2_PORT_BASE + UART_LSR) & LSR_THR_EMPTY)) {
            if (++spins >= COM2_THR_DRAIN_SPINS) {
                /* Port wedged: latch it dead and stop hanging the kernel with
                 * interrupts disabled. Return the count written so far so the
                 * ring-3 caller (swarm_svc) sees a short write and stops. */
                com2_dead = 1;
                return i;
            }
        }
        io_outb(COM2_PORT_BASE + UART_THR, buf[i]);
    }
    return len;
}

uint32_t com2_read(uint8_t *buf, uint32_t len) {
    if (!com2_ready || !buf || com2_nodev) {
        return 0;
    }
    uint32_t n = 0;
    while (n < len && (io_inb(COM2_PORT_BASE + UART_LSR) & LSR_DATA_READY)) {
        buf[n++] = io_inb(COM2_PORT_BASE + UART_RBR);
    }
    return n;
}

/* Sticky: set once LSR reads 0xFF — the unbacked-port signature (all error
 * bits + TEMT + THRE + DR simultaneously, impossible on real silicon). QEMU
 * boots with a single -serial (nearly every functional CI gate, and the
 * qBraid/laptop boots) leave 0x2F8 unassigned, and unassigned I/O reads
 * all-ones — so without this latch the ADR-0022 RX-boost check would see
 * DATA_READY on EVERY tick of every COM2-less boot and hand swarm-svc a
 * permanent unearned scheduling skew. Latched once; one io_inb saved per
 * tick thereafter. */
static uint8_t com2_absent = 0;

int com2_rx_pending(void) {
    if (!com2_ready || com2_dead || com2_absent || com2_nodev) {
        return 0;
    }
    uint8_t lsr = io_inb(COM2_PORT_BASE + UART_LSR);
    if (lsr == 0xFF) {
        com2_absent = 1;
        return 0;
    }
    return (lsr & LSR_DATA_READY) ? 1 : 0;
}
