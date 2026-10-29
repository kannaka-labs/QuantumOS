# Changelog

All notable changes to QuantumOS. The project uses semantic-ish milestone versions;
`v0.4.0` is the first *tagged* release — earlier versions are retrospective milestone
groupings of the 126 merged PRs (#7–#183), recorded here so the history reads honestly.
Full per-PR provenance: `git log` and the PR list on GitHub.

## [Unreleased]

*(nothing yet)*

## [0.6.2] — 2026-10-29 — networking as an egress grant, UEFI boot, a faster shell and TCP, the first Rust citizen

The v1 contracts (syscall ABI, the MCP tool surface, the society wire protocol) are unchanged and
pass their freeze gates. One error code is added, `SYS_EEGRESS` (-7), for a network call the
egress grant refuses. Everything else is additive.

### Networking is an egress grant (ADR-0030; #324, #326, #327)

- **A NIC is not networking.** A NIC without `egress=` on the boot line binds and does nothing.
  The grant names a TTL, a byte budget, an allowlist of `host:port/proto` entries and the ports
  the guest may listen on (`egress=ttl=600;bytes=4194304;allow=10.0.2.2:53/udp,...;listen=8080`).
  A call outside the grant is refused with `SYS_EEGRESS`; every use and every refusal is a receipt
  row (`AUDIT_EGRESS`) in the authority ledger, and `qsh egress` shows the grant and what is left.
- **A host can grant after boot** over the console escape `ESC ] egress ; <key> ; <spec> BEL`,
  gated by `egress_key=<16 hex>` on the boot line, which ring 3 cannot read (#327). The net thread
  waits offline for a grant instead of giving up.
- Every network gate in CI and the host bridge carry an explicit grant.

### NIC drivers

- Intel e1000 (82540EM), the NIC QEMU attaches by default (#325).
- virtio-net: checksum offload, merged receive buffers, MSI-X interrupts (#328). Measured on QEMU:
  no throughput change, because the TCP stack was the bottleneck, which the next section fixes.
- DNS uses the name servers DHCP hands out; a Firecracker network self-test (#323).

### TCP and the scheduler, measured

- **Receive (#330):** the 8 KiB receive ring was the window, and each refill cost a scheduler round
  trip. A 128 KiB ring, silly-window avoidance and a deeper virtio-net receive queue take a 64 KiB
  fetch from 4.5 s to 1.3 s.
- **Send (#332):** a 64 KiB send ring, the peer's window, slow start, fast retransmit and
  zero-window probes replace stop-and-wait. A 64 KiB upload goes from 20 s to 1.8 s guest clock
  (29 ms on the wire). New: `qsh tcpput`, `scripts/tcp_sink.py`.
- **Net-thread handoffs (#334):** syscalls kick the net thread, the NIC interrupt hands over, and the
  thread wakes its waiters and yields instead of holding its quantum. On the CI runner a 64 KiB
  fetch goes from 900 ms to 10 ms, a 256 KiB upload from 3.15 s to 1.06 s.
- **Scheduler (#336):** a boosted pick no longer re-anchors the round-robin rotation (under keystroke
  or NIC load the tail of the process table starved without bound), one plain tick is given up while a
  boost waits, and a new process is stamped at creation. A load-mode fairness gate runs in CI.

### The shell over virtio-vsock (ADR-0029; #319, #320, #321, #322)

- The public console launcher's `--vsock` carries the shell over a virtio-vsock channel; COM1 stays
  the kernel log and the fallback. `help` to prompt on the public console: 1.4 s to 88 ms (p50).

### UEFI-only machines boot (ADR-0031, #337)

- **The ISO is hybrid again.** `v0.6.1`'s ISO carried no EFI boot image because the release pipeline
  lacked `grub-efi-amd64-bin`; this release's ISO boots from BIOS and from UEFI (gated under OVMF
  by `make ci-smoke-iso-uefi`).
- GRUB on UEFI said "out of memory": the linker script emitted a 64 MB load segment for the NOLOAD
  stack and heap. They are linker symbols now.
- **A boot-stack overflow fixed.** The 16 KB boot stack overflowed by about 7.5 KB during init on
  every boot, zeroing page tables below it. BIOS and `-kernel` boots never touched the zeroed entries;
  the UEFI framebuffer did. The stack is 64 KB, placed below the tables, with a high-water line
  (`STACK:`) and an identity-map check (`PGTBL:`) at the end of init.
- The screen console renders on the firmware's framebuffer when there is no text mode (128x48 cells
  at 1024x768); BIOS boots keep VGA text.

### The first Rust citizen (ADR-0032, #339)

- `/bin/rhello`: a `no_std` Rust program built with plain `rustc` for `x86_64-unknown-none` and linked
  with the C citizens' link line, gated in CI (it runs, the kernel copies into a Rust buffer, the field
  capability refuses it with exactly -4, it exits 7). `user/rust/qsys.rs` twins `usys.h` with its
  struct sizes pinned at compile time.
- **Building needs Rust now:** stable `rustc` with the bare-metal target
  (`rustup target add x86_64-unknown-none`; `make install-deps` does it).

### Entropy and hardware probes

- virtio-rng, labelled as host randomness, not a QPU (#317).
- COM2 is confirmed with the 8250 loopback test and logged "absent" when probed out (#267, #309).

### Docs

- `docs/HARDWARE.md`, the real-hardware compatibility table, and a hardware-report issue form (#338).

### Host tools package (`quantumos-host-tools` 0.6.2)

- `qos_bridge` waits for the audit on an idle bound under a 90 s cap, not a fixed 12 s (#315); N-way
  key checks anchor at the end of the FSKEY line (#310); the scheduler probe samples every 2 s in
  idle mode (#311).
- The v1 MCP golden is re-pinned to mcp 1.30.0 (#314, after the dependency bump in #243).
- The qBraid gateway backend is tested with the SDK mocked, in CI (#318); a Firecracker boot gate
  runs the public console's VM shape in CI (#313); `scripts/` gains a console output benchmark (#316).

## [0.6.1] — 2026-10-25 — fixes from testing the installed package, Sashiko review, qBraid Lab compute

Before more outreach, the installed 0.6.0 package was tested end to end, the way an agent uses it. `quantumos-host-tools` came from PyPI into clean environments on Windows and Linux, and the hub was driven over MCP in front of Kannaka (MCP and CLI), kannaka-quantum, Kannaktopus, the 0xSCADA CLI and steward. `qos-mcp` booted the release kernel under QEMU/KVM. The test found the bugs fixed below, and the fixes were re-verified the same way. The v1 contracts (syscall ABI, the MCP tool surface, the society wire protocol) are unchanged.

### Sashiko review, with your own model key (ADR-0028)

- `qos-review` (`tools/sashiko-review`): an MCP server and CLI that runs a Sashiko review of a commit in a
  repository you listed. It runs on your machine, with your own model provider and key, and returns the findings to
  the caller. Nothing is posted.
- Your key is handed only to the Sashiko process, and it is redacted from everything else.
- Every review has a required token cap, which Sashiko enforces. `review_quote` shows the worst case before
  anything runs.
- The hub asks before `review_start`.

### qBraid Lab compute in the provider broker (#305)

- `qbraid_lab` plugin: the account's Lab server and on-demand instances, priced per minute from qBraid's live profile list. qBraid instances can't be tagged, so a crash between create and record is handled one of two ways. Option b, `--qbraid-lab-sole-user`: refuse while foreign instances exist, then clean up our own. Option c, the default: never guess; log `orphan_possible` and let the person decide.
- ADR-0026 (provider broker) and ADR-0027 (ecosystem hub) are Accepted (#304).

### Fixed, from an end-to-end test of the installed 0.6.0 package

- **Python servers behind the hub start from a uv venv on Windows.** uv's venv launcher sets `PYTHONHOME` to its own
  interpreter, and every program the hub started inherited it, so a different Python (kannaka-quantum's) failed with
  "No module named ...". The hub and its CLI connector now drop the launcher variables unless a server entry sets
  them.
- **An installed package finds a kernel.** `QOS_KERNEL=/path/to/kernel.elf32` (from a release, checked against
  `SHA256SUMS.kernel`) is now honoured by `qos_boot` and by the society tools, which take no kernel argument and
  could not boot from a pip install before.
- The memory bridge finds `kannaka` on `PATH`, and its default build path no longer ends in `.exe` off Windows.
- The person is no longer asked to approve a call that cannot run: an `ask` call to a CLI tool is validated first.
- A downstream call that takes longer than `call_timeout_s` (default 300 s; set per server) answers with a timeout
  instead of holding the agent.
- CLI output reaches the agent without terminal colour codes.
- `hub.example.json`'s 0xSCADA entries name commands the 0xSCADA CLI has (there is no `alarms` command).

## [0.6.0] — 2026-10-25 — Firecracker with storage, a public console, and agents that spend inside a budget

The v1 contracts frozen in 0.5.1 are unchanged. The syscall ABI, the MCP tool surface of
`qos-mcp`, and the COM2 / attestation wire all pass their freeze gates. Everything below is
additive.

### QuantumOS on Firecracker, with a network and a disk

- **PVH boot entry:** QuantumOS boots as a Firecracker microVM (#234).
- **Networking under Firecracker:**
  - a NIC ops table, so the stack is no longer bound to the rtl8139 (#236);
  - virtio-net over virtio-mmio, the NIC Firecracker actually has (#237);
  - virtio-net over modern virtio-pci as well as virtio-mmio, one driver for both transports
    (ADR-0024, #246).
- **Storage under Firecracker (#279):**
  - a block-device indirection (`blkdev_ops_t`), with persistence calling only `blkdev_*`;
  - a virtio-blk driver over both transports;
  - the virtio transports now bind several devices at once. Before, a second probe re-pointed
    the NIC's registers.
  - New gate: `ci-smoke-disk-virtio`.
- **Disk flush:** a cache flush gets up to ~30 s before the disk is called dead, as the ATA
  spec allows. A loaded CI runner's fsync had outlasted the old ~1 s (#293).

### Kernel

- **tcp:** a listener table, so a second network service can start (#240).
- **modbusd:** a Modbus/TCP agent in ring 3, with control writes refused (#239).
- **sched:** the CPU goes to a waiting shell on COM1 RX. Keystroke echo dropped from ~350 ms
  to ~1 ms (#248).
- **Honest labels:**
  - the boot entropy line says where the seed came from, and claims "quantum" only with a
    QPU (#251);
  - every qpu result names its backend: QSIM, or the QPU (#268, #274).
- **waitpid:** never reports a recycled pid's previous owner's exit. A program spawned into a
  reused pid used to get the old exit code on its parent's first poll; this was the
  "stillborn spawn" CI kept retrying past. Every process now has a generation in the exit
  ledger (#295).

### The public console (qos.ninja-portal.com)

- **A throwaway QuantumOS microVM per visitor**, in the browser (#245). Each one is restored
  from a booted snapshot, so the prompt appears in ~20 ms (#249). Each restored session's
  generator is reseeded (ADR-0025, #250).
- **Funnel metrics:** `/stats`, `/metrics`, a page beacon, and a measured timing table (#270,
  #271). Internal traffic is tagged and kept out of the visitor counts (#277).
- **Hardening (#257):**
  - an idle timeout with a warning;
  - input rate limits and an output cap;
  - a busy refusal that says when a machine will be free;
  - refusing to listen on a public address without a declared proxy (#277, #278).
- **Keep a machine (#256, #280):**
  - "Keep this machine" returns a key, shown once. What you `sync` comes back when you resume
    with it, on a cold boot with the disk attached.
  - Kept machines are deleted after 7 days without use, with a 2 GB cap. The store is keyed by
    a hash of the key.
- **Encrypted off-site backup** of kept machines to OCI Object Storage: AES-256-GCM before
  upload, mirrored deletions, and a dead disk never erases the backup (#282).

### Quantum

- **A host quantum broker on qBraid**, reached from `qsh qpu` over COM1 (#269). The simulator
  is the default. A real QPU job needs an opt-in, a daily budget, a per-job cap and per-visitor
  limits. The cost model is fitted to billed Rigetti jobs (#272), and refusals say the
  simulator is free and when limits reset (#273).
- **A daily calibration circuit** with a public, append-only series (#276).

### Agents: real resources inside a budget, and one endpoint for the ecosystem

- **Bring your own model:** any AI can drive the frozen MCP surface (#232). Spend guardrails:
  a cheaper default model, a usage report and a token budget (#233).
- **The provider broker** (ADR-0026, Proposed; #283, #288). An agent can quote, provision and
  tear down real resources on leases, inside an envelope the person sets:
  - USD per day and per action, the allowed resource classes, and the longest lease;
  - approval with a key only the person holds; no agent op can approve or raise the budget;
  - real spending off by default;
  - leases are tagged at the provider, reaped at expiry, and cleaned up by tag after a crash,
    never touching anything else;
  - a ledger of estimates and billed costs side by side.
  - **Plugins:**
    - OCI Object Storage buckets (#289) and Always Free VMs, with a per-region shape check
      (#294). Both were run for real in a least-privilege tenancy.
    - qBraid jobs, through the existing quantum broker unchanged (#290).
  - Its MCP tools: `broker_quote`, `broker_provision` and the rest (#292).
- **The hub** (ADR-0027, Proposed; #297). One MCP endpoint in front of the ecosystem's MCP
  servers and CLIs, with allow, ask or deny per tool (#298):
  - an `ask` call is stored exactly and runs only after the person approves it;
  - CLIs without an MCP server are reached through allowlisted argv templates (#299);
  - MCP servers over HTTP, each isolated so one failing server can't take the hub down (#300);
  - short-lived bearer tokens are renewed on the open session (#301);
  - the example config covers Kannaka memory and swarm, kannaka-quantum, Kannaktopus, the
    kannaka, 0xSCADA and steward CLIs, KAX City, and the broker.

### The package (`quantumos-host-tools`)

- **New commands:** `qos-broker`, `qos-broker-mcp` and `qos-hub` (the `quantumos_agent`
  package), plus an `[oci]` extra (#302).
- **Fix:** `mcp` is pinned to `>=1.10,<2`. A fresh install of 0.5.1 resolves mcp 2.x, on
  which `qos-mcp` cannot even import (FastMCP was renamed). A new CI step builds the wheel,
  installs it clean and runs the installed commands (#302).

### Docs and CI

- The headline demo link was a 404; everything now points at `kannaka-labs` (#241). The org
  Namespace Guard is adopted (#242). There's a qBraid OS integration brief (#244).

## [0.5.1] — 2026-07-15 — v1 contracts frozen, 9–19× faster agent wire, rebirth-proof IPC

### ADR-0020 v1 contract freeze — pre-freeze corrections (guest syscall ABI)

Groundwork before the golden-diff ABI freeze gate lands, so v1 pins a *correct*
surface (`user/usys.h`):
- Corrected the `field_info_` errno docstring: it cited Linux `-14 EFAULT` / `-22
  EINVAL`; the syscall actually returns QuantumOS `SYSCALL_EFAULT` (-2) /
  `SYSCALL_EINVAL` (-1).
- Added 5 symmetric user-side twin-ABI `_Static_assert`s (`udp_req_t`=24,
  `field_imprint_req_t`=76, `field_recall_req_t`=76, `field_recall_out_t`=136,
  `user_args_t`=516) so a struct-size drift fails BOTH ring builds, not just the
  kernel's.
- Annotated the intentional errno-band overlaps as frozen-v1 decisions:
  `svc_restarts()` returns `-1` (not-a-service) sharing `SYSCALL_EINVAL`, and
  `qseed_value()` returns a raw u64 not partitioned from the negative-errno band.
- Split the 6 kernel-side ring-crossing `_k_t` twins (`udp_req_k_t`,
  `cap_derive_req_k_t`, the four `qpu_*_k_t`) out of `kernel/src/syscall.c` into a
  new kernel-internal header `kernel/include/kernel/syscall_abi.h`, so the
  upcoming ABI freeze probe can `#include` and compiler-measure them. ABI-neutral
  (identical structs, `_Static_assert`s moved with them).

### Added — v1 golden ABI freeze gate (ADR-0020)

`contracts/abi/v1.golden` freezes the guest+kernel ABI contract — the 35 syscall
numbers, the sub-op namespaces, the errno table, capability permission bits, and
the ring-crossing struct sizes — and CI diffs it on every push/PR. The values are
**compiler-measured**, not parsed from source: two probe TUs (`user/abi_probe.c`,
`kernel/src/abi_probe_kern.c`) emit the contract into a `.abi_ents` section under
the real build flags, and `scripts/extract-abi.py` reads it back with `objcopy`.
The gate cross-checks that the user/kernel twins agree, diffs against the
committed golden (an intended change is a visible golden diff, never silently
regenerated — `make regen-abi-golden` is human-only), and a teeth-check compiles
a mutated `SYS_QPU` and asserts the gate reddens, so a broken generator can't ship
vacuously green. New CI job `abi-golden` (gcc + binutils, no boot, no pip) that
also gates `release`.

The frozen surface now also pins ring-crossing struct **field offsets**
(`__builtin_offsetof` for every field of the six `udp`/`cap_derive`/`qpu_*`
twins, cross-checked user↔kernel) — so a size-preserving field reorder, which
the size check alone would miss, reddens the gate. The capability resource-type
namespace (`cap_resource_type_t`), the arg-page field offsets, and the device-ID
namespace (`net`/`qpu`/`com2`/`console`/`disk` — the `resource_id` a
`CAP_RESOURCE_DEVICE` capability names) are frozen too (237 golden entries). With
this, **ADR-0020 is Accepted** for the guest syscall ABI (contract a) — frozen and
CI-enforced. The MCP-schema lane (contract b), the COM2/attestation lane
(contract c, now unblocked by ADR-0019), and the durable audit-format freeze
remain scoped as deferred follow-ups.

### ADR-0021 dynamic-PMM — hardening groundwork (no behavior change)

Groundwork before dynamic PMM sizing lands, all no-ops at the current hardcoded
128 MB (`kernel/src/memory.c`, `kernel/include/kernel/memory.h`):
- Added `PMM_PHYS_CEIL` (0x40000000) / `PMM_MAX_FRAMES` (262144) — the 1 GB
  identity-map ceiling `boot.S` already provides — and **clamp the frame count to
  it before the bitmap is sized**, so no frame the allocator can ever hand out is
  unmappable (a `>= 1 GB` frame would be a #158-class escalation).
- A **static-layout floor**: if a machine reports less RAM than the kernel heap's
  backing frames need, `pmm_init` panics loudly instead of letting `kheap_init`
  build the heap on unbacked physical memory.
- Guarded the kernel-image reservation loop against a short frame count, asserted
  the frame bitmap can't overrun the kernel region, and widened the frame-address
  multiply to 64-bit (the clamp was the only thing preventing a 32-bit wrap).

Verified no-op at 128 M: `ci-smoke` boots to `QuantumOS ready` with the PMMROVER,
PMMHEAP, and PMMCLAMP self-tests green. The last is a synthetic teeth-check — the
clamp is factored into `pmm_clamp_frames()` and `pmm_clamp_selftest` feeds it a
> 1 GB frame count and asserts it clamps to exactly 262144, so the 1 GB ceiling is
proven load-bearing on the `-m 128M` leg without any > 1 GB RAM (revert-confirmed:
removing the clamp panics the boot and reddens the gate). The mmap-driven dynamic
sizing + high-memory feature legs follow in subsequent increments.

### ADR-0021 dynamic-PMM — mmap-driven sizing (PR-3)

The `128 * 1024 * 1024` hardcode (and its `TODO: Get actual memory size from
multiboot`) is gone: `memory_init` now takes the RAM total as a parameter
(`memory_init(void)` -> `memory_init(uint64_t total_memory)`), sized from a real
parse of the Multiboot1 memory map. `multiboot_parse_memory()`
(`kernel/src/main.c`) treats the e820 map as **untrusted** bootloader input — the
whole mmap buffer must sit inside the 1 GB identity map with no overflow; each
entry's size must be `>= 20` and advance the cursor without walking past the
window (iteration-capped at 1024); only type-1 available regions count; each
region end is overflow-checked and the RAM total is the **MAX** region end (never
a sum), clamped to `PMM_PHYS_CEIL`. It falls back to `mem_upper`, then **panics**
if neither is present rather than sizing a wild bitmap, and runs only *after*
`boot_validate_multiboot` confirms the MB1 magic (parsing a v2 block at v1 offsets
would wild-read like the closed framebuffer bug). Byte-wise `mb_rd32`/`mb_rd64`
avoid unaligned access on the untrusted buffer.

The parsed total flows `kernel_main` -> `boot_config.total_memory` ->
`memory_init` -> `pmm_init`. The frame bitmap is now sized for the **1 GB maximum
(fixed 32 KB)** rather than detected RAM, so it always covers the fixed
kernel-image/heap reservations no matter what the mmap reported — structurally
ruling out a small-RAM overrun; frames above the detected total stay
free-but-unreachable (the alloc scan is bounded by `total_frames`).

A one-line greppable `PMMSIZE=<hex>` boot marker reports the derived frame count.
New `ci-smoke` gate asserts it lands in the 128 MB-class window `[0x7F00, 0x8000]`
— observed `0x7FE0` at `-m 128M` (QEMU reserves ~128 KB at the top of RAM, which
is exactly why the count is *not* the naive `0x8000` a hardcode would give).
Revert-confirmed: forcing a 2 GB total clamps to `0x40000` and still boots to
`QuantumOS ready` (the clamp survives), but trips the gate with `frame count
262144 outside window` — the gate has teeth. The `-m 256M/512M` scaling legs (the
true hardcode-vs-live differential) follow in PR-4.

### ADR-0021 dynamic-PMM — high-memory proof + differential CI (PR-4), **Accepted**

Completes the feature and flips ADR-0021 to Accepted. Two new boot self-tests
(`kernel/src/memory.c`, wired after `PMMCLAMP`):
- `pmm_highmem_selftest` (`PMMHIGH`) — the load-bearing proof that dynamic sizing
  is safe above 128 MB. On RAM past 128 MB it allocates a top-of-pool frame,
  asserts its physical address is genuinely high (`>= 128 MB`, `< 1 GB`), and —
  the real proof — writes two distinct sentinels through the returned identity VA
  and reads them back, so a `boot.S` that mapped only 128 MB (or a wrong/stale
  map) faults or mismatches here instead of silently handing out an unreachable
  frame. On the `-m 128M` leg there is no high frame, so it reports the **skip**
  branch (proving the guard ran — not a vacuous always-pass).
- `pmm_residency_storm_selftest` (`PMMSTORM`) — extends the single-shot heap
  reservation proof to a bulk drain: 1024 frames pulled with the rover aimed
  straight at the reserved kernel heap, asserting none land in
  `[__heap_start, __heap_end)`. Catches a multi-allocation rover regression that
  the single-shot `PMMHEAP` check could miss.

New differential CI legs `make ci-smoke-mem256` / `ci-smoke-mem512` (CI jobs
`pmm-differential-256/512`, both gating `release`) boot at 256/512 MB and assert
the frame count lands in that size's window (`~0xFFE0` / `~0x1FFE0`, i.e. it
~doubles / ~quadruples the 128 MB leg's `0x7FE0`) **and** that the `PMMHIGH`
writeback took its live branch. This is the true hardcode-vs-live differential
that the `-m 128M` leg alone cannot show: a 128 MB hardcode prints `0x8000` at
every `-m` and never the live `PMMHIGH`. Revert-confirmed two ways — re-hardcoding
`memory_init` to 128 MB reddens the `mem256` sizing window (`0x8000` outside
`[65280,65536]`); broadening the high-memory guard so 256 MB wrongly skips reddens
the `mem256` `PMMHIGH`-live check while sizing still passes (the two gates are
independent). Every "un-fakeable gate" named in the ADR-0021 design is now live.
Build clean under `-Werror`; format/cppcheck/api-consistency green.

### ADR-0022 prereq-2 — scheduler perf/stability baseline (report-only + calibration)

ADR-0022 deferred the COM2-latency fix behind two prerequisites; prereq 1 (the
latency gate) shipped. This is prereq 2: a scheduler perf/stability baseline so a
future scheduler-constant change is *measured*, not blind (ADR-0016). **Measurement-
only** — it changes no timer HZ / `SCHED_QUANTUM_TICKS` / service model.

An adversarial design panel refuted the naive metric: aggregate context switches are
dominated by voluntary `yield()`s (every long-lived citizen busy-yields with a non-
blocking recv, resetting the quantum counter thousands of times per 10 ms tick), so
`switch_count/tick` is both quantum-insensitive (a gate on it is vacuous) and host-
throughput dependent (CI-flaky). The fix is a **dedicated `preempt_count`** in the
scheduler, incremented only at the timer-quantum-expiry reschedule in `scheduler_tick`
(never on `SYS_YIELD`), which is 1/quantum-paced by construction. Exposed as a new
`SYSINFO_SCHED` sub-op (no new syscall — no ADR-0020 golden churn) on one atomic line
`SCHED: switches=.. preempt=.. ticks=.. maxgap=.. spread=.. runnable=..`, driven by a
qsh `sched` command and `QosVM.sched()`. The dead `now = 0 /* TODO */` timing stub in
`process_switch_to` is made live (`timer_get_ticks()`), and a per-PCB `sched_picks`
counter added, so the line also carries the fairness/tail snapshot (max reschedule gap,
run-count spread) over the runnable roster.

`scripts/test_qos_sched.py` (gate `make ci-smoke-sched`, CI job `scheduler-baseline`)
is **report-only**: it records the tick-normalized baseline and asserts only a non-
vacuous **liveness floor** (≥100 switches and ≥12 runnable over the window), so it
reddens on a dead/wedged scheduler but arms no perf band yet. Empirical calibration
(WSL, 3 runs each) **selected the gated scalar** for the follow-up — preemptions per
1000 guest ticks tracks 1/quantum almost exactly and host-invariantly:

| `SCHED_QUANTUM_TICKS` | preempt/1000t (idle, load) | switch/1000t |
|---|---|---|
| 5 (default) | 199.5–200.0 | ~520 |
| 20 | 49.9, 49.8 | ~131 |
| 80 | harness times out (system too sluggish to fill the tick window) |

So a 4× quantum change moves the metric 4× with <0.5 % variance — the follow-up arms a
floor of ~100 (= q5-median × 0.5), revert-confirmed by a `SCHED_QUANTUM_TICKS` bump to
20 (q=80 grinds the harness itself, so 20 is the practical lever). Build clean under
`-Werror`; format/cppcheck/api-consistency green.

### ADR-0022 prereq-2 — arm the scheduler gate, **ADR Accepted**

`scripts/test_qos_sched.py` flips from report-only to a real gate: it asserts
**preemptions-per-1000-guest-ticks in `[100, 600]`** (the PR-1-calibrated scalar; ~200 at
the default quantum) on top of the liveness floor. Everything asserted is a delta over
guest ticks, so the band is host-CPU-invariant — a slow CI runner reads the same rate.
The floor catches a *coarsened* round-robin cadence (the scheduler getting more sluggish —
ADR-0022's regression direction); the loose ceiling catches pathological thrash without
reddening on the intended future fix (a *smaller* quantum raises the rate). **Revert-
confirmed** on WSL: at the default `SCHED_QUANTUM_TICKS=5` the gate is green (rate 199.9);
bumping it to 20 drops the rate to 49.9 and reddens the floor; restoring returns it to
green. This completes ADR-0022's second prerequisite, so the ADR flips to **Accepted** —
the deferral decision and both prerequisites (latency gate + scheduler baseline) are done;
the latency fix itself stays a deliberately deferred, now-unblocked future epic.

### Fixed — post-ADR-0021/0022 sweep

- `pmm_highmem_selftest` rover clamped to the first high frame (#219, ADR-0021 follow-up).
- The echo/client IPC demos NUL-terminate their `recv_msg` buffers — a reply could
  disclose stale stack bytes past the message (#220, self-disclosure class).
- The browser boot-gate interactivity probe keys on `uptime` instead of a
  ghost-EPERM-prone command, de-flaking the qemu-wasm CI leg (#221).

### ADR-0023 — IPC peer re-wiring on watchdog rebirth + dead-target unlink (#222–#224), **Accepted**

Two defects, one decision (adversarial design review pre-implementation: 15 findings,
3 blockers — all folded into the ADR before a line of code):
- **No capability outlives the process it names.** The epic #175 spawn-channel unlink
  generalizes to EVERY `CAP_RESOURCE_IPC` cap naming a dead pid (`cap_revoke_ipc_targets`)
  — closing the recycled-pid authority leak for `SYS_SEND_TO` and the `SYS_CAP_DERIVE`
  targeted-peer check. The capability self-test's survivor assert inverts to gate it.
- **Wiring comes back by declaration, not pid-reuse luck.** `service_definition_t` grows
  a per-direction `ipc_peers[]` table; `start_slot` re-mints declared pairs on every
  start inside the grant cli window — two passes (my running peers; running services
  declaring me), every mint guarded by liveness + pid **generation** (a stale RUNNING
  slot would reintroduce the exact leak Part 1 closes), pair mints transactional.
  paradoxd/fieldsyncd/swarm-svc/qsh ↔ ghostd and the ADR-0019 key cap converted; the
  citizens.c hand mints are gone, along with every "known restart limitation" caveat.
- swarm_svc re-forwards its cached session key when fieldsyncd's pid changes — the
  re-mint restores the delivery *path*; the re-forward restores the *key* (a reborn
  fieldsyncd would otherwise emit zero-tag frames its keyed peers silently reject).
- Gates: the ci-smoke session delivers a delayed `ghost` to the REBORN shell, asserted
  on the post-`QSH: reborn` log slice (whole-log grep passes vacuously — the session
  prints `ghost R=` twice pre-rebirth); `ghost exit` (exit-is-a-feature for ghostd)
  restarts the TARGET under the living shell and a post-`GHOSTD: FIELD REBORN` slice
  gate proves Part 1's necessity in the live routing path. Revert-confirmed in both
  directions; the forward run happened to rebirth ghostd onto a recycled pid — the
  exact scenario the unlink protects.

### ADR-0022 — the I/O-priority boost: the deferred latency epic, **shipped** (#225)

The option table's principled fix, hardened by a 4-lens adversarial panel (31 findings:
7 blockers, 15 majors). A process whose awaited I/O just arrived — an IPC delivery, or
a COM2 RX byte for the registered holder — is flagged `io_boost` and picked out of turn
at the next reschedule. **PING 0.45 s → 0.052 s (~9×), STATUS 0.90 s → 0.048 s (~19×)**;
every agent-facing MCP tool call rides this wire. The panel's teeth are in the design:
a sticky 0xFF absent-COM2 latch (an unbacked port reads all-ones — the boost would have
fired every tick of every COM2-less CI boot), `SCHED_BOOST_MAX_CONSEC=2` (a boost
ping-pong would starve the roster AND freeze quantum expiry itself, invisibly to every
liveness floor), consume-at-dispatch-commit (`pick_next` stays pure — it is a bare
predicate in the CPUKILL pre-check), a (pid, generation)+live-cap-validated COM2-holder
registration, and `boost_count` counting HONORED picks only. Gates re-armed in the same
increment: ci-smoke-latency asserts PING median < 0.30 s AND a positive honored-boost
delta (binding effect to mechanism — a bare median is gameable by a quantum change the
sched gate deliberately tolerates), STATUS < 0.6 s; ci-smoke-sched arms a max_gap ≤ 600
tick starvation ceiling under a boost-invariant paced load. Four-way revert-confirmed:
`SCHED_IO_BOOST 0` reddens both latency assertions (the boost-off median lands exactly
on the phase-locked 0.45 s rotation floor) while the preempt band stays green in both
directions.

### ADR-0020 — MCP + wire freeze lanes: the v1 contract freeze is **complete** (#226)

The two deferred lanes land, mirroring the ABI golden discipline (2-lens panel first:
21 findings, 4 blockers):
- **`contracts/wire/v1.golden`** — the COM2 swarm-bridge framing, opcodes, Lamport
  attestation parameters, CRC8 parameters, reply-auth geometry, FSYN/FSYP coupling-frame
  sizes+offsets, and attestation-string KATs, compiler-measured from a probe
  (`user/wire_probe.c`) whose macros are the SAME ones the emitters now use — twinned
  against a host ring extracted from the live `scripts/qos_bridge.py` parser constants.
  The twin rule + a MUST_TWIN set turn guest↔host drift (previously guarded only by a
  code comment) into a red diff; `host:kat:ping_frame` freezes byte order + CRC span in
  one measured number.
- **`contracts/mcp/v1-tools.json`** — all 22 MCP tools, name + inputSchema (descriptions
  and result-dict shapes deliberately excluded and recorded as such); pydantic's volatile
  `title` keys normalized away; `mcp`/`pydantic`/`pydantic-core` pinned in
  `requirements-mcp-gate.txt` AND the golden `_meta` with a generator-skew self-check, so
  a wrong-environment regen cannot be committed silently. Teeth live in the extractors
  (changed-value AND added-name mutations both proven caught; exit 1 is reserved for
  diffs so an inverted teeth-check cannot pass on a crash). New pinned-pip CI lane for
  the MCP gate; the wire gate rides the stdlib-only abi-golden job.

With all three lanes frozen — syscall ABI (#206–#212), wire, and MCP — **the v1
agent-surface contract of ADR-0020 is complete.**

## [0.5.0] — 2026-07-11 — Agent-reachable QPU, hardening, dead-code payoff

Post-v0.4.0 increments landed autonomously through the panel → gate (revert-and-confirm)
→ merge pipeline. PRs #187–#190.

### Added
- **`qos_qpu_submit` — the 22nd MCP tool** (#189): an agent can now run a named quantum
  circuit (`bell` / `ghz` / `grover`) on the in-OS QPU broker and get the exact integer
  result back — capability-gated (only `swarm_svc` holds the submit cap), quota-bounded
  (`qsub_max`), and recorded in the authority ledger (`AUDIT_QSUBMIT`). The
  conscience-before-wallet demo made agent-facing. Gated by `ci-smoke-qsubmit`.
- **Published to PyPI**: `pip install quantumos-host-tools` is live; tagged releases now
  auto-publish (the `PYPI_API_TOKEN` secret is wired into `release.yml`).

### Fixed
- **PMM heap-reservation gap** (#187): the kernel heap's backing frames are now reserved
  in the physical allocator, so the low-first rover can never hand a live-heap frame to a
  page table or ELF segment (an arbitrary-corruption path reachable only under a large
  residency storm). New `PMMHEAP` boot self-test, revert-confirmed.
- **Multiboot2-magic trap** (#187): the kernel is Multiboot1-only, so an MB2 magic is now
  refused with an honest panic instead of parsing a v2 info block as v1 (which would lose
  the cmdline and wild-write the framebuffer). New `MB2REJECT` self-test.
- **`qos_memory_import` docstring** (#190): corrected — each memory's Kannaka similarity
  *is* carried into its slot energy (the docstring had claimed it was not).
- **Society error identity** (#190): the society tools no longer misattribute a failure to
  the unrelated single-VM identity (`_society_err`).

### Changed
- **Dead-code payoff** (#188): deleted the unwired `cap_transfer` helper, the
  declared-never-defined `apic_*` prototypes, and five stale duplicate bare headers under
  `kernel/include/` — −641 lines, proven dead by a green `ci-smoke`.

## [0.4.0] — 2026-07-11 — Quantum stack, agent societies, hardened (first tagged release)

The agent-native arc completed and battle-tested. PRs #151–#183.

### Added
- **Quantum stack** (epics #148/#149/#150, PRs #151–#155): `qsv`, an *exact* Gaussian-integer
  quantum state-vector citizen (zero rounding, digest-vs-host-mirror CI gate); a capability-gated
  kernel **QPU job broker** (`SYS_QPU`, opaque circuits only, per-manifest `qsub` quota); a COM2
  quantum-submit transport over the attested bridge; a host gateway dispatching
  **PennyLane lightning.qubit / lightning.gpu (cuQuantum) / CUDA-Q / qBraid real QPUs**, all
  cross-oracled against the exact engine (≤1e-9).
- **Agent society arc** (PRs #171–#179): the end-to-end agent-native showcase (`agentdemo` boot
  token) — an orchestrator citizen delegates narrowed field capabilities to sub-agents it spawns
  itself over **spawn-time parent↔child IPC channels**; content-consensus digests verified per
  kernel-vouched sender; **division-of-labor** private field workspaces; a **society of
  societies** — two kernels exchanging verified society results over the field-sync wire; the
  ring-3 roster refactored to `kernel/src/citizens.c`.
- **CI gates**: qsh singleton-IPC-cap invariant (#176), disk *upgrade* path — frozen audit-ledger
  home survives field growth (#183), qseed-handoff smoke incl. the errno-collision gate (#168).

### Fixed
- **Adversarial bug-hunt campaign** (PRs #156–#167): ~31 real bugs across quantum, networking,
  trust-core, process/memory, and lifecycle code — 0 false positives across 8 hunts. Highlights:
  closed a ring-3 → whole-OS DoS in syscall copy-in/out (#158→#160, `COPYGUARD` gate); DNS answers
  bound to their query (on-link poisoning, #162); spawn error paths reclaim address spaces (#164);
  argv buffer cleared between spawns (#165).

### Changed
- Performance: amortized-O(1) frame allocator via a search rover (#169); capability read-lookups
  bounded by a high-water mark (#170).
- Dead code deleted per the unwired-optionality rule: the never-wired shared-memory region
  subsystem (#180) and the orphaned duplicate `ipc.h` (#181).

## [0.3.0] — 2026-07-10 — Field memory, agent surface, societies *(retrospective, untagged)*

The OS becomes agent-native. PRs #112–#147.

### Added
- **Associative memory as a syscall**: `SYS_IMPRINT`/`SYS_RECALL`/`SYS_FIELD_INFO` — capability-
  scoped kernel field regions with Q15 wave-resonance scoring; recall from ~15%-corrupted cues
  (epic #95). **Memories survive reboot** on the checksummed QDSK volume (epic #96); sync refuses
  foreign disks (#115).
- **Two kernels, one field** (epic #97): `fieldsyncd` couples ghostd oscillator fields over UDP
  (R_x 0.03→0.99); then the **N-way mean-field society** (epic #139) — N kernels with distinct
  qseeds synchronize over a multicast L2, min-pairwise verdict.
- **TCP server + httpd** (epic #98): QuantumOS serves a live status page.
- **MCP server** (epic #99) and the deepened **agent toolbox** (epic #125): 21 tools — boot,
  qsh scripting, field imprint/recall, fs, fetch, qrand, societies — every result carrying the
  Lamport-verified boot identity. **Kannaka HRM bridge** (epic #127): agent memories flow between
  the kernel field and the host memory system.
- **QuantumOS in the browser** (epic #100): the real kernel under qemu-wasm on GitHub Pages.
- **The trust core** (Phase D, epics #133/#135/#137/#144): the capability **authority ledger**
  (every grant/deny/spawn/revocation recorded, durable across reboots #146, REVOKE-vs-REAP
  attribution #147); **explicit intent manifests** with enforced quotas (spawn #136, CPU #145,
  quantum submissions #152); **cross-ring one-hop capability delegation** (`SYS_CAP_DERIVE` #138)
  with cascade revocation on delegator death.

## [0.2.0] — 2026-07-08 — A real microkernel OS *(retrospective, untagged)*

Non-booting skeleton → an interactive OS on real hardware, in six days. PRs #27–#111.

### Added
- **Core microkernel in one day** (#31–#46): multiboot v1 + long-mode boot, 100 Hz PIT preemptive
  round-robin scheduler, capability-based security, ring-3 user mode with fault containment,
  per-process address spaces, in-kernel ELF64 loader, services-as-ring-3-processes with
  capability-checked IPC, watchdog heartbeats, real kernel heap.
- **ghostOS/HRM-0 citizens** (#53–#59): `ghostd` Hopfield–Kuramoto associative memory;
  `SYS_QRAND` with honest qseed provenance; `paradoxd` field-gated paradox resolver; COM2 swarm
  bridge with **Lamport-attested boot**; the resonant scheduler wired and *measured honestly*
  (it loses to round-robin — verdict published in CI).
- **Interactive OS** (epic #62): `qsh` shell, embedded ustar initrd + VFS, `SYS_SPAWN`/`SYS_WAITPID`
  with argv — boot → shell → exec → exit, all CI-gated.
- **Persistent storage** (epic #71): ATA PIO driver + writable ramfs overlay + `sync`; two-boot
  survival proven in CI.
- **Networking** (epics #73/#80/#82): PCI/RTL8139, ARP, IPv4/UDP/DHCP, ICMP, DNS, `SYS_RESOLVE`,
  ring-3 UDP sockets, TCP client — the shell fetches a web page. CMOS RTC (`date`).
- **libq + native citizens** (#86–#92): ring-3 runtime (arena heap, integer printf, fixed-point
  math) and native apps — `consciousnessd`, `kannakad`, `quantumd`, `qtop`.
- **Real hardware** (epic #101): VGA screen console, GRUB ISO (USB-flashable), quiet boot; a real
  laptop boots to an interactive shell on its own screen and keyboard.

## [0.1.0] — 2026-02-13 — Bootstrap foundation *(retrospective, untagged)*

PRs #7–#22: the original skeleton — architecture documents, message-passing IPC, process
management, CI scaffolding, the resonant-scheduler concept, and a kernel that compiled but did
not yet boot. Dormant until the 2026-07-02 revival.
