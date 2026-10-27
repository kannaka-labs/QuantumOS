/**
 * QuantumOS Console Input/Output (epic #62 phase 1, #63)
 *
 * The interactive half of the boot console. Output has always existed
 * (early_console_write in boot.S, polled COM1 TX); this adds INPUT: an
 * interrupt-driven ring buffer fed by two producers —
 *
 *   - COM1 RX (IRQ4): bytes typed into the serial console (QEMU
 *     -serial stdio, the qBraid watch window, CI's piped stdin), and
 *   - the PS/2 keyboard (IRQ1): scancode set 1 translated to ASCII,
 *     for interactive/graphical boots.
 *
 * Ring 3 reaches the console through SYS_CONS, guarded as a capability
 * resource (CAP_RESOURCE_DEVICE over DEVICE_ID_CONSOLE) exactly like the
 * COM2 swarm bridge: no capability, no console. console_write() is the
 * raw byte sink for the shell — unlike SYS_WRITE it adds no "[user pid]"
 * prefix, so a ring-3 shell can own its own line discipline.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <kernel/types.h>

/* I/O base of the first UART (the boot console). Also the capability
 * resource_id, so the guarded resource is self-identifying — the same
 * convention as DEVICE_ID_COM2 (CAP_RESOURCE_DEVICE / 0x3F8). */
#define COM1_PORT_BASE 0x3F8
#define DEVICE_ID_CONSOLE COM1_PORT_BASE

/* Bring up console input: drain any byte already waiting in the COM1
 * receiver INTO the ring (CI pipes input from t=0, before this runs —
 * clearing the FIFO here would eat it), then enable the COM1 RX
 * interrupt and reset the PS/2 state machine. The COM1 TX path used by
 * early_console_write is untouched. IRQ1/IRQ4 still need unmasking via
 * interrupt_enable() once the IDT handlers are live. */
void console_init(void);

/* IRQ4 body: drain the COM1 receiver into the ring (bounded; no-op
 * when no UART was detected at init). */
void console_com1_irq(void);

/* True if a real 16550 answered the init-time scratch-register test.
 * False on serial-less hardware — reads at 0x3F8 float to 0xFF there,
 * and nothing should ever spin on that port's status bits. */
int console_com1_present(void);

/* IRQ1 body: read one scancode from the PS/2 data port, translate
 * (set 1, US layout, shift tracked), and push the ASCII byte. */
void console_kbd_irq(void);

/* Drain up to `len` buffered input bytes (non-blocking; 0 if none).
 * Interrupt-safe: the drain runs with interrupts disabled. */
uint32_t console_read(uint8_t *buf, uint32_t len);

/* Nonzero while the input ring holds undrained bytes. */
int console_rx_pending(void);

/* Console input that arrived on another wire (the vsock shell channel,
 * ADR-0029): runs the same escape parsing as COM1 receive (session reseed,
 * ADR-0025) and lands in the same input ring. Interrupts must be off. */
void console_input_bytes(const uint8_t *buf, uint32_t len);

/* Write `len` raw bytes to COM1, polling THR-empty per byte. The whole
 * write runs with interrupts disabled (like early_console_write) so a
 * timer-tick log line cannot split it. Returns bytes written (== len). */
uint32_t console_write(const uint8_t *buf, uint32_t len);

#endif /* CONSOLE_H */
