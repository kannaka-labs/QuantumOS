/**
 * QuantumOS virtio-net over virtio-mmio (the Firecracker NIC)
 *
 * Firecracker exposes no PCI at all (docs/FIRECRACKER.md), so the rtl8139
 * can never be found there and the whole network stack is dead on that
 * hypervisor. This driver is the other half: a modern (virtio 1.x) MMIO
 * transport and a virtio-net device behind the same netdev contract.
 *
 * Device placement is not discoverable — there is no bus to walk. The
 * hypervisor passes it on the kernel command line, one token per device:
 *
 *     virtio_mmio.device=4K@0xd0000000:5
 *                        size@base:irq
 *
 * `virtio_mmio_add_device()` records one such token; call it BEFORE
 * netdev_init(), from the cmdline parser, exactly as `ip=` and `peer=`
 * are handled today. The probe then checks each recorded slot for the
 * virtio magic and a network device id.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <kernel/types.h>
#include <kernel/netdev.h>

/* Most slots any one guest is given. Firecracker's default is well under
 * this; the array is static so the probe needs no allocator. */
#define VIRTIO_MMIO_MAX_DEVICES 8

/* Record one `virtio_mmio.device=<size>@<base>:<irq>` slot from the
 * cmdline. Returns 1 if stored, 0 if the table is full or irq >= 16 (the
 * rtl8139 lesson: an unusable line can never deliver RX, so refuse it
 * here rather than unmask a bogus vector later). Idempotent per base. */
int virtio_mmio_add_device(uint64_t base, uint64_t size, uint8_t irq);

/* How many slots the cmdline gave us (diagnostics / boot log). */
int virtio_mmio_device_count(void);

/* The netdev entry points. Declared because they are non-static (the ops
 * table below takes their addresses); the stack calls them only through
 * that table, never by name. */
int virtio_net_init(void);
int virtio_net_present(void);
void virtio_net_get_mac(uint8_t out[ETH_ADDR_LEN]);
int virtio_net_transmit(const uint8_t *frame, uint16_t len);
void virtio_net_irq(void);
uint8_t virtio_net_irq_line(void);
uint16_t virtio_net_receive(uint8_t *buf, uint16_t max);

/* The driver as a netdev; bound by netdev_init() via the probe list. */
extern const netdev_ops_t virtio_net_netdev;

#endif /* VIRTIO_NET_H */
