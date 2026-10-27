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

Boot reaches the `qsh` prompt in about 1.6 s (it took about 75 s before the COM2
absence check below); the holographic field works
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
NIC and ATA disk are absent; COM2 is unbacked. Firecracker returns 0 (not 0xFF) for
reads of a missing port, so `com2_init` detects the absence with the scratch register
and the swarm bridge stays idle without touching the port. Before that check, the
first COM2 write spun its full drain bound in VM exits with interrupts off and froze
the guest for tens of seconds. Firecracker's logger reports the VGA CRTC writes as
`MissingAddressRange` at `Warning` level, which is why the config above sets `Error`.

## Storage (virtio-blk over virtio-mmio, #255)

Give the microVM a drive and QuantumOS binds it as its disk:

```json
  "drives": [{"drive_id": "data", "path_on_host": "/srv/qos/data.img",
              "is_root_device": false, "is_read_only": false}],
```

A zero-filled image is a fresh volume: `sync` writes the QDSK archive, the
field and the audit ledger to it, and the next boot restores them. Boot lines:

```
[BOOT] BLK: virtio-blk up (mmio), sectors: 0x2000
[BOOT] BLK: device offers no FLUSH - writes are durable only as the host makes them
[BOOT] BLK: sector RW self-test OK (pattern verified, original restored)
```

The FLUSH line appears with Firecracker's default `cache_type` ("Unsafe"); set
`"cache_type": "Writeback"` on the drive to have the guest's flush reach the host.
A drive and a NIC work together (each virtio-mmio slot is its own binding).

## Networking (virtio-net over virtio-mmio, ADR-0024)

Give the microVM a tap device and QuantumOS binds it as virtio-net:

```json
  "network-interfaces": [
    { "iface_id": "eth0", "guest_mac": "02:fc:00:00:00:15", "host_dev_name": "qostap0" }
  ]
```

Firecracker appends `virtio_mmio.device=4K@0xd0000000:5` to the command line, and the
boot log shows `NET: virtio-net up (mmio) - MAC: ...`. There is no DHCP server on a
bare tap, so pass a static address in `boot_args` (`ip=10.0.2.15`). The stack expects
the gateway at 10.0.2.2, so put that on the host end of the tap:

```sh
sudo ip tuntap add qostap0 mode tap user "$(id -un)"
sudo ip addr add 10.0.2.2/24 dev qostap0 && sudo ip link set qostap0 up
# ... boot; then in qsh:  http 10.0.2.2 8000   (a host-side http.server)
sudo ip link del qostap0
```

Verified 2026-10-22 on Firecracker v1.16.1 (debain2): `qsh: http 10.0.2.2 -> HTTP/1.0 200 OK`
from a host `python3 -m http.server`, and fieldsyncd's UDP to `peer=10.0.2.2` on the
tap. QuantumOS does not answer ICMP echo, so test it with traffic it starts, not `ping`.

## Shell over vsock (ADR-0029)

Under Firecracker, and nested Firecracker especially, every byte written to the emulated
16550 costs about 0.9 ms on the host side, so `help` (1.5 KB) took ~1.4 s to appear
(#259). With a vsock device the shell can use a batched channel instead:

```json
"vsock": {"guest_cid": 3, "uds_path": "v.sock"}
```

The kernel (`kernel/src/virtio_vsock.c`) binds it over virtio-mmio and listens on
**port 1024**. The host connects through the device's Unix socket with Firecracker's
protocol: write `CONNECT 1024\n`, read `OK <host port>\n`. Then:

- **While a host peer is connected,** the shell's output (`SYS_CONS_WRITE`) goes to vsock
  and not to COM1, and bytes the host writes arrive as console input. The input goes
  through the same escape parsing as COM1, including the session reseed.
- **The kernel log** (`[BOOT]`, `[user pid=...]`) stays on COM1 always.
- **When the peer leaves,** or a snapshot restore resets the transport, the shell is back
  on COM1 and the listener stays armed.
- **Nothing is lost:** output that vsock cannot take in time (no credit, device gone) is
  written to COM1 instead.

A relative `uds_path` is opened against Firecracker's cwd, so every VM restored from one
snapshot listens in its own directory. Measured on debain2 (nested, like the console
host): `help` to the next prompt, p50 1384 ms over COM1, 20 ms over vsock
(`scripts/ci_firecracker_vsock.py`, also run in CI). The public console uses it with
`--vsock` (tools/qos-console).

## Public console

`tools/qos-console/` serves QuantumOS to the web: each visitor gets a fresh microVM
booted from this same `kernel.elf` (no drives, no NIC, serial console bridged to a
WebSocket), deleted on disconnect or after ten minutes. See its README for the
launcher, the outbound tunnel to the public edge, the caps and the deployment.
