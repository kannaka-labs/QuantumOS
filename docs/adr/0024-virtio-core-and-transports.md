# 24. virtio-net: One Device Driver over virtio-pci and virtio-mmio

Date: 2026-10-22
Status: Accepted (2026-10-22; PCI and MMIO transports shipped together, CI gate
`ci-smoke-net-virtio`)

## Context

QuantumOS had one emulated NIC driver, the rtl8139 (epic #73), and since PR #237 a
virtio-net driver hard-wired to the virtio-mmio register block, for Firecracker. Every
production hypervisor (QEMU/KVM, the clouds, Firecracker) offers virtio-net as its
paravirtual NIC, and most offer it over PCI. The rtl8139 is an emulated 1990s part:
QEMU has to model its register file and a 64 KiB copy ring, and nothing in a cloud
offers one.

Two latent bugs blocked the MMIO path on a real Firecracker guest:

1. **Device windows above 1 GB were not mapped.** Boot paging identity-maps only the
   low 1 GB (ADR-0021). Firecracker places virtio-mmio slots at `0xd0000000` (3.25 GB),
   and QEMU puts PCI memory BARs above 1 GB as well. The first register read
   page-faulted.
2. **A missing COM2 froze the guest.** Firecracker returns 0 for reads of an unbacked
   I/O port, where QEMU returns 0xFF. `LSR=0` means "THR never empties", so the swarm
   bridge's first `SYS_COM2` write spun 2,000,000 polls (each one a VM exit) with
   interrupts off. The guest stopped servicing interrupts, the NIC included, for tens
   of seconds. This is also why a NIC-less Firecracker boot took 75 s to reach the
   qsh prompt. With the check below it takes 1.6 s (measured on debain2: main's
   kernel 75.5 s, main plus only this fix 1.6 s).

## Decision

**One device driver, two transports.** `kernel/include/kernel/virtio.h` defines
`virtio_transport_t`: status, 64-bit features, `queue_setup(index, size, desc, driver,
device)`, `notify`, `isr_ack`, `config_read8`, and an INTx line. Two transports
implement it:

- `virtio_mmio.c`: virtio-mmio v2 (virtio 1.2 §4.2). Slots come from the
  `virtio_mmio.device=` cmdline tokens Firecracker appends.
- `virtio_pci.c`: modern virtio-pci (§4.1). It finds the device on bus 0 by its
  modern id (0x1041) or its transitional id (0x1000, confirmed by subsystem id). It
  walks the vendor capabilities (common, notify, ISR, device config), maps the BAR
  windows they name, and enables memory decoding and bus mastering. MSI-X stays off:
  every vector is set to `VIRTIO_MSI_NO_VECTOR`, so the device raises the INTx line
  firmware routed to the 8259, like every other device in this kernel. Legacy-only
  devices (no capability list) are refused, not half-driven.

`virtio_net.c` is transport-agnostic and follows the §3.1.1 initialisation sequence in
order:

1. Reset, ACKNOWLEDGE, DRIVER.
2. Feature negotiation. It requires `VERSION_1` and takes `MAC` and `STATUS` only if
   offered. It takes no offloads: checksums are computed in software, so `CSUM`,
   `GUEST_CSUM`, TSO and `MRG_RXBUF` are deliberately left off.
3. FEATURES_OK, then a read-back check.
4. Two split virtqueues of 64 entries in kmalloc memory (identity-mapped, so VA == PA),
   with compiler barriers around every avail/used index update.
5. 64 receive buffers posted, then DRIVER_OK.

Every frame carries the 12-byte `virtio_net_hdr` that VERSION_1 mandates. Receive is
interrupt-driven: the IRQ reads or acknowledges the ISR and drains the used ring into
a software queue. `receive()` falls back to draining the ring itself, with interrupts
off, when that queue is empty, so a lost or unrouted interrupt degrades to polling
and never to silence. Transmit is a single descriptor polled to completion with a
bounded wait, the rtl8139 discipline.

**Probe order:** virtio first (MMIO slots, then PCI), rtl8139 as the fallback. One
log line names the driver, the transport and the MAC:
`NET: virtio-net up (pci) - MAC: ...` or `(mmio)`.

**Device windows above 1 GB:** `mmio_identity_map(base, len)` (`kernel/src/mmio.c`)
installs uncached (PCD|PWT) 2 MB identity mappings into the boot page tables from a
small static pool. It never touches PDPT[1] (the per-process user half) and never
touches the upper canonical half. `vmspace_create` copies the shared kernel device
entries into every new address space without `PG_USER`, so ring 3 cannot reach them.
`vmspace_destroy` skips any entry without `PG_USER`, so a process exit never frees a
shared table. Mapping is done at probe time, before the first user process exists.

**COM2 absence:** `com2_init` writes two patterns to the scratch register and reads
them back. If no UART answers, writes are dropped (as on an unbacked QEMU port) and
reads return nothing, without touching the port.

## Consequences

- **CI:** `ci-smoke-net-virtio` runs the full `ci-smoke-net` and `ci-smoke-http` gates
  with `-device virtio-net-pci`: ARP, DHCP, ICMP to the gateway, DNS, and HTTP 200 from
  both a loopback server and example.com. The rtl8139 gates are unchanged, and the NIC
  model is a Makefile variable (`NET_NIC`, `NET_UP`).
- **Verified on Firecracker v1.16.1:** the guest binds virtio-mmio, answers ARP from
  the host's tap, and fetches `HTTP/1.0 200 OK` from a host `http.server`. The MMIO
  path has no CI gate: QEMU's `microvm` machine cannot load the PVH image (its loader
  finds the Multiboot header first), and CI runners have no KVM for Firecracker.
- **Not done, in order of value:** multiqueue (`MQ`), which needs SMP to matter
  (ADR-0005); TSO/GSO, which has nothing to offload until the TCP stack builds segments
  larger than the MSS. Checksum offload, `MRG_RXBUF` and MSI-X landed in #253 (amendment
  below). (The e1000 driver for hypervisors and bare metal that offer neither virtio nor
  an rtl8139 landed in #252, behind the same netdev seam.)
  Packed virtqueues and `EVENT_IDX` are throughput work this single-core kernel does
  not yet need.

## Amendment (#255): several devices per transport

The transport op table now takes the binding it acts on, and `dev` holds the
transport's per-device state (a virtio-mmio slot's register window, a
virtio-pci function's capability windows). Each transport binds up to
`VIRTIO_MAX_BOUND` devices, and a probe skips devices already bound. The
first second device is virtio-blk; without this, its probe would have
re-pointed the transport's single register global away from the NIC.

## Amendment (#253): checksum offload, merged RX buffers, MSI-X

Three features are now negotiated when offered, each with its own cmdline switch
(`virtio_net.nofeat=csum,mrgrx,msix`) and its own bring-up line
(`NET: virtio-net feature <name>: negotiated | off-by-cmdline | not-offered`), so a
regression bisects without a rebuild. Step 2 of the sequence above reads: VERSION_1
required; MAC, STATUS, CSUM (+GUEST_CSUM), MRG_RXBUF taken if offered and allowed.

- **CSUM / GUEST_CSUM.** TX stores the TCP pseudo-header sum and marks the frame
  `NEEDS_CSUM`; the device finishes it. The netdev ops table is unchanged: the stack
  learns the rule through `netdev_set_tx_csum_offload()` / `netdev_tx_csum_offload()`
  (driver-set state in `netdev.c`), and only TCP, the one protocol it checksums, is
  marked. RX frames the device verified (`DATA_VALID`) skip the stack's check through
  `netdev_rx_csum_valid_set()` / `netdev_rx_csum_valid()`, cleared per call; frames
  the device left `NEEDS_CSUM` are finished in the driver. QEMU offers neither bit on
  user-net (SLIRP has no virtio-net header), so there the driver reports
  `not-offered` and the software path runs; over a tap both are negotiated.
- **MRG_RXBUF.** `num_buffers` consecutive used entries make one frame; RX buffers
  become 1 KiB chunks (64 KiB of RX memory instead of 100 KiB), so the merge path
  runs on every full-size segment instead of never. Malformed counts (past
  `used->idx`) recycle what is there and drop the frame.
- **MSI-X (virtio-pci only).** `kernel/src/pci_msix.c` parses capability 0x11, maps
  the table, points `queues + 1` entries at the boot CPU's local APIC (IDT vectors
  64..67), enables MSI-X and disables INTx. The local APIC is software-enabled for
  the purpose, with LINT0 kept as the 8259's ExtINT path, so every other device is
  untouched. The transport grew an optional `msix_enable` op (NULL on virtio-mmio);
  `queue_setup` stamps vector `1 + q` and reads it back. INTx is the fallback whenever
  the capability is missing or the switch is off. Because `receive()` polls the used
  ring when its queue is empty, a dead MSI-X would still pass the fetch gates; the RX
  handler therefore logs `first MSI-X rx interrupt` once, and the gates assert it.

Gates: `ci-smoke-net-virtio` (offloads on: MRG_RXBUF + MSI-X asserted, first interrupt
asserted, csum honestly `not-offered`, plus a 64 KiB fetch that exercises merging),
`ci-smoke-net-virtio-nofeat` (all three `off-by-cmdline`, INTx, no MSI-X line, same
proofs), `ci-smoke-net-virtio-tap` (host tap: `csum: negotiated`, 64 KiB fetch with
device-completed TX checksums). Negative controls and the before/after fetch timings
are in PR #253's body. TX completion now asks for no interrupt
(`VRING_AVAIL_F_NO_INTERRUPT`): it was always observed by the poll.
