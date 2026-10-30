# Boot options: the kernel command line and the console escapes

Everything a host can tell a QuantumOS guest, in one place. Tokens come from the
Multiboot command line (`-append` under QEMU, `boot_args` under Firecracker, the GRUB
entry on the ISO); escapes are sequences a host writes into the guest's console (COM1,
or vsock when a peer is attached) after boot. All of them are parsed in
`kernel/src/main.c` (`parse_cmdline`) and `kernel/src/console.c` (the `ESC ]` parser);
the file:line anchors below are where to look when something does not take.

## Command-line tokens

Tokens are matched only at whitespace boundaries, so a value can never be mistaken for
a substring of another token. Unknown tokens are ignored. A malformed value is logged
and skipped; it never stops the boot.

| token | what it does | where |
|---|---|---|
| `quiet` | Silence the demo kernel's steady-state chatter (the oscillator and tick lines) so the console is a clean prompt. The boot log and `[user pid=...]` lines still print. | `main.c` (`g_boot_quiet`), gated by `make ci-smoke-quiet` |
| `qseed=<up to 16 hex digits>` | Seed the kernel's random pool with entropy the host measured, typically from a real QPU (`qos-qbraid-boot`). `qseed` in the shell shows the seed and its provenance line; without this token the pool is seeded from the host's randomness. | `main.c` (`qseed=` scan), `tools/qpu-broker`, ADR-0025 |
| `egress=<spec>` | **Networking is a grant** (ADR-0030). A present NIC without this token binds and does nothing (`NET: NIC present, no egress grant`). The spec is `ttl=<s>;bytes=<n>;allow=<host:port/proto>,...;listen=<port>,...`, for example `egress=ttl=600;bytes=4194304;allow=10.0.2.2:53/udp,10.0.2.2:8000/tcp;listen=8080`. A call outside the grant fails with `SYS_EEGRESS` and every use or refusal is an `AUDIT_EGRESS` row (`qsh egress`). A bad spec is refused and the boot stays offline. | `main.c` → `egress_configure`, `docs/NETWORKING.md` (grammar and examples) |
| `egress_key=<16 hex>` | The key a host must present to grant egress *after* boot over the console escape (below). Never logged, and not readable from ring 3. | `main.c` → `egress_set_key` |
| `ip=A.B.C.D` | Static IPv4 address; DHCP is skipped. Needed on a raw L2 with no DHCP server (two guests coupling, a Firecracker tap). The stack expects the gateway at `10.0.2.2`. | `main.c` → `net_set_static_ip`, epic #97 |
| `peer=A.B.C.D[,A.B.C.D...]` | The field-coupling partner(s) for `fieldsyncd`: two or more kernels synchronize one oscillator field over UDP. A comma-separated list names several peers (the N-way society gates). | `main.c` (`cmdline_get_peers`), ADR-0014, `docs/RUNTIME.md` |
| `virtio_mmio.device=<size>@<base>:<irq>` | Where a virtio-mmio device lives, one token per device; a hypervisor with no discoverable bus (Firecracker) emits these itself (`4K@0xd0000000:5`). Size takes a K or M suffix; base is hex or decimal. This token *is* device discovery on such hosts. | `main.c` (`cmdline_get_virtio_mmio`), ADR-0024, `docs/FIRECRACKER.md` |
| `virtio_net.nofeat=csum,mrgrx,msix` | Leave the named virtio-net offloads out of feature negotiation, so a regression bisects without a rebuild. Any subset, comma-separated. | `main.c` → `virtio_net_set_nofeat`, #253, gated by `ci-smoke-net-virtio-nofeat` |
| `agentdemo` | Run the `agentd` end-to-end demonstration at boot: the orchestrator delegates narrowed field capabilities to `/bin/agentsub` sub-agents it spawns off the initrd (epic #175). Off by default; the society gates turn it on. | `main.c` (`g_boot_agent_demo`) |

Things that are *not* tokens, and where they are set instead: RAM comes from the
Multiboot memory map (capped at the 1 GB identity map); the console device is whatever
the machine offers (COM1, the VGA text console, a GOP framebuffer under UEFI, vsock when a
peer connects); the scheduler experiments are build flags (`make ... SCHED=...`), not boot
options.

## Console escapes (host → guest after boot)

An `ESC ] <name> ; <fields...> BEL` sequence written into the guest's console input is
consumed by the kernel and never reaches the shell. They exist so a launcher can hand a
guest something only the host knows, after the kernel is already up.

| escape | what it does | where |
|---|---|---|
| `ESC ] qseed ; <16 hex> BEL` | Reseed the random pool for this session. The public console sends 8 fresh random bytes to every visitor's VM, because all of them are restored from one snapshot and would otherwise share a seed. | `console.c`, ADR-0025, `tools/qos-console/README.md` |
| `ESC ] egress ; <key> ; <spec> BEL` | Grant egress after boot. `<key>` must equal the boot line's `egress_key=`; `<spec>` is the same grammar as `egress=`. The net thread waits offline for this instead of giving up. | `console.c` → egress, #327, `docs/NETWORKING.md` |
| `ESC ] qpu ; S ; <tag> ; s ; <shots> ; <circuit hex> BEL` | Not host → guest but the reverse: this is what the guest's `qpu` command *writes* on COM1 for a host broker to pick up. The broker answers with `ESC ] qres ; <tag> ; <state> ; <job> ; <text> BEL`. On a machine with no broker nothing answers and the command says so. | `tools/qpu-broker/README.md` |

Escapes are parsed only from the host side of the console (COM1 or vsock), never from
the PS/2 keyboard, so a person at a real machine cannot type one by accident.

## Typical lines

```
# CI's default: a clean shell, no network
-append "quiet"

# QEMU user-mode network with a grant for DNS and one web port
-append "quiet egress=ttl=600;bytes=4194304;allow=10.0.2.2:53/udp,10.0.2.2:8000/tcp"

# Two guests coupling fields on a raw L2 (static addresses, each names the other)
-append "quiet ip=10.0.2.15 peer=10.0.2.16"
-append "quiet ip=10.0.2.16 peer=10.0.2.15"

# Firecracker: devices on mmio (Firecracker appends these itself), a tap, a grant
"boot_args": "quiet ip=10.0.2.15 egress=ttl=600;bytes=4194304;allow=10.0.2.2:8000/tcp"

# A seed measured on a real QPU
-append "quiet qseed=3f9a1c0e5b7d2a44"
```

See also: `docs/NETWORKING.md` (the grant in depth), `docs/FIRECRACKER.md` (mmio,
vsock, the public console), `docs/CONSOLE_SHELL.md` (what the shell does with the console).
