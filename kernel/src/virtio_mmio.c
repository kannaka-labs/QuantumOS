/**
 * QuantumOS virtio-mmio transport (virtio 1.2 §4.2, modern / Version 2).
 *
 * There is no bus to walk: the hypervisor names each device on the kernel
 * command line (`virtio_mmio.device=4K@0xd0000000:5`, recorded through
 * virtio_mmio_add_device()). The probe maps each recorded window, checks
 * the magic, the version and the device id, and binds the first match
 * that is not already bound. Each slot is its own binding (its window is
 * the binding's `dev`), so the NIC and the disk coexist.
 *
 * The window is mapped before the first register read: Firecracker puts
 * devices at 0xd0000000, above the 1 GB the boot tables identity-map, so
 * an unmapped read is a page fault (the pre-ADR-0024 driver read it
 * without a mapping and could never have run).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/virtio.h>
#include <kernel/mmio.h>
#include <kernel/boot.h>

/* Register offsets (§4.2.2) */
#define VM_MAGIC 0x000
#define VM_VERSION 0x004
#define VM_DEVICE_ID 0x008
#define VM_DEVICE_FEATURES 0x010
#define VM_DEVICE_FEATURES_SEL 0x014
#define VM_DRIVER_FEATURES 0x020
#define VM_DRIVER_FEATURES_SEL 0x024
#define VM_QUEUE_SEL 0x030
#define VM_QUEUE_NUM_MAX 0x034
#define VM_QUEUE_NUM 0x038
#define VM_QUEUE_READY 0x044
#define VM_QUEUE_NOTIFY 0x050
#define VM_INTERRUPT_STATUS 0x060
#define VM_INTERRUPT_ACK 0x064
#define VM_STATUS 0x070
#define VM_QUEUE_DESC_LOW 0x080
#define VM_QUEUE_DESC_HIGH 0x084
#define VM_QUEUE_DRIVER_LOW 0x090
#define VM_QUEUE_DRIVER_HIGH 0x094
#define VM_QUEUE_DEVICE_LOW 0x0a0
#define VM_QUEUE_DEVICE_HIGH 0x0a4
#define VM_CONFIG 0x100

#define VIRTIO_MAGIC 0x74726976u /* "virt" */

/* Most slots any one guest is given. Firecracker's default is well under
 * this; the array is static so the probe needs no allocator. */
#define VIRTIO_MMIO_MAX_DEVICES 8

typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t irq;
    int bound; /* a driver owns this slot */
} mmio_slot_t;

static mmio_slot_t slots[VIRTIO_MMIO_MAX_DEVICES];
static int slot_count;

/* A binding's `dev` is its register window. */
static inline volatile uint8_t *regs(const virtio_transport_t *t) {
    return (volatile uint8_t *)t->dev;
}

static inline uint32_t rd32(volatile uint8_t *reg, uint32_t off) {
    return *(volatile uint32_t *)(reg + off);
}

static inline uint32_t r32(const virtio_transport_t *t, uint32_t off) {
    return rd32(regs(t), off);
}

static inline void w32(const virtio_transport_t *t, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(regs(t) + off) = v;
}

int virtio_mmio_add_device(uint64_t base, uint64_t size, uint8_t irq) {
    if (slot_count >= VIRTIO_MMIO_MAX_DEVICES || base == 0) {
        return 0;
    }
    if (irq >= 16) {
        /* No usable 8259 line means RX can never fire (the rtl8139 lesson). */
        return 0;
    }
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].base == base) {
            return 1; /* idempotent */
        }
    }
    slots[slot_count].base = base;
    slots[slot_count].size = size;
    slots[slot_count].irq = irq;
    slots[slot_count].bound = 0;
    slot_count++;
    return 1;
}

int virtio_mmio_device_count(void) {
    return slot_count;
}

/* ---- transport ops ---- */

static uint8_t m_get_status(const virtio_transport_t *t) {
    return (uint8_t)r32(t, VM_STATUS);
}

static void m_set_status(const virtio_transport_t *t, uint8_t s) {
    w32(t, VM_STATUS, s);
}

static uint64_t m_get_features(const virtio_transport_t *t) {
    w32(t, VM_DEVICE_FEATURES_SEL, 0);
    uint64_t lo = r32(t, VM_DEVICE_FEATURES);
    w32(t, VM_DEVICE_FEATURES_SEL, 1);
    uint64_t hi = r32(t, VM_DEVICE_FEATURES);
    return lo | (hi << 32);
}

static void m_set_features(const virtio_transport_t *t, uint64_t f) {
    w32(t, VM_DRIVER_FEATURES_SEL, 0);
    w32(t, VM_DRIVER_FEATURES, (uint32_t)f);
    w32(t, VM_DRIVER_FEATURES_SEL, 1);
    w32(t, VM_DRIVER_FEATURES, (uint32_t)(f >> 32));
}

static int m_queue_setup(const virtio_transport_t *t, uint16_t index, uint16_t size, uint64_t desc,
                         uint64_t driver, uint64_t device) {
    w32(t, VM_QUEUE_SEL, index);
    if (r32(t, VM_QUEUE_READY) != 0) {
        return 0;
    }
    uint32_t max = r32(t, VM_QUEUE_NUM_MAX);
    if (max == 0 || max < size) {
        return 0;
    }
    w32(t, VM_QUEUE_NUM, size);
    w32(t, VM_QUEUE_DESC_LOW, (uint32_t)desc);
    w32(t, VM_QUEUE_DESC_HIGH, (uint32_t)(desc >> 32));
    w32(t, VM_QUEUE_DRIVER_LOW, (uint32_t)driver);
    w32(t, VM_QUEUE_DRIVER_HIGH, (uint32_t)(driver >> 32));
    w32(t, VM_QUEUE_DEVICE_LOW, (uint32_t)device);
    w32(t, VM_QUEUE_DEVICE_HIGH, (uint32_t)(device >> 32));
    __asm__ volatile("" ::: "memory");
    w32(t, VM_QUEUE_READY, 1);
    return 1;
}

static void m_notify(const virtio_transport_t *t, uint16_t index) {
    w32(t, VM_QUEUE_NOTIFY, index);
}

static uint32_t m_isr_ack(const virtio_transport_t *t) {
    uint32_t st = r32(t, VM_INTERRUPT_STATUS);
    if (st) {
        w32(t, VM_INTERRUPT_ACK, st);
    }
    return st;
}

static uint8_t m_config_read8(const virtio_transport_t *t, uint32_t off) {
    return *(volatile uint8_t *)(regs(t) + VM_CONFIG + off);
}

int virtio_mmio_probe(uint32_t device_id, virtio_transport_t *out) {
    for (int s = 0; s < slot_count; s++) {
        if (slots[s].bound) {
            continue; /* another driver's device */
        }
        uint64_t size = slots[s].size ? slots[s].size : 0x1000;
        if (!mmio_identity_map(slots[s].base, size)) {
            continue;
        }
        volatile uint8_t *reg = (volatile uint8_t *)(uintptr_t)slots[s].base;
        if (rd32(reg, VM_MAGIC) != VIRTIO_MAGIC || rd32(reg, VM_VERSION) != 2 ||
            rd32(reg, VM_DEVICE_ID) != device_id) {
            continue; /* not virtio, legacy (v1), or another device type */
        }
        slots[s].bound = 1;
        out->name = "mmio";
        out->dev = (void *)(uintptr_t)reg;
        out->get_status = m_get_status;
        out->set_status = m_set_status;
        out->get_features = m_get_features;
        out->set_features = m_set_features;
        out->queue_setup = m_queue_setup;
        out->notify = m_notify;
        out->isr_ack = m_isr_ack;
        out->config_read8 = m_config_read8;
        out->msix_enable = 0; /* virtio-mmio has one line and no MSI-X */
        out->irq = slots[s].irq;
        return 1;
    }
    return 0;
}
