/**
 * QuantumOS virtio-net (modern virtio 1.x; ADR-0024)
 *
 * One device driver over two transports (kernel/include/kernel/virtio.h):
 * virtio-pci for QEMU/KVM and the clouds, virtio-mmio for Firecracker,
 * which exposes no PCI at all (docs/FIRECRACKER.md). It sits behind the
 * same netdev contract as the rtl8139 and is probed before it.
 *
 * For the MMIO transport, device placement is not discoverable — there is
 * no bus to walk. The hypervisor passes it on the kernel command line, one
 * token per device:
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
#include <kernel/virtio.h>

/* virtio_mmio_add_device() is declared in virtio.h (it is transport-level:
 * every virtio-mmio device, NIC or disk, is named this way). irq >= 16 is
 * refused there (the rtl8139 lesson: an unusable line can never deliver
 * RX, so refuse it rather than unmask a bogus vector later). */

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
