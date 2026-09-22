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
#include <kernel/rtl8139.h>
#include <kernel/boot.h>

/* Drivers, in probe order. The first whose init() returns 1 is bound.
 * virtio-net is FIRST deliberately: its probe is a handful of MMIO reads
 * at addresses the cmdline already named, whereas the rtl8139 probe walks
 * the PCI bus — and a Firecracker guest has no PCI bus at all. Cheap and
 * certain before expensive and absent. With no virtio_mmio.device= token
 * the virtio probe returns 0 immediately and costs nothing. */
static const netdev_ops_t *const drivers[] = {
    &virtio_net_netdev,
    &rtl8139_netdev,
};

#define DRIVER_COUNT ((int)(sizeof(drivers) / sizeof(drivers[0])))

static const netdev_ops_t *bound; /* NULL until a driver reports present */

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
    return (bound && bound->transmit) ? bound->transmit(frame, len) : -1;
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
    return (bound && bound->receive) ? bound->receive(buf, max) : 0;
}
