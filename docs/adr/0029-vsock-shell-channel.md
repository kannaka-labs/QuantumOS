# 29. A virtio-vsock Channel for the Shell, with COM1 Kept as the Log and the Fallback

Date: 2026-10-26
Status: Accepted (2026-10-26, Nick: "yes"; #259)

## Context

On the public console, typing is fast but output is slow. #259 measured it with
`scripts/bench_console_output.py`, the time from sending `help` (1521 bytes) to the next
prompt, 10 runs each:

| | p50 | ms/byte |
|---|---|---|
| Firecracker v1.16.1 on debain2 (itself a KVM guest, so nested; same as the console host) | 1384 ms | 0.91 |
| QEMU TCG, same kernel | 16 ms | 0.01 |

Five kernel-side variants on Firecracker made no difference (1350 to 1435 ms):
- no line-status poll before each byte, which halves the port I/O;
- one `rep outsb` for the whole buffer;
- the VGA text console off;
- the VGA cursor sync off;
- the baseline.

A host-side timeline shows why: the 1531 bytes of one `help` arrive as 1529 separate one-byte
reads, evenly paced at 0.2 to 0.5 ms. The cost is per byte, in how KVM and Firecracker's
emulated 16550 deliver serial output on a nested host. Nothing in the guest's loop reaches it.
Firecracker has no virtio-console. Its one paravirtual stream device is **virtio-vsock**,
which moves a whole buffer per notification.

Constraints:
- **The console restores every session from a snapshot** (ADR-0025). On restore,
  Firecracker drops all vsock connections and sends the guest
  `VIRTIO_VSOCK_EVENT_TRANSPORT_RESET`.
- **COM1 is load-bearing.** Every CI gate, `qos_bridge.py`, the MCP server, the escape-coded
  reseed and QPU relay (ADR-0025, #269), bare metal and QEMU all use it.
- **Firecracker's vsock is host-initiated through a Unix socket.** The host connects to the
  device's `uds_path` and writes `CONNECT <port>\n`. Firecracker answers `OK <host port>\n`,
  and the stream is spliced to a guest listener on that port.

## Decision

Add a virtio-vsock device driver and a shell stream over it. COM1 keeps doing what it does.

1. **Driver: `kernel/src/virtio_vsock.c`,** virtio device type 19, over the existing
   `virtio_transport_t` (ADR-0024). Firecracker exposes it over MMIO; QEMU's
   `vhost-vsock-pci` covers PCI. It uses three queues: rx, tx and event. It implements the
   stream subset of virtio 1.2 §5.10:
   - the 44-byte `virtio_vsock_hdr`;
   - `REQUEST` / `RESPONSE` / `RW` / `CREDIT_UPDATE` / `CREDIT_REQUEST` / `SHUTDOWN` / `RST`;
   - credit flow control (`buf_alloc`, `fwd_cnt`);
   - the transport-reset event.

   It supports **one listening port and one connection.** There is no socket API for ring 3
   in this ADR.

2. **Guest side: the shell's output follows its input.** While no vsock peer is connected on
   the shell port (default 1024), nothing changes: `console_write` writes COM1 and the screen.
   While a peer **is** connected:
   - **Shell output** (`SYS_WRITE` from the console holder) goes to the vsock tx queue, a
     whole `console_write` buffer per packet, and **not** to COM1.
   - **Input from vsock** feeds the same RX ring the COM1 IRQ fills. So qsh, `SYS_CONS_READ`
     and the escape parsers (reseed, QPU relay) see one input stream, whichever wire it came
     in on.
   - **The kernel log** (`boot_log`, `early_console_write`) stays on COM1 always. So the host
     keeps a full log, and a fault while a peer is attached is still visible.

   When the peer disconnects or the transport resets, output returns to COM1 at once.

3. **Why mute COM1 for the shell rather than tee.** Teeing would leave the guest paying
   ~0.9 ms per byte for the COM1 copy, and qsh's next prompt would wait for it. That is the
   whole cost this ADR exists to remove. Muting only while a peer is attached breaks nothing:
   - **Every COM1 consumer never connects a vsock peer, so it sees exactly what it sees
     today.** That covers the CI gates, `qos_bridge.py`, the MCP server, QEMU and bare metal.
   - **Only the console launcher connects, and it reads the shell from vsock.**

4. **Host side: `tools/qos-console/qos_console.py`.**
   - The VM config gains `"vsock": {"guest_cid": 3, "uds_path": "v.sock"}`. The path is
     relative, so each restored VM gets its own socket in its run directory, which is
     Firecracker's cwd.
   - After a restore, the launcher connects with `CONNECT 1024`, retrying until the guest's
     listener is back after the transport reset.
   - It bridges the websocket to that stream instead of to COM1 stdout. If the connect fails,
     it falls back to COM1 as today, and the session log says which path was used.

5. **The snapshot is taken with the listener armed.** The guest re-arms after the reset
   event. The launcher's connect retry covers the window.

## Consequences

- The cost per output batch becomes one virtqueue notification instead of one exit per
  byte. **Expected, not yet measured:** `help` in tens of milliseconds on the console host.
  The first slice's PR must post the same benchmark (#259's table, with a vsock row) before
  the launcher switches by default.
- **New attack surface.** A virtio-vsock driver parses host-written headers. The host is the
  hypervisor and is already trusted with COM1 and memory, but the parser must still be
  bounds-checked:
  - `len` against the buffer;
  - `op` against the known set;
  - the connection's `dst_port` against the one listener; anything else gets `RST`.
- **Two input wires can race.** Bytes from COM1 and vsock interleave in one ring in arrival
  order. In practice only one is attached at a time; the launcher uses one or the other.
- **Not covered:** guest-initiated connections, multiple ports, a ring-3 socket API, and
  vsock on bare metal (there is none). The bare-metal and QEMU shell keeps COM1 and is
  unchanged.

## Testing

- **QEMU:** `-device vhost-vsock-pci,guest-cid=3` needs the host `vhost_vsock` module. CI
  runners may not have it, so the PCI path is checked by hand on debain2.
- **Firecracker:** the `firecracker-boot` job (#260/#313) already boots with KVM. A second leg
  adds the vsock device, connects through `uds_path`, sends `help`, and asserts:
  - the output arrives over vsock;
  - it arrives in under 200 ms on the runner;
  - the snapshot/restore reconnect works.
- **Non-vacuous (ADR-0016):** with the guest listener disabled, the leg must fail. It must
  not fall back to COM1 silently. The fallback is the launcher's behaviour, not the gate's.

## Alternatives considered

- **Fewer exits per byte on COM1** (#259 option 2). Measured, no effect. Rejected.
- **A non-nested console host** (option 1). It may cut the per-exit cost, but it changes
  hosting, not the design, and it was not measurable here. It is independent of this ADR.
- **virtio-console.** Firecracker does not offer it.
- **Print less** (option 4). Cheap, but `help` itself is the slow case.
