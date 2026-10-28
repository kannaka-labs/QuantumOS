/**
 * QuantumOS virtio-pci transport (virtio 1.2 §4.1, modern layout).
 *
 * Finds the device on PCI bus 0 by its modern id (0x1040 + type, e.g.
 * 0x1041 for net, 0x1042 for block) or its transitional id (0x1000 net,
 * 0x1001 block, confirmed by the subsystem id), walks the vendor capabilities for the common, notify,
 * ISR and device-config structures, maps the memory BARs they live in,
 * and drives the device through them. Interrupts default to the legacy
 * INTx line routed by firmware (config offset 0x3C), like every other
 * device in this kernel. A driver may ask for MSI-X instead (#253,
 * msix_enable): the function's MSI-X table is programmed through
 * pci_msix.c, the common config's config vector becomes 0 and each queue
 * set up afterwards gets vector 1 + index; without the capability, or when
 * the driver never asks, every vector stays VIRTIO_MSI_NO_VECTOR and INTx
 * is the path.
 *
 * The legacy I/O-BAR register layout is not used even on a transitional
 * device: its modern capabilities are always present and are what we
 * negotiate VERSION_1 through.
 *
 * Each bound device keeps its own windows in a pci_binding_t (the
 * binding's `dev`), so the NIC and the disk are driven independently.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio.h>
#include <kernel/pci.h>
#include <kernel/pci_msix.h>
#include <kernel/mmio.h>
#include <kernel/boot.h>

#define VIRTIO_PCI_VENDOR 0x1AF4
#define VIRTIO_PCI_MODERN_BASE 0x1040
#define VIRTIO_PCI_LEGACY_NET 0x1000
#define VIRTIO_PCI_LEGACY_BLOCK 0x1001
#define VIRTIO_PCI_LEGACY_ENTROPY 0x1005

#define PCI_STATUS_DWORD 0x04 /* status is the high half */
#define PCI_STATUS_CAP_LIST (1u << 20)
#define PCI_CAP_PTR 0x34
#define PCI_SUBSYSTEM 0x2C
#define PCI_INTERRUPT_LINE 0x3C
#define PCI_CAP_ID_VNDR 0x09

/* virtio_pci_cap cfg_type (§4.1.4) */
#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_ISR 3
#define CAP_DEVICE 4

/* struct virtio_pci_common_cfg offsets (§4.1.4.3) */
#define C_DFSELECT 0x00
#define C_DF 0x04
#define C_GFSELECT 0x08
#define C_GF 0x0C
#define C_MSIX_CONFIG 0x10
#define C_NUM_QUEUES 0x12
#define C_STATUS 0x14
#define C_Q_SELECT 0x16
#define C_Q_SIZE 0x18
#define C_Q_MSIX_VECTOR 0x1A
#define C_Q_ENABLE 0x1C
#define C_Q_NOTIFY_OFF 0x1E
#define C_Q_DESC 0x20
#define C_Q_DRIVER 0x28
#define C_Q_DEVICE 0x30

#define VIRTIO_MSI_NO_VECTOR 0xFFFF
#define MAX_QUEUES 4

typedef struct {
    volatile uint8_t *common;
    volatile uint8_t *notify_base;
    uint32_t notify_mult;
    volatile uint8_t *isr;
    volatile uint8_t *devcfg;
    uint16_t notify_off[MAX_QUEUES];
    uint16_t device;   /* PCI device id, so a second probe skips it */
    pci_device_t pdev; /* the function, for MSI-X programming */
    int msix_on;       /* queue_setup stamps vector 1 + index when set */
} pci_binding_t;

static pci_binding_t bindings[VIRTIO_MAX_BOUND];
static int binding_count;

static inline pci_binding_t *B(const virtio_transport_t *t) {
    return (pci_binding_t *)t->dev;
}

static inline void w8(volatile uint8_t *b, uint32_t o, uint8_t v) {
    *(volatile uint8_t *)(b + o) = v;
}
static inline void w16(volatile uint8_t *b, uint32_t o, uint16_t v) {
    *(volatile uint16_t *)(b + o) = v;
}
static inline void w32(volatile uint8_t *b, uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(b + o) = v;
}
static inline uint8_t r8(volatile uint8_t *b, uint32_t o) {
    return *(volatile uint8_t *)(b + o);
}
static inline uint16_t r16(volatile uint8_t *b, uint32_t o) {
    return *(volatile uint16_t *)(b + o);
}
static inline uint32_t r32(volatile uint8_t *b, uint32_t o) {
    return *(volatile uint32_t *)(b + o);
}

static uint8_t cfg8(const pci_device_t *d, uint8_t off) {
    uint32_t v = pci_config_read32(d->bus, d->slot, d->func, (uint8_t)(off & 0xFC));
    return (uint8_t)(v >> ((off & 3) * 8));
}

static uint32_t cfg32(const pci_device_t *d, uint8_t off) {
    return pci_config_read32(d->bus, d->slot, d->func, off);
}

/* ---- transport ops ---- */

static uint8_t p_get_status(const virtio_transport_t *t) {
    return r8(B(t)->common, C_STATUS);
}

static void p_set_status(const virtio_transport_t *t, uint8_t s) {
    w8(B(t)->common, C_STATUS, s);
}

static uint64_t p_get_features(const virtio_transport_t *t) {
    w32(B(t)->common, C_DFSELECT, 0);
    uint64_t lo = r32(B(t)->common, C_DF);
    w32(B(t)->common, C_DFSELECT, 1);
    uint64_t hi = r32(B(t)->common, C_DF);
    return lo | (hi << 32);
}

static void p_set_features(const virtio_transport_t *t, uint64_t f) {
    w32(B(t)->common, C_GFSELECT, 0);
    w32(B(t)->common, C_GF, (uint32_t)f);
    w32(B(t)->common, C_GFSELECT, 1);
    w32(B(t)->common, C_GF, (uint32_t)(f >> 32));
}

static int p_queue_setup(const virtio_transport_t *t, uint16_t index, uint16_t size, uint64_t desc,
                         uint64_t driver, uint64_t device) {
    volatile uint8_t *common = B(t)->common;
    if (index >= MAX_QUEUES || index >= r16(common, C_NUM_QUEUES)) {
        return 0;
    }
    w16(common, C_Q_SELECT, index);
    uint16_t max = r16(common, C_Q_SIZE);
    if (max == 0 || max < size || r16(common, C_Q_ENABLE) != 0) {
        return 0;
    }
    w16(common, C_Q_SIZE, size); /* a smaller power of two is allowed */
    /* MSI-X: queue q interrupts on vector 1 + q (0 is the config vector).
     * The device may refuse a vector by reading back NO_VECTOR (§4.1.5.1.2);
     * treat that as a failed setup rather than a queue that never fires. */
    uint16_t vec = B(t)->msix_on ? (uint16_t)(1 + index) : (uint16_t)VIRTIO_MSI_NO_VECTOR;
    w16(common, C_Q_MSIX_VECTOR, vec);
    if (r16(common, C_Q_MSIX_VECTOR) != vec) {
        boot_log("VIRTIO: virtio-pci refused the queue's MSI-X vector");
        return 0;
    }
    w32(common, C_Q_DESC, (uint32_t)desc);
    w32(common, C_Q_DESC + 4, (uint32_t)(desc >> 32));
    w32(common, C_Q_DRIVER, (uint32_t)driver);
    w32(common, C_Q_DRIVER + 4, (uint32_t)(driver >> 32));
    w32(common, C_Q_DEVICE, (uint32_t)device);
    w32(common, C_Q_DEVICE + 4, (uint32_t)(device >> 32));
    B(t)->notify_off[index] = r16(common, C_Q_NOTIFY_OFF);
    __asm__ volatile("" ::: "memory");
    w16(common, C_Q_ENABLE, 1);
    return 1;
}

static void p_notify(const virtio_transport_t *t, uint16_t index) {
    if (index >= MAX_QUEUES) {
        return;
    }
    pci_binding_t *b = B(t);
    w16(b->notify_base, (uint32_t)b->notify_off[index] * b->notify_mult, index);
}

static uint32_t p_isr_ack(const virtio_transport_t *t) {
    return r8(B(t)->isr, 0); /* reading the ISR status acknowledges it (§4.1.4.5) */
}

static uint8_t p_config_read8(const virtio_transport_t *t, uint32_t off) {
    return r8(B(t)->devcfg, off);
}

static int p_msix_enable(const virtio_transport_t *t, uint16_t queues) {
    pci_binding_t *b = B(t);
    if (b->msix_on || queues == 0 || queues + 1 > MSIX_VECTOR_COUNT) {
        return 0;
    }
    if (!pci_msix_setup(&b->pdev, (uint8_t)(queues + 1))) {
        return 0; /* no capability / table too small: INTx stays */
    }
    b->msix_on = 1;
    w16(b->common, C_MSIX_CONFIG, 0);
    if (r16(b->common, C_MSIX_CONFIG) != 0) {
        /* Only link-status changes ride this vector; the queues still get
         * theirs in queue_setup, so note it and carry on. */
        boot_log("VIRTIO: virtio-pci refused the config MSI-X vector");
    }
    return 1;
}

/* Map a capability's window and return its kernel address, or 0. */
static volatile uint8_t *map_cap(const pci_device_t *d, uint8_t bar, uint32_t offset,
                                 uint32_t length) {
    uint64_t base = pci_bar_mem_base(d, bar);
    if (base == 0 || length == 0) {
        return 0;
    }
    uint64_t addr = base + offset;
    if (!mmio_identity_map(addr, length)) {
        return 0;
    }
    return (volatile uint8_t *)(uintptr_t)addr;
}

static int already_bound(uint16_t pci_device) {
    for (int i = 0; i < binding_count; i++) {
        if (bindings[i].device == pci_device) {
            return 1;
        }
    }
    return 0;
}

static pci_device_t find_device(uint32_t device_id) {
    pci_device_t d;
    uint16_t modern = (uint16_t)(VIRTIO_PCI_MODERN_BASE + device_id);
    if (!already_bound(modern)) {
        d = pci_find(VIRTIO_PCI_VENDOR, modern);
        if (d.found) {
            return d;
        }
    }
    /* Transitional ids: net (0x1000), block (0x1001) and entropy (0x1005);
     * confirm by subsystem. QEMU's plain virtio-*-pci devices are these. */
    uint16_t legacy = 0;
    if (device_id == VIRTIO_ID_NET) {
        legacy = VIRTIO_PCI_LEGACY_NET;
    } else if (device_id == VIRTIO_ID_BLOCK) {
        legacy = VIRTIO_PCI_LEGACY_BLOCK;
    } else if (device_id == VIRTIO_ID_ENTROPY) {
        legacy = VIRTIO_PCI_LEGACY_ENTROPY;
    }
    if (legacy && !already_bound(legacy)) {
        d = pci_find(VIRTIO_PCI_VENDOR, legacy);
        if (d.found && (cfg32(&d, PCI_SUBSYSTEM) >> 16) == device_id) {
            return d;
        }
    }
    d.found = false;
    return d;
}

int virtio_pci_probe(uint32_t device_id, virtio_transport_t *out) {
    if (binding_count >= VIRTIO_MAX_BOUND) {
        return 0;
    }
    pci_device_t d = find_device(device_id);
    if (!d.found) {
        return 0;
    }
    if (!(cfg32(&d, PCI_STATUS_DWORD) & PCI_STATUS_CAP_LIST)) {
        boot_log("VIRTIO: virtio-pci device has no capability list (legacy-only) - unsupported");
        return 0;
    }

    /* Fill the next free binding; it is only kept (binding_count bumped)
     * once the device proved usable, so a failed probe leaves no trace. */
    pci_binding_t *b = &bindings[binding_count];
    volatile uint8_t *common = 0, *notify_base = 0, *isr = 0, *devcfg = 0;
    uint32_t notify_mult = 0;
    uint8_t ptr = cfg8(&d, PCI_CAP_PTR) & 0xFC;
    for (int guard = 0; ptr && guard < 48; guard++) {
        uint8_t id = cfg8(&d, ptr);
        uint8_t next = cfg8(&d, (uint8_t)(ptr + 1)) & 0xFC;
        if (id == PCI_CAP_ID_VNDR) {
            uint8_t type = cfg8(&d, (uint8_t)(ptr + 3));
            uint8_t bar = cfg8(&d, (uint8_t)(ptr + 4));
            uint32_t offset = cfg32(&d, (uint8_t)(ptr + 8));
            uint32_t length = cfg32(&d, (uint8_t)(ptr + 12));
            /* The first capability of each type is the preferred one. */
            if (type == CAP_COMMON && !common) {
                common = map_cap(&d, bar, offset, length);
            } else if (type == CAP_NOTIFY && !notify_base) {
                notify_base = map_cap(&d, bar, offset, length);
                notify_mult = cfg32(&d, (uint8_t)(ptr + 16));
            } else if (type == CAP_ISR && !isr) {
                isr = map_cap(&d, bar, offset, length);
            } else if (type == CAP_DEVICE && !devcfg) {
                devcfg = map_cap(&d, bar, offset, length);
            }
        }
        ptr = next;
    }
    if (!common || !notify_base || !isr || !devcfg) {
        boot_log("VIRTIO: virtio-pci modern capabilities incomplete - unsupported");
        return 0;
    }

    uint8_t irq = (uint8_t)(cfg32(&d, PCI_INTERRUPT_LINE) & 0xFF);
    if (irq >= 16) {
        boot_log("VIRTIO: virtio-pci has no usable IRQ line - device disabled");
        return 0;
    }

    pci_enable_bus_master_mem(&d);
    w16(common, C_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);

    b->common = common;
    b->notify_base = notify_base;
    b->notify_mult = notify_mult;
    b->isr = isr;
    b->devcfg = devcfg;
    for (int q = 0; q < MAX_QUEUES; q++) {
        b->notify_off[q] = 0;
    }
    b->device = d.device;
    b->pdev = d;
    b->msix_on = 0;
    binding_count++;

    out->name = "pci";
    out->dev = b;
    out->get_status = p_get_status;
    out->set_status = p_set_status;
    out->get_features = p_get_features;
    out->set_features = p_set_features;
    out->queue_setup = p_queue_setup;
    out->notify = p_notify;
    out->isr_ack = p_isr_ack;
    out->config_read8 = p_config_read8;
    out->msix_enable = p_msix_enable;
    out->irq = irq;
    return 1;
}
