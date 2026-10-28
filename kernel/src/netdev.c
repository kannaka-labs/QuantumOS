/**
 * QuantumOS netdev binding.
 *
 * A static probe list, bound once. No allocation, no registration calls at
 * runtime: drivers are known at link time, which keeps the boot path free of
 * ordering surprises and lets the linker drop an unreferenced driver.
 *
 * Probe ORDER is policy and is deliberately explicit below.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/netdev.h>
#include <kernel/virtio_net.h>
#include <kernel/e1000.h>
#include <kernel/rtl8139.h>
#include <kernel/boot.h>
#include <kernel/egress.h>

/* Drivers, in probe order. The first whose init() returns 1 is bound.
 * virtio-net is FIRST deliberately (ADR-0024): it is the paravirtual NIC
 * every modern hypervisor offers, and the rtl8139 is the emulated fallback.
 * Its probe tries the virtio-mmio slots the cmdline named (Firecracker has
 * no PCI bus at all), then one PCI bus-0 scan for a virtio-net function
 * (0x1AF4:0x1041, or transitional 0x1000). With neither present it returns
 * 0 and the rtl8139 probe runs exactly as before. */
static const netdev_ops_t *const drivers[] = {
    &virtio_net_netdev,
    &e1000_netdev,
    &rtl8139_netdev,
};

#define DRIVER_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

static const netdev_ops_t *bound; /* NULL until a driver reports present */

/* Checksum offload side channel (#253; see netdev.h). */
static int tx_csum_offload;
static int rx_csum_valid; /* for the frame netdev_receive last returned */

int netdev_init(void) {
    for (int i = 0; i < DRIVER_COUNT; i++) {
        const netdev_ops_t *d = drivers[i];
        if (d && d->init && d->init()) {
            bound = d;
            return 1;
        }
    }
    boot_log("NET: no network device found");
    return 0;
}

const char *netdev_name(void) {
    return (bound && bound->name) ? bound->name : "none";
}

int netdev_present(void) {
    return (bound && bound->present) ? bound->present() : 0;
}

void netdev_get_mac(uint8_t out[ETH_ADDR_LEN]) {
    if (bound && bound->get_mac) {
        bound->get_mac(out);
        return;
    }
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out[i] = 0;
    }
}

int netdev_transmit(const uint8_t *frame, uint16_t len) {
    /* ADR-0030: no grant, or a spent one, and the frame stays here. Counted after a
     * successful hand-off, so the budget measures what reached the wire. */
    if (!bound || !bound->transmit || !egress_active()) {
        return -1;
    }
    int r = bound->transmit(frame, len);
    if (r == 0) {
        egress_account(len);
    }
    return r;
}

void netdev_irq(void) {
    if (bound && bound->irq) {
        bound->irq();
    }
}

uint8_t netdev_irq_line(void) {
    return (bound && bound->irq_line) ? bound->irq_line() : 0;
}

uint16_t netdev_receive(uint8_t *buf, uint16_t max) {
    rx_csum_valid = 0; /* the driver asserts it per frame, or not at all */
    uint16_t n = (bound && bound->receive) ? bound->receive(buf, max) : 0;
    if (n == 0) {
        return 0;
    }
    /* ADR-0030: inbound frames are drained from the NIC regardless (the ring must not
     * wedge) but dropped here when no grant is live, and counted when one is. */
    if (!egress_active()) {
        return 0;
    }
    egress_account(n);
    return n;
}

void netdev_set_tx_csum_offload(int on) {
    tx_csum_offload = on ? 1 : 0;
}

int netdev_tx_csum_offload(void) {
    return tx_csum_offload;
}

void netdev_rx_csum_valid_set(int valid) {
    rx_csum_valid = valid ? 1 : 0;
}

int netdev_rx_csum_valid(void) {
    return rx_csum_valid;
}
