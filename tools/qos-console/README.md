# QuantumOS public console

A web page where a visitor boots a real QuantumOS virtual machine and types into its
shell. Each visitor gets a fresh Firecracker microVM on KVM (docs/FIRECRACKER.md), with
a 4 MiB save disk (below) and no network; its serial console is bridged to the browser over a WebSocket.
The machine is deleted when the visitor leaves, when the guest halts, after two minutes
with no input and no output, or after ten minutes.

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
| `qpu_relay.py` | The COM1 side channel to the host quantum broker (`--qpu-broker`, #262): moves `qpu` requests from the guest to `tools/qpu-broker`, never to the page, and strips OSC escapes from visitor input. |
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
| `sessions_ended{reason}` | `disconnect`, `expired`, `idle`, `output_cap`, `guest-exited` or `error` |
| `internal_sessions`, `internal_beacons` | our own traffic, tagged with the internal token (below); counted nowhere else |

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
- `GET /calibration.json` and `GET /calibration` serve the daily QPU calibration series
  (#275, `--calibration-file`; written by `tools/qpu-broker/qpu_calibrate.py`), as JSON
  and as a plain-text table. The launcher only reads the file. Days since the first row
  that have no row at all are listed in `missing_days` and shown as `MISSING`. Public:
  https://qos.ninja-portal.com/calibration and /calibration.json.
- **Unique visitors per day.** The count comes from an HMAC-SHA256 of the address under
  a 32-byte salt. The salt exists only in memory and is replaced at each UTC midnight,
  and the set of hashes is dropped at the same time. Nothing derived from an address is
  ever written to disk. A launcher restart replaces the salt early, so a visitor who
  comes back the same day after a restart is counted twice.
- With `--state-file` (the unit uses `/var/lib/qos-console/metrics.json`), the counts
  and timings are saved every 30 s and at shutdown, so they survive restarts.
- **Our own traffic is kept out.** Ad captures, demos, smoke tests and benches carry the
  token from `--internal-token-file` (the unit uses `/etc/qos-console/internal.token`,
  qosconsole 400; the unit runs with `Group=kvm`, so a group-readable file is not enough): as an `X-QOS-Internal` header from scripts
  (`smoke_client.py` and `bench_console.py` take `--internal-token-file`), or by opening
  the page as `https://qos.ninja-portal.com/?internal=<token>` for a browser capture,
  which passes it on to `/ws` and `/beacon`. Those sessions and beacons count only in
  `internal_sessions` and `internal_beacons`, so the funnel, the timings and the unique
  visitors are outside visitors alone. `/stats` says from which day this holds
  (`internal_since`); counts before it include our own traffic. The token is compared in
  constant time and never logged. Without the file nothing is tagged.

## Measured timings

These come from real runs of `bench_console.py`. Each run is one WebSocket session.
Timings are taken on the client's monotonic clock from the moment the WebSocket opens
(the TLS handshake is not included), and there are 10 sequential runs per row. Echo is
one key (alternately `x` and DEL) until its echo arrives: 40 keys per run, 400 samples
per row, taken after the console has been quiet for 3 s. Output rate is the bytes of
`help` (about 1.4 KB) divided by the time from Enter to the next prompt.

Host: debain2, a Xeon E5-2697 v4 KVM guest, which makes this nested KVM. Firecracker
v1.16.1, with the console's VM shape (1 vCPU, 128 MiB, no drives, no NIC, `quiet`).
Measured 2026-10-23; the two vsock rows 2026-10-27.

The Commit column is the main commit the measured kernel was built from, so a row can be
matched to the fixes it already includes. The first row's kernel predates the record
keeping, and its commit was not written down.

| Path | Commit | Kernel | Shell | To `qsh>` (median, max) | Echo p50 / p90 | Output rate |
|---|---|---|---|---|---|---|
| Cold boot, before #246/#248 (3 runs, 10 keys each) | not recorded (before #246) | `b972dbd3…` (debug build, the console's first kernel) | COM1 | 75.9 s, 78.3 s | 350.0 / 350.6 ms | ~1.2 KB/s |
| Cold boot (`--snapshot off`), on the host | `c597791` | `43c4c1af…` (release, #268) | COM1 | 1.33 s, 1.40 s | 2.0 / 2.3 ms | ~1.0 KB/s |
| Snapshot restore, on the host | `c597791` | `43c4c1af…` | COM1 | 18 ms, 21 ms | 2.0 / 2.4 ms | ~1.0 KB/s |
| Snapshot restore, public: `wss://qos.ninja-portal.com` from a home PC (O1 nginx + SSH tunnel) | `3cf0287` | `1b0f9b33…` (release, #251) | COM1 | 15 ms, 33 ms after the socket opened (opening it took a median 0.92 s) | 55.7 / 65.5 ms | ~0.95 KB/s |
| Snapshot restore, on the host, `--vsock` | `ee5c386` | `3dcd8e32…` (release, #320) | vsock | 20 ms, 26 ms | 2.2 / 3.3 ms | ~77 KB/s |
| Snapshot restore, public, `--vsock` (the deployed console) | `ee5c386` | `3dcd8e32…` | vsock | 19 ms, 46 ms after the socket opened | 51.1 / 60.9 ms | ~18 KB/s |

Reading the table:

- The 75 s cold boot was the absent-COM2 spin that #246 fixed. The 350 ms echo was the
  scheduler rotation that #248 fixed.
- On the public path, echo is dominated by the network: PC to O1 plus the tunnel to
  debain2. On the host it is about 2 ms.
- Over COM1, output was about 1 KB/s on every path: serial TX under nested KVM costs about
  1 ms per byte (#259). Over the vsock shell channel (ADR-0029) it is ~77 KB/s on the host.
  On the public path it is ~18 KB/s, limited by the network round trip rather than the VM.
  `help` (1.5 KB) reaches the next prompt in about 0.09 s instead of 1.4 s.

To reproduce on the host, run a second launcher beside the service, with its own run
dir and port. Its startup sweep only touches its own run dir.

```
python3 qos_console.py --kernel kernel.elf --firecracker /usr/local/bin/firecracker \
    --run-dir ~/qos-bench-run --listen-tcp 127.0.0.1:8931 [--snapshot off]
python3 bench_console.py ws://127.0.0.1:8931/ws --runs 10 --pause 1 --json out.json
python3 bench_console.py wss://qos.ninja-portal.com/ws --runs 10 --pause 11   # public; 6 boots/min/address
```

## Saved machines (#256)

With `--save-dir`, every VM gets a small blank virtio-blk disk (`--save-mib`, 4 MiB) in
its own session directory. Restored VMs share one snapshot, which was taken with the
drive at a *relative* path (`disk.img`); Firecracker opens it against its own working
directory, which is the session directory, so each visitor writes only their own disk.

- **Keep.** The page sends `{"type": "keep"}`. The launcher answers once with a resume
  key (128 random bits, `secrets.token_urlsafe(16)`): `{"type": "kept", "token": ...}`.
  Nothing is stored yet.
- **Save.** The visitor types `sync` in QuantumOS (the OS's own persistence, ADR-0007).
  When the session ends, the disk is kept only if it carries a synced QuantumOS volume
  (the `QDSK1` superblock). A keep that was never synced stores nothing, and its key
  stops working. The page gets `{"type": "saved"}` or `{"type": "not_saved"}` if it is
  still connected.
- **Resume.** The page opens `/ws?resume=1` and sends `{"type": "resume", "token": ...}`
  as its first frame, so the key never appears in a URL or a proxy log. The VM
  cold-boots (about 2 s) with a copy of the kept disk, and the kernel restores the files,
  the field and the audit ledger at boot. When the session ends the disk is written back.
- **Refusals:** `unknown_machine` (wrong, expired or never-synced key), `machine_open`
  (open in another session), `no_token` (no first frame within 10 s), `saving_off` (a
  resume to a launcher without `--save-dir`, refused before any VM starts so the key is
  never typed into a guest); for keep: `storage_full`, `too_many_today`.

What is stored, in `--save-dir` (mode 0700, files 0600): `<sha256 of the key>.img` and
`<same>.json` (created and last-used times). No key, no address, nothing about the
visitor. The images are not encrypted, so the operator can read them; the page says so.
The log carries the first 8 hex digits of the hash, never the key.

| Flag | Default | Meaning |
|---|---|---|
| `--save-dir` | unset | where kept machines live; unset means VMs have no disk at all |
| `--save-mib` | 4 | size of each VM's disk |
| `--save-days` | 7 | a machine unused this long is deleted (checked hourly and at start) |
| `--save-total-mib` | 2048 | new keeps are refused when the store would exceed this; nobody's machine is evicted |
| `--saves-per-ip-day` | 3 | keeps per visitor address per UTC day (in memory, per-process address hash) |

`/stats` adds `machines_kept` (kept at session end, so synced), `machines_resumed` and
`now.saved_machines`.

### Off-site backup (#281)

`backup_saves.py` (run nightly at 03:30 UTC by `deploy/qos-backup.{service,timer}`)
copies the store into one private OCI Object Storage bucket, **encrypted before upload**
(AES-256-GCM, object name as associated data; format in the script's docstring). Changed
machines are re-uploaded and expired ones deleted. Only machines this host recorded
uploading are ever deleted, so a dead disk (which loses that record) deletes nothing;
and if more than half the bucket (over two machines) would go at once, nothing is deleted
and the unit fails with exit 3. A wiped store cannot erase its own backup.

- Host files (all root, 0600, dir 0700): `/etc/qos-backup/backup.key` (32 random bytes;
  **keep an offline copy**: without it the bucket cannot be read), `oci_api_key.pem`
  (the API signing key; only its public half was ever pasted into OCI), `oci.conf`,
  `backup.json`. Upload state: `/var/lib/qos-backup/state.json`.
- The OCI user `qos-backup` can only read and write objects in bucket
  `qos-console-saves` (compartment `qos`): listing other buckets and compute are refused.
- Restore drill: `backup_saves.py restore --to DIR --config /etc/qos-backup/backup.json`,
  then compare with the store.
- Runtime: `/opt/qos-backup/venv` (python3 -m venv; `pip install oci cryptography`).

## Limits

Every limit, with its flag and default (#257):

| Limit | Flag | Default | When it is reached |
|---|---|---|---|
| VMs at once, in total | `--max-vms` | 4 | refused: `{"reason": "busy", "message": "all 4 machines are busy", "retry_after_s": N}` |
| VMs per visitor address | `--per-ip` | 1 | refused: `one-per-visitor` |
| Session length | `--max-seconds` | 600 | session ends: `expired` |
| No input and no output | `--idle-seconds` | 120 | a warning line on the terminal `--idle-warn-seconds` (15) before, then the session ends: `idle` |
| Input frames | `--input-frames-per-s` | 64/s, 1 s burst | excess frames are dropped, with one notice per session; never a disconnect |
| Input bytes | `--input-bytes-per-s` | 4096/s, 1 s burst | as above |
| Input frame size | `MAX_INPUT` | 1 KiB | the frame is cut to 1 KiB |
| Guest output | `--max-output-bytes` | 2 MiB | output is sent up to the cap, then the session ends: `output_cap` |
| Memory and CPU per VM | `--mem-mib` | 128 MiB, 1 vCPU | |
| All VMs together | systemd `MemoryMax`, `CPUQuota` | 1 GiB, 200 % | |
| Page loads, boots per address | nginx `qos_page`, `qos_boot` | 5/s, 6/min | |

- **Busy means retry-after, not a queue.** `retry_after_s` is the time until the soonest
  running session reaches `--max-seconds`. That is an upper bound, since idle sessions end
  sooner. Nothing is held open waiting, so a crowd can't pile up connections on the
  launcher. The page shows "one is free in at most M:SS, often sooner".
- **`X-Real-IP` is trusted only behind a proxy.** `--per-ip` takes the address from
  `X-Real-IP`, which only nginx sets. The launcher listens on a Unix socket (or loopback
  in tests). It refuses to start on any other `--listen-tcp` address unless
  `--trust-forwarded-for` says a proxy in front really sets that header.
- The guest has no NIC, and its only drive is its own 4 MiB save disk, so a visitor can
  reach nothing but the guest.
- The launcher log records session id, a per-process salted hash of the address, start,
  end, reason and duration. It never records keystrokes or guest output. The metrics
  above add no new record of addresses.

## O1 side

- User `qostunnel`, whose only authorized key is
  `restrict,port-forwarding,permitlisten="127.0.0.1:8930",command="/bin/false"`.
- `/etc/nginx/qos-console.htpasswd` (root:nginx 640); the plaintext credentials are in
  `/etc/kannaka-secrets/qos-console.env` (root 600).
- Public at `https://qos.ninja-portal.com` (its own server block, no basic auth, certbot
  certificate); the staging route `https://staff.ninja-portal.com/qos/` stays behind basic auth.

## Tests

```
python3 -m pytest -q tools/qos-console          # Linux; needs websockets>=15, pytest-asyncio
```
