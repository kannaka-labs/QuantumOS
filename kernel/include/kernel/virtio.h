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

/* Split-virtqueue descriptor flags (§2.7.5) */
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

/**
 * One transport's view of one device. Every function acts on the device
 * the transport's probe bound; a transport binds at most one device per
 * device type, which is all a single-NIC kernel needs.
 */
typedef struct {
    const char *name; /* "mmio" or "pci" — for the boot log */

    uint8_t (*get_status)(void);
    void (*set_status)(uint8_t status);

    uint64_t (*get_features)(void);
    void (*set_features)(uint64_t features);

    /* Configure queue `index` with `size` entries at the three given
     * physical addresses and enable it. Returns 1 on success, 0 if the
     * device has no such queue, refuses `size`, or it is already live. */
    int (*queue_setup)(uint16_t index, uint16_t size, uint64_t desc, uint64_t driver,
                       uint64_t device);

    /* Tell the device that queue `index` has new available buffers. */
    void (*notify)(uint16_t index);

    /* Read-and-acknowledge the interrupt status. Nonzero means the device
     * raised an interrupt (bit 0: used buffer, bit 1: config change). */
    uint32_t (*isr_ack)(void);

    /* One byte of the device-specific configuration space. */
    uint8_t (*config_read8)(uint32_t offset);

    /* The legacy 8259 line the device interrupts on (< 16). */
    uint8_t irq;
} virtio_transport_t;

/* Probe each transport for a device of `device_id`. On success the
 * transport is filled in and 1 is returned; 0 means no such device. */
int virtio_mmio_probe(uint32_t device_id, virtio_transport_t *out);
int virtio_pci_probe(uint32_t device_id, virtio_transport_t *out);

#endif /* VIRTIO_H */
