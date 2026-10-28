/**
 * QuantumOS network device abstraction (netdev)
 *
 * The network stack (Ethernet, ARP, IPv4, ICMP, DHCP, DNS, UDP, TCP) was
 * written against the RTL8139 and called `rtl8139_*` by symbol name, so a
 * second NIC could not exist without shadowing those symbols — which is not
 * acceptable in a capability kernel. This header is the seam: one small ops
 * table, filled in by a driver, bound once at boot.
 *
 * The contract is unchanged from the one rtl8139.h already expressed; only
 * the binding is late now. `netdev_init()` probes the registered drivers in
 * order and keeps the first that reports itself present. With no NIC the
 * bound device stays NULL and every accessor degrades exactly as the old
 * `rtl8139_present() == 0` path did — honest degrade, boot continues.
 *
 * The motivating second driver is virtio-net over virtio-mmio: Firecracker
 * exposes no PCI at all (docs/FIRECRACKER.md), so the RTL8139 can never be
 * found there and the whole stack above it is dead on that hypervisor.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NETDEV_H
#define NETDEV_H

#include <kernel/types.h>

/* Link-layer constants. These live here rather than in a driver header
 * because the stack needs them without knowing which NIC it has. */
#define ETH_ADDR_LEN 6
#define NET_MTU 1500
#define NET_FRAME_MAX 1600 /* MTU + headers, rounded up */

/**
 * One NIC driver. Every function mirrors the rtl8139 entry point of the
 * same name, so porting a driver is filling this in and nothing else.
 *
 * `init` is the probe: it must return 1 only if a device was found AND
 * brought up, and 0 otherwise, without side effects a later driver would
 * trip over. It may be called once per boot.
 */
typedef struct {
    const char *name;
    int (*init)(void);
    int (*present)(void);
    void (*get_mac)(uint8_t out[ETH_ADDR_LEN]);
    int (*transmit)(const uint8_t *frame, uint16_t len);
    void (*irq)(void);
    uint8_t (*irq_line)(void);
    uint16_t (*receive)(uint8_t *buf, uint16_t max);
} netdev_ops_t;

/* Probe every registered driver in order and bind the first present one.
 * Safe to call with no NIC available. Returns 1 if one was bound. */
int netdev_init(void);

/* Name of the bound driver, or "none". Never NULL — for logs. */
const char *netdev_name(void);

/* The accessors below forward to the bound driver. With none bound they
 * return the no-NIC answer (0 / no-op) rather than faulting, which is the
 * behaviour the stack already relies on. */
int netdev_present(void);
void netdev_get_mac(uint8_t out[ETH_ADDR_LEN]);
int netdev_transmit(const uint8_t *frame, uint16_t len);
void netdev_irq(void);
uint8_t netdev_irq_line(void);
uint16_t netdev_receive(uint8_t *buf, uint16_t max);

/* ---- checksum offload side channel (#253) ----
 *
 * The ops table above is the contract and is unchanged. Offload needs two
 * facts to cross it without a per-frame flags argument, so they travel as
 * driver-set state in netdev.c:
 *
 *  - TX: a driver whose device completes L4 checksums calls
 *    netdev_set_tx_csum_offload(1) at init. The TCP builder then stores the
 *    pseudo-header sum alone in the checksum field (the "partial" the
 *    device finishes) and the driver marks every IPv4/TCP frame it sends as
 *    needing a checksum. Both sides key off the same flag, so a frame is
 *    never half-done. Drivers without offload never call it; their frames
 *    carry the full software checksum as before.
 *  - RX: a driver that knows the device verified the frame it is about to
 *    return from receive() calls netdev_rx_csum_valid_set(1); the stack
 *    reads netdev_rx_csum_valid() for THAT frame and skips its own check.
 *    netdev_receive() clears the flag before every call, so a driver that
 *    stays silent gets software verification. */
void netdev_set_tx_csum_offload(int on);
int netdev_tx_csum_offload(void);
void netdev_rx_csum_valid_set(int valid);
int netdev_rx_csum_valid(void);

#endif /* NETDEV_H */
