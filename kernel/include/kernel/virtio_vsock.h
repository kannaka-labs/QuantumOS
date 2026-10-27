#ifndef VIRTIO_VSOCK_H
#define VIRTIO_VSOCK_H

#include <kernel/types.h>

/* virtio-vsock shell channel (ADR-0029, #259). One stream listener on
 * VSOCK_SHELL_PORT and at most one connection. While a host peer is
 * connected, the shell's console output goes here instead of COM1; the
 * kernel log always stays on COM1. Every entry point expects interrupts off
 * (the timer IRQ, or console_write's irq_save). */

#define VSOCK_SHELL_PORT 1024

/* Probe and bring up the device; silent when absent. Returns 1 when bound. */
int virtio_vsock_init(void);

/* 1 while a host peer is connected to the shell port. */
int virtio_vsock_connected(void);

/* Drain the device: accept or refuse connections, feed received bytes to
 * the console input path, track credit, handle transport resets. Returns 1
 * when new console input arrived. Cheap when idle (one memory read). */
int virtio_vsock_poll(void);

/* Send `len` bytes to the connected peer. Returns the number of bytes sent;
 * less than `len` (possibly 0) when there is no peer, no credit within a
 * bounded wait, or the device stopped answering. The caller writes the rest
 * to COM1, so output is never lost. */
uint32_t virtio_vsock_send(const uint8_t *buf, uint32_t len);

#endif /* VIRTIO_VSOCK_H */
