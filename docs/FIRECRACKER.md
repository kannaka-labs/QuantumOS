# QuantumOS on Firecracker

QuantumOS boots as a Firecracker microVM through the **PVH boot protocol**
(x86_64; verified with Firecracker v1.16.1 on a KVM host, 2026-09-06).

Firecracker cannot load Multiboot images. `kernel/src/boot.S` therefore carries a
`.note.Xen` ELF note (`XEN_ELFNOTE_PHYS32_ENTRY`) pointing at `_pvh_start`, a 32-bit
stub that reads `hvm_start_info` and synthesizes the Multiboot1 info block the kernel
already understands (memory map, command line, mem_lower/mem_upper), then joins the
normal `_start` path. The kernel stays Multiboot1-only (ADR-0021); nothing above the
boot stub changes. `kernel/link.ld` keeps the note in its own `PT_NOTE` segment.

## Run

Use the **ELF64** image (`build/x86_64/kernel.elf`); the `.elf32` copy is for GRUB and
`qemu -kernel`, which cannot take this path (QEMU's loader finds the Multiboot header
first and refuses 64-bit images).

```json
{
  "boot-source": { "kernel_image_path": "/path/to/build/x86_64/kernel.elf", "boot_args": "quiet" },
  "drives": [],
  "machine-config": { "vcpu_count": 1, "mem_size_mib": 128 },
  "logger": { "level": "Error", "log_path": "/tmp/qos-fc.log" }
}
```

```sh
firecracker --no-api --config-file qos.json     # serial console (COM1) on stdio
```

Boot reaches `qsh` in about a minute on a CPU-only host; the holographic field works
inside the microVM:

```
qsh> imprint a cat sat on the mat
FIELD: imprinted slot 0
qsh> recall a cxt sxt on thx mat
FIELD: winner="a cat sat on the mat" slot=0 score=15975 n=1
```

## What the microVM does not have

Firecracker exposes no VGA, no PCI and no ATA, and only virtio-mmio devices. So:
the text-mode console writes to `0xB8000` land in plain RAM (harmless); the `rtl8139`
NIC and ATA disk are absent (`httpd: no network -- idle`); COM2 is unbacked (the
swarm bridge stays idle). Firecracker's logger reports the VGA CRTC and COM2 port
writes as `MissingAddressRange` at `Warning` level, which is why the config above
sets `Error`. Networking and storage inside Firecracker need virtio-mmio drivers,
which QuantumOS does not have yet.

## Public console

`tools/qos-console/` serves QuantumOS to the web: each visitor gets a fresh microVM
booted from this same `kernel.elf` (no drives, no NIC, serial console bridged to a
WebSocket), deleted on disconnect or after ten minutes. See its README for the
launcher, the outbound tunnel to the public edge, the caps and the deployment.
