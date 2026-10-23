/**
 * QuantumOS virtio-pci transport (virtio 1.2 §4.1, modern layout).
 *
 * Finds the device on PCI bus 0 by its modern id (0x1040 + type, e.g.
 * 0x1041 for net) or its transitional id (0x1000-0x103f, matched by the
 * subsystem id), walks the vendor capabilities for the common, notify,
 * ISR and device-config structures, maps the memory BARs they live in,
 * and drives the device through them. Interrupts are the legacy INTx line
 * routed by firmware (config offset 0x3C) — MSI-X is left disabled
 * (vectors set to VIRTIO_MSI_NO_VECTOR), matching how every other device
 * in this kernel is wired to the 8259.
 *
 * The legacy I/O-BAR register layout is not used even on a transitional
 * device: its modern capabilities are always present and are what we
 * negotiate VERSION_1 through.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio.h>
#include <kernel/pci.h>
#include <kernel/mmio.h>
#include <kernel/boot.h>

#define VIRTIO_PCI_VENDOR 0x1AF4
#define VIRTIO_PCI_MODERN_BASE 0x1040
#define VIRTIO_PCI_LEGACY_NET 0x1000

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

static volatile uint8_t *common;
static volatile uint8_t *notify_base;
static uint32_t notify_mult;
static volatile uint8_t *isr;
static volatile uint8_t *devcfg;
static uint16_t notify_off[MAX_QUEUES];

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

/* Physical base of memory BAR `bar`, or 0 for an I/O BAR / bad index. */
static uint64_t bar_base(const pci_device_t *d, uint8_t bar) {
    if (bar > 5) {
        return 0;
    }
    uint32_t lo = cfg32(d, (uint8_t)(0x10 + 4 * bar));
    if (lo & 1) {
        return 0; /* I/O-space BAR: the modern structures never need one */
    }
    uint64_t base = lo & ~0xFULL;
    if (((lo >> 1) & 3) == 2 && bar < 5) {
        base |= (uint64_t)cfg32(d, (uint8_t)(0x10 + 4 * (bar + 1))) << 32;
    }
    return base;
}

/* ---- transport ops ---- */

static uint8_t p_get_status(void) {
    return r8(common, C_STATUS);
}

static void p_set_status(uint8_t s) {
    w8(common, C_STATUS, s);
}

static uint64_t p_get_features(void) {
    w32(common, C_DFSELECT, 0);
    uint64_t lo = r32(common, C_DF);
    w32(common, C_DFSELECT, 1);
    uint64_t hi = r32(common, C_DF);
    return lo | (hi << 32);
}

static void p_set_features(uint64_t f) {
    w32(common, C_GFSELECT, 0);
    w32(common, C_GF, (uint32_t)f);
    w32(common, C_GFSELECT, 1);
    w32(common, C_GF, (uint32_t)(f >> 32));
}

static int p_queue_setup(uint16_t index, uint16_t size, uint64_t desc, uint64_t driver,
                         uint64_t device) {
    if (index >= MAX_QUEUES || index >= r16(common, C_NUM_QUEUES)) {
        return 0;
    }
    w16(common, C_Q_SELECT, index);
    uint16_t max = r16(common, C_Q_SIZE);
    if (max == 0 || max < size || r16(common, C_Q_ENABLE) != 0) {
        return 0;
    }
    w16(common, C_Q_SIZE, size); /* a smaller power of two is allowed */
    w16(common, C_Q_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    w32(common, C_Q_DESC, (uint32_t)desc);
    w32(common, C_Q_DESC + 4, (uint32_t)(desc >> 32));
    w32(common, C_Q_DRIVER, (uint32_t)driver);
    w32(common, C_Q_DRIVER + 4, (uint32_t)(driver >> 32));
    w32(common, C_Q_DEVICE, (uint32_t)device);
    w32(common, C_Q_DEVICE + 4, (uint32_t)(device >> 32));
    notify_off[index] = r16(common, C_Q_NOTIFY_OFF);
    __asm__ volatile("" ::: "memory");
    w16(common, C_Q_ENABLE, 1);
    return 1;
}

static void p_notify(uint16_t index) {
    if (index >= MAX_QUEUES) {
        return;
    }
    w16(notify_base, (uint32_t)notify_off[index] * notify_mult, index);
}

static uint32_t p_isr_ack(void) {
    return r8(isr, 0); /* reading the ISR status acknowledges it (§4.1.4.5) */
}

static uint8_t p_config_read8(uint32_t off) {
    return r8(devcfg, off);
}

/* Map a capability's window and return its kernel address, or 0. */
static volatile uint8_t *map_cap(const pci_device_t *d, uint8_t bar, uint32_t offset,
                                 uint32_t length) {
    uint64_t base = bar_base(d, bar);
    if (base == 0 || length == 0) {
        return 0;
    }
    uint64_t addr = base + offset;
    if (!mmio_identity_map(addr, length)) {
        return 0;
    }
    return (volatile uint8_t *)(uintptr_t)addr;
}

static pci_device_t find_device(uint32_t device_id) {
    pci_device_t d = pci_find(VIRTIO_PCI_VENDOR, (uint16_t)(VIRTIO_PCI_MODERN_BASE + device_id));
    if (d.found) {
        return d;
    }
    /* Transitional ids: only net (0x1000) is in scope; confirm by subsystem. */
    if (device_id == VIRTIO_ID_NET) {
        d = pci_find(VIRTIO_PCI_VENDOR, VIRTIO_PCI_LEGACY_NET);
        if (d.found && (cfg32(&d, PCI_SUBSYSTEM) >> 16) == device_id) {
            return d;
        }
    }
    d.found = false;
    return d;
}

int virtio_pci_probe(uint32_t device_id, virtio_transport_t *out) {
    pci_device_t d = find_device(device_id);
    if (!d.found) {
        return 0;
    }
    if (!(cfg32(&d, PCI_STATUS_DWORD) & PCI_STATUS_CAP_LIST)) {
        boot_log("NET: virtio-pci device has no capability list (legacy-only) - unsupported");
        return 0;
    }

    common = notify_base = isr = devcfg = 0;
    notify_mult = 0;
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
        boot_log("NET: virtio-pci modern capabilities incomplete - unsupported");
        return 0;
    }

    uint8_t irq = (uint8_t)(cfg32(&d, PCI_INTERRUPT_LINE) & 0xFF);
    if (irq >= 16) {
        boot_log("NET: virtio-pci has no usable IRQ line - networking disabled");
        return 0;
    }

    pci_enable_bus_master_mem(&d);
    w16(common, C_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);

    out->name = "pci";
    out->get_status = p_get_status;
    out->set_status = p_set_status;
    out->get_features = p_get_features;
    out->set_features = p_set_features;
    out->queue_setup = p_queue_setup;
    out->notify = p_notify;
    out->isr_ack = p_isr_ack;
    out->config_read8 = p_config_read8;
    out->irq = irq;
    return 1;
}
