/**
 * QuantumOS virtio core — transport abstraction (ADR-0024)
 *
 * A virtio device driver (virtio-net today) is written once against this
 * small ops table; each transport fills it in:
 *
 *   - virtio-mmio  (kernel/src/virtio_mmio.c) — Firecracker and other
 *     bus-less microVMs; devices named on the kernel command line.
 *   - virtio-pci   (kernel/src/virtio_pci.c)  — QEMU, KVM and the clouds;
 *     the modern (virtio 1.x) capability layout, INTx interrupts.
 *
 * A transport can bind several devices at once (one per device type: the
 * NIC and the disk on the same microVM). Each binding keeps its own
 * register windows behind `dev`, and every op takes the binding it acts
 * on, so probing a second device never disturbs the first.
 *
 * Both are MODERN only (VIRTIO_F_VERSION_1). Legacy virtio-pci (the I/O
 * BAR layout) is deliberately unsupported: every hypervisor we target
 * offers the modern interface, including on QEMU's transitional devices.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include <kernel/types.h>

/* Device status bits (virtio 1.2 §2.1) */
#define VIRTIO_ST_ACKNOWLEDGE 1
#define VIRTIO_ST_DRIVER 2
#define VIRTIO_ST_DRIVER_OK 4
#define VIRTIO_ST_FEATURES_OK 8
#define VIRTIO_ST_FAILED 128

/* Feature bits common to every device */
#define VIRTIO_F_VERSION_1 32

/* Device types (virtio 1.2 §5) */
#define VIRTIO_ID_NET 1
#define VIRTIO_ID_BLOCK 2
#define VIRTIO_ID_ENTROPY 4

/* Split-virtqueue descriptor flags (§2.7.5) */
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

/* Split-virtqueue available-ring flag (§2.7.6): ask the device not to
 * interrupt. A polled driver sets it so its line never stays asserted. */
#define VRING_AVAIL_F_NO_INTERRUPT 1

/* Most devices one transport binds at once (a NIC and a disk today). */
#define VIRTIO_MAX_BOUND 4

typedef struct virtio_transport virtio_transport_t;

/**
 * One transport's view of one bound device. Every op takes the binding it
 * acts on; `dev` is the transport's private per-device state.
 */
struct virtio_transport {
    const char *name; /* "mmio" or "pci" — for the boot log */
    void *dev;        /* transport-private: this binding's register windows */

    uint8_t (*get_status)(const virtio_transport_t *t);
    void (*set_status)(const virtio_transport_t *t, uint8_t status);

    uint64_t (*get_features)(const virtio_transport_t *t);
    void (*set_features)(const virtio_transport_t *t, uint64_t features);

    /* Configure queue `index` with `size` entries at the three given
     * physical addresses and enable it. Returns 1 on success, 0 if the
     * device has no such queue, refuses `size`, or it is already live. */
    int (*queue_setup)(const virtio_transport_t *t, uint16_t index, uint16_t size, uint64_t desc,
                       uint64_t driver, uint64_t device);

    /* Tell the device that queue `index` has new available buffers. */
    void (*notify)(const virtio_transport_t *t, uint16_t index);

    /* Read-and-acknowledge the interrupt status. Nonzero means the device
     * raised an interrupt (bit 0: used buffer, bit 1: config change). */
    uint32_t (*isr_ack)(const virtio_transport_t *t);

    /* One byte of the device-specific configuration space. */
    uint8_t (*config_read8)(const virtio_transport_t *t, uint32_t offset);

    /* The legacy 8259 line the device interrupts on (< 16). */
    uint8_t irq;
};

/* Record one `virtio_mmio.device=<size>@<base>:<irq>` slot from the
 * cmdline (token format in virtio_net.h). Returns 1 if stored, 0 if the
 * table is full or irq >= 16. Idempotent per base. */
int virtio_mmio_add_device(uint64_t base, uint64_t size, uint8_t irq);

/* How many slots the cmdline gave us (diagnostics / boot log). */
int virtio_mmio_device_count(void);

/* Probe each transport for a device of `device_id` that is not already
 * bound. On success `out` is filled in and 1 is returned; 0 means no such
 * device (or no free binding). A failed probe leaves earlier bindings
 * untouched. */
int virtio_mmio_probe(uint32_t device_id, virtio_transport_t *out);
int virtio_pci_probe(uint32_t device_id, virtio_transport_t *out);

#endif /* VIRTIO_H */
