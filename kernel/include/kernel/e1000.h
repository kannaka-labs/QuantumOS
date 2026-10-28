/**
 * QuantumOS Intel e1000 (82540EM) NIC driver (#252)
 *
 * The third NIC behind the netdev contract, for hypervisors and bare metal
 * that offer neither virtio nor an rtl8139: QEMU's `-device e1000`, VMware,
 * VirtualBox, and the 8254x family on real boards. Memory-mapped registers
 * (BAR0, usually placed in the 3-4 GB hole above the boot identity map, so
 * the window is mapped with mmio_identity_map() first), legacy 16-byte
 * descriptor rings, EEPROM-sourced MAC, INTx receive interrupt.
 *
 * Same shape as rtl8139.c: polled transmit, interrupt-driven receive into a
 * small software queue the network layer drains (ADR-0005: the IRQ is the
 * single producer, the net thread the single consumer).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef E1000_H
#define E1000_H

#include <kernel/types.h>
#include <kernel/netdev.h>

/* Probe PCI bus 0 for an 82540EM (8086:100E), map BAR0, reset the chip,
 * read the MAC from the EEPROM (falling back to RAL/RAH), set up the RX and
 * TX descriptor rings and enable the receive interrupt. Returns 1 if a NIC
 * was found and brought up, 0 if absent or unusable (silent when absent:
 * the rtl8139 probe follows). */
int e1000_init(void);

/* Non-zero once a NIC is up. */
int e1000_present(void);

/* Copy the NIC's 6-byte MAC into out. */
void e1000_get_mac(uint8_t out[ETH_ADDR_LEN]);

/* Transmit one raw Ethernet frame through the next TX descriptor and wait
 * (bounded) for the chip to report it done. Returns 0 on success, -1 on
 * error/timeout. */
int e1000_transmit(const uint8_t *frame, uint16_t len);

/* IRQ body (called from the shared IRQ dispatch): reads ICR (which clears
 * it) and drains every completed RX descriptor into the software queue,
 * returning the descriptors to the chip. */
void e1000_irq(void);

/* The PIC line the NIC is wired to (from PCI config), for unmasking. */
uint8_t e1000_irq_line(void);

/* Pop one received frame into buf (up to max bytes). Returns the frame
 * length, or 0 if none is waiting. Interrupt-safe. Called by the network
 * layer. */
uint16_t e1000_receive(uint8_t *buf, uint16_t max);

/* This driver as a netdev. Bound by netdev_init() via the probe list in
 * kernel/src/netdev.c, after virtio-net and before the rtl8139. */
extern const netdev_ops_t e1000_netdev;

#endif /* E1000_H */
