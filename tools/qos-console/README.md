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
| `bench_console.py` | The timing harness behind "Measured timings" below. |
| `deploy/nginx-qos-public.conf` | The public server block (`qos.ninja-portal.com`), certbot lines left out. |

## Metrics (#258)

The launcher counts the visitor funnel. It keeps counts and timings only: no addresses,
no session ids, and nothing typed.

| Step | Counted when |
|---|---|
| `page_loads` | the page sends `GET beacon?e=load` |
| `boot_clicks` | the visitor presses Boot (`GET beacon?e=boot`) |
| `sessions_started{mode}` | a `/ws` session gets a VM (`restore` or `cold`) |
| `sessions_refused{reason}` | `busy` or `one-per-visitor` |
| `boots_completed{mode}` | the `qsh>` prompt has been sent to the visitor |
| `sessions_with_input` | the visitor typed at least one key (only that it happened, never what) |
| `sessions_ended{reason}` | `disconnect`, `expired`, `guest-exited` or `error` |

There are three timings, each measured from the moment the session is admitted:
`first_output` (the first guest byte sent), `ready` (the prompt sent) and `session` (the
whole session).

- `GET /stats` returns JSON: totals, p50/p90/max over the last 1000 values of each
  timing, and a row per UTC day (60 days are kept). The public site proxies it:
  https://qos.ninja-portal.com/stats.
- `GET /metrics` returns Prometheus text format: the counters above as `qos_*_total`, the
  gauges `qos_sessions_active`, `qos_sessions_max` and `qos_snapshot_ready`, and the
  histograms `qos_first_output_seconds`, `qos_ready_seconds` and `qos_session_seconds`
  (buckets from 0.01 s to 600 s). nginx does not proxy it, so it answers only on the
  launcher's socket (on O1: `curl 127.0.0.1:8930/metrics`).
- `GET /beacon?e=load|boot` returns 200, or 400 for any other event. The nginx
  locations for `/beacon` and `/stats` have `access_log off`, so they add no record of
  addresses. Both share the page's rate limit.
- **Unique visitors per day.** The count comes from an HMAC-SHA256 of the address under
  a 32-byte salt. The salt exists only in memory and is replaced at each UTC midnight,
  and the set of hashes is dropped at the same time. Nothing derived from an address is
  ever written to disk. A launcher restart replaces the salt early, so a visitor who
  comes back the same day after a restart is counted twice.
- With `--state-file` (the unit uses `/var/lib/qos-console/metrics.json`), the counts
  and timings are saved every 30 s and at shutdown, so they survive restarts.

## Measured timings

These come from real runs of `bench_console.py`. Each run is one WebSocket session.
Timings are taken on the client's monotonic clock from the moment the WebSocket opens
(the TLS handshake is not included), and there are 10 sequential runs per row. Echo is
one key (alternately `x` and DEL) until its echo arrives: 40 keys per run, 400 samples
per row, taken after the console has been quiet for 3 s. Output rate is the bytes of
`help` (about 1.4 KB) divided by the time from Enter to the next prompt.

Host: debain2, a Xeon E5-2697 v4 KVM guest, which makes this nested KVM. Firecracker
v1.16.1, with the console's VM shape (1 vCPU, 128 MiB, no drives, no NIC, `quiet`).
Measured 2026-10-23.

| Path | Kernel | To `qsh>` (median, max) | Echo p50 / p90 | Output rate |
|---|---|---|---|---|
| Cold boot, before #246/#248 (3 runs, 10 keys each) | `b972dbd3…` (debug build, the console's first kernel) | 75.9 s, 78.3 s | 350.0 / 350.6 ms | ~1.2 KB/s |
| Cold boot (`--snapshot off`), on the host | `43c4c1af…` (release, #268) | 1.33 s, 1.40 s | 2.0 / 2.3 ms | ~1.0 KB/s |
| Snapshot restore, on the host | `43c4c1af…` | 18 ms, 21 ms | 2.0 / 2.4 ms | ~1.0 KB/s |
| Snapshot restore, public: `wss://qos.ninja-portal.com` from a home PC (O1 nginx + SSH tunnel) | `1b0f9b33…` (release, #251) | 15 ms, 33 ms after the socket opened (opening it took a median 0.92 s) | 55.7 / 65.5 ms | ~0.95 KB/s |

Reading the table:

- The 75 s cold boot was the absent-COM2 spin that #246 fixed. The 350 ms echo was the
  scheduler rotation that #248 fixed.
- On the public path, echo is dominated by the network: PC to O1 plus the tunnel to
  debain2. On the host it is about 2 ms.
- Output stays at about 1 KB/s on every path. That is serial TX under nested KVM, about
  1 ms per byte (#259).

To reproduce on the host, run a second launcher beside the service, with its own run
dir and port. Its startup sweep only touches its own run dir.

```
python3 qos_console.py --kernel kernel.elf --firecracker /usr/local/bin/firecracker \
    --run-dir ~/qos-bench-run --listen-tcp 127.0.0.1:8931 [--snapshot off]
python3 bench_console.py ws://127.0.0.1:8931/ws --runs 10 --pause 1 --json out.json
python3 bench_console.py wss://qos.ninja-portal.com/ws --runs 10 --pause 11   # public; 6 boots/min/address
```

## Limits

- 4 VMs at once in total, 1 per visitor address, 10 minutes each, 128 MiB and 1 vCPU each.
- Input is capped at 1 KiB per frame. nginx limits page loads to 5/s and boots to 6/min per address.
- The guest has no NIC and no drives, so a visitor can reach nothing but the guest.
- The launcher log records session id, a per-process salted hash of the address, start,
  end, reason and duration. It never records keystrokes or guest output. The metrics
  above add no new record of addresses.

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
