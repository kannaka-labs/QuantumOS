# QuantumOS public console

A web page where a visitor boots a real QuantumOS virtual machine and types into its
shell. Each visitor gets a fresh Firecracker microVM on KVM (docs/FIRECRACKER.md), with
no disk and no network; its serial console is bridged to the browser over a WebSocket.
The machine is deleted when the visitor leaves, when the guest halts, or after ten
minutes.

```
browser ──https/wss──▶ O1 nginx (/qos/, basic auth, rate limits)
                          │ 127.0.0.1:8930
                          ▼ reverse SSH tunnel (opened outbound by the KVM host)
                       KVM host: qos-console.service ──▶ /run/qos-console/ws.sock
                                    └─ per session: firecracker restored from a snapshot (COM1 on stdio)
```

## Fast start

At startup the launcher boots one VM to the `qsh>` prompt, pauses it, and writes a
Firecracker snapshot (vmstate plus the 128 MiB guest memory) to `/run/qos-console/base-*`.
Each visitor's VM is restored from it (`PUT /snapshot/load`, File memory backend, about
15 ms on debain2) instead of booting, and the visitor is sent the boot transcript the
snapshot VM printed. Every restored VM maps the memory file copy-on-write
(MAP_PRIVATE): clean pages are shared, and each VM's writes stay private to it. The
memory file is never written after the snapshot is taken.

- The snapshot is rebuilt when `kernel.elf` changes (checked by size and mtime, then
  sha256, at each session start). Old snapshots are deleted once no VM maps them.
- Until a snapshot for the current kernel exists, or if a restore fails, the session
  cold-boots as before (about 1.5 s). `--snapshot off` cold-boots every session.
- The builder VM is not counted against `--max-vms`; it lives for about 2 s per kernel.
- Every restored VM starts from the same guest state. So before the visitor can type, the
  launcher sends the kernel 8 fresh random bytes on COM1 (`ESC ] qseed ; <16 hex> BEL`).
  The kernel consumes that sequence and mixes it into its generator (ADR-0025). Each
  visitor gets their own `qseed` and `qrand` output. The seed comes from the host's OS
  random source, not a QPU. Nothing on the console uses it for secrets (the VM has no NIC).

## Pieces

| File | What it is |
|---|---|
| `qos_console.py` | The launcher. One VM per WebSocket; caps; cleanup; logs start/end/duration only. |
| `web/index.html` | The page (xterm.js, pinned with SRI). |
| `deploy/qos-console.service` | Launcher unit: dedicated user in the `kvm` group, `PrivateNetwork`, `/dev/kvm` only, `MemoryMax=1G`, `CPUQuota=200%` for all VMs together. |
| `deploy/qos-console-tunnel.service` | Outbound reverse tunnel from the KVM host to O1; the host opens no inbound port. |
| `deploy/nginx-qos-console.conf` | The nginx locations and rate-limit zones on O1. |
| `deploy/install-debain2.sh` | Installs the launcher side. |
| `test_qos_console.py` | Tests against a fake hypervisor (`fake_firecracker.py`). |
| `smoke_client.py` | End-to-end check: boot, wait for `qsh>`, run a command. |

## Limits

- 4 VMs at once in total, 1 per visitor address, 10 minutes each, 128 MiB and 1 vCPU each.
- Input is capped at 1 KiB per frame. nginx limits page loads to 5/s and boots to 6/min per address.
- The guest has no NIC and no drives, so a visitor can reach nothing but the guest.
- The launcher log records session id, a per-process salted hash of the address, start,
  end, reason and duration. It never records keystrokes or guest output.

## O1 side

- User `qostunnel`, whose only authorized key is
  `restrict,port-forwarding,permitlisten="127.0.0.1:8930",command="/bin/false"`.
- `/etc/nginx/qos-console.htpasswd` (root:nginx 640); the plaintext credentials are in
  `/etc/kannaka-secrets/qos-console.env` (root 600).
- The staging route is `https://staff.ninja-portal.com/qos/`. Going public means a DNS
  record for a dedicated hostname, a certificate, and a server block without basic auth.

## Tests

```
python3 -m pytest -q tools/qos-console          # Linux; needs websockets>=15, pytest-asyncio
```
