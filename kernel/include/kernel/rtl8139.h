/**
 * QuantumOS RealTek RTL8139 NIC driver (epic #73 phase 1)
 *
 * A minimal driver for the RTL8139 (QEMU's `-device rtl8139`). Polled
 * transmit, interrupt-driven receive into a linear ring buffer. Frames
 * received by the IRQ handler are pushed to a small kernel queue the
 * network layer drains; the raw driver knows nothing of protocols.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef RTL8139_H
#define RTL8139_H

#include <kernel/types.h>
#include <kernel/netdev.h>

/* ETH_ADDR_LEN, NET_MTU and NET_FRAME_MAX now live in <kernel/netdev.h>
 * because the stack needs them without knowing which NIC it has. These
 * aliases keep this driver's own source unchanged. */
#define RTL_MTU NET_MTU
#define RTL_FRAME_MAX NET_FRAME_MAX

/* Probe PCI for the RTL8139, reset it, read the MAC, set up the RX ring
 * and TX descriptors, and enable RX/TX + the ROK/TOK interrupt. Returns
 * 1 if a NIC was found and brought up, 0 if absent (honest degrade —
 * the default NIC-less boot logs it and continues). */
int rtl8139_init(void);

/* Non-zero once a NIC is up. */
int rtl8139_present(void);

/* Copy the NIC's 6-byte MAC into out. */
void rtl8139_get_mac(uint8_t out[ETH_ADDR_LEN]);

/* Transmit one raw Ethernet frame (bounded polling on descriptor
 * availability). Returns 0 on success, -1 on error/timeout. */
int rtl8139_transmit(const uint8_t *frame, uint16_t len);

/* IRQ body (called from the shared IRQ dispatch): drains received
 * packets from the ring into the network queue and acknowledges the
 * NIC's interrupt status. */
void rtl8139_irq(void);

/* The PIC line the NIC is wired to (from PCI config), for unmasking. */
uint8_t rtl8139_irq_line(void);

/* Drain one received frame into buf (up to max bytes). Returns the frame
 * length, or 0 if the queue is empty. Interrupt-safe. Called by the
 * network layer. */
uint16_t rtl8139_receive(uint8_t *buf, uint16_t max);

/* This driver as a netdev. Bound by netdev_init() via the probe list in
 * kernel/src/netdev.c; the stack calls it only through that table. */
extern const netdev_ops_t rtl8139_netdev;

#endif /* RTL8139_H */
