#ifndef VIRTIO_RNG_H
#define VIRTIO_RNG_H

#include <kernel/types.h>

/* virtio entropy device (virtio-rng, device type 4) over virtio-mmio or
 * virtio-pci (#254). The bytes are the HOST's randomness, never quantum. */

/* Probe and bring up the device. Returns 1 when bound, 0 when absent or
 * unusable (silent when absent). Safe to call more than once. */
int virtio_rng_init(void);

/* 1 once a device is bound. */
int virtio_rng_present(void);

/* Fill `len` bytes of `buf` from the device. Returns 0 on success, -1 when
 * there is no device or it stopped answering (the device is then dropped). */
int virtio_rng_fill(void *buf, uint32_t len);

#endif /* VIRTIO_RNG_H */
