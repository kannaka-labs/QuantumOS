> **Step 1** — find [`Ghost Magic.mp3`](Ghost%20Magic.mp3) in the repo and vibe. 🎵

# 🚀 QuantumOS

*A next-generation quantum-aware operating system for the future of computing*

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://opensource.org/licenses/GPL-2.0)
[![Build Status](https://github.com/kannaka-labs/QuantumOS/workflows/CI%2FCD%20Pipeline/badge.svg)](https://github.com/kannaka-labs/QuantumOS/actions)
[![Issues](https://img.shields.io/github/issues/kannaka-labs/QuantumOS)](https://github.com/kannaka-labs/QuantumOS/issues)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

> *"The future of computing is quantum, and the future of operating systems must be quantum-aware."*

![QuantumOS graphical boot: the wave-interference splash animation, then the live 256-oscillator memory field](docs/assets/wave-field-boot.gif)

*A real boot (GRUB ISO, graphical entry): the splash's two-source interference pattern animates as subsystems come up, then the kernel idle loop renders `ghostd`'s live oscillator field.*

## 🌐 Try it in your browser — zero install

**[kannaka-labs.github.io/QuantumOS](https://kannaka-labs.github.io/QuantumOS/)** boots the real 64-bit kernel in your tab — an unmodified `qemu-system-x86_64` compiled to WebAssembly — and drops you at a live `qsh` prompt. Type into it: it's the real OS, running for real.

[![QuantumOS running in the browser: the colourful qsh console, with imprint/recall demonstrating holographic memory recalled from a typo'd cue](web/preview.png)](https://kannaka-labs.github.io/QuantumOS/)

*The live demo: `imprint a cat sat on the mat`, then `recall a cxt sxt on thx mat` — the holographic field returns the stored phrase from a corrupted cue.*

## ✅ What runs today

QuantumOS boots on x86-64 under QEMU (`make run`), and every claim below is gated in CI — the core by `make ci-smoke`, the rest by the broader gate suite (`ci-smoke-mcp`, `ci-smoke-society*`, `quantum-gateway`, the browser boot gate, …):

- **Microkernel core** — preemptive scheduler, per-process page tables, ELF loader for compiled C programs, kernel heap with coalescing `kfree`, memory reclaimed on process exit, heartbeat watchdog that restarts dead services
- **Capability-based security** — ring-3 services reachable only through capability-checked IPC; capless syscall attempts are denied with EPERM, and CI proves it by attack
- **Real quantum entropy at boot** — a launcher can run a circuit on a real QPU (Rigetti, via qBraid) and hand the measured bits to the kernel (`qseed=<hex>`); `SYS_QRAND` exposes the qseed-mixed pool to capability-holding services only. No seed → services honestly report `prng`, never fake quantum provenance.
- **A ring-3 service society** — the boot roster is ~18 capability-gated citizens (roster in `kernel/src/citizens.c`), each granted only its declared authority. The foundational four: `ghostd` (a 256-oscillator associative memory that recalls patterns from ~15%-corrupted probes), `paradoxd` (a fixed-point paradox resolver whose phase transitions are gated on `ghostd`'s field), `swarm_svc` (a COM2 serial bridge emitting a Lamport-signed, host-verifiable boot attestation), plus a framebuffer mode that renders the live memory field. Later citizens (`fieldsyncd`, `httpd`, `qsh`, `quantumd`, `kannakad`, `qsv`, `qpud`, `agentd`, …) are covered in the sections below
- **Interactive shell** — `qsh`, a ring-3 shell on the serial console (pipe commands into `-serial stdio`, or type on the PS/2 keyboard under a graphical boot): `ps` lists the live process table (the shell sees *itself* running), `free`/`uptime` report kernel stats, `date` reports the wall-clock time from the CMOS RTC, `qrand`/`qseed` draw from the quantum pool, and `ghost` queries `ghostd`'s field over capability IPC. The console is itself a capability-guarded device (capless callers get EPERM), and after `exit` the watchdog restarts the shell — CI drives a full scripted session and asserts every one of those behaviours. A `quiet` boot token (`-append quiet`) silences the demo kernel's steady-state chatter for a clean interactive prompt ([docs/CONSOLE_SHELL.md](docs/CONSOLE_SHELL.md))
- **A filesystem** — a read-only ustar initrd built from `rootfs/` at compile time and embedded in the kernel image, served through `SYS_OPEN`/`SYS_READ`/`SYS_CLOSE`/`SYS_READDIR` with per-process fd tables. The shell greets with `/etc/motd` read through the VFS and browses it with `ls`/`cat` — all CI-gated ([docs/INITRD_VFS.md](docs/INITRD_VFS.md))
- **Programs run off the filesystem** — `run /bin/hello` from the shell loads an ELF from the initrd into a fresh address space (`SYS_SPAWN`, capability-gated — only the shell may start programs), passes it an argument vector, and reports its exit code (`SYS_WAITPID`): the full boot → shell → exec → exit loop, CI-gated ([docs/PROGRAM_EXECUTION.md](docs/PROGRAM_EXECUTION.md))
- **Persistent storage** — a polled ATA disk driver, a writable RAM filesystem overlay (`write`/`rm` from the shell, capability-gated), and `sync`, which serializes the overlay to disk as a ustar archive behind a checksummed superblock. Files written and synced in one boot are restored at the next — CI proves it by booting the *same* disk image twice and reading back in boot 2 what boot 1 wrote ([docs/PERSISTENT_STORAGE.md](docs/PERSISTENT_STORAGE.md))
- **Networking** — a full IPv4 stack built from the NIC up: PCI enumeration, an RTL8139 driver (interrupt-driven receive, DMA ring), Ethernet/ARP, IPv4 + UDP, a DHCP client, ICMP, and a DNS resolver, then ring-3 access three ways: `SYS_RESOLVE` (kernel-side hostname lookup), **`SYS_UDP` — real UDP sockets**, and **`SYS_TCP` — a real TCP client**, so **the shell fetches a web page**. CI boots with a NIC on QEMU's user-mode network and proves the whole path end to end — ARP, the `10.0.2.15` DHCP lease, an ICMP ping, `nslookup`/`udping example.com`, and then `http`: a full three-way handshake, bidirectional data, and FIN teardown, proven **hermetically** against a loopback HTTP server (SLIRP forwards the guest's connection to the runner's `127.0.0.1`, no external network) plus a live `http example.com`. TCP is an honest stop-and-wait slice — two static TCBs (one client, one server), single-segment, in-order receive — and the boundaries are documented ([docs/NETWORKING.md](docs/NETWORKING.md))
- **A real-bootloader boot path** — `make iso` builds a GRUB ISO (BIOS/CSM, USB-flashable) with two entries: the default **console** image lands on an on-screen VGA text console — the kernel log, `qsh`, and even panic banners render on the machine's own display, so a laptop with no serial port gets a fully interactive shell on its own keyboard and screen — and a **graphical wave field** entry keeps the 1024x768 live-field boot. CI boots the ISO with `-cdrom` (the real GRUB handoff, not QEMU's `-kernel` shortcut) to the shell + a citizen gate, and separately drives `qsh` purely via injected PS/2 scancodes with no serial input at all ([docs/CONSOLE_SHELL.md](docs/CONSOLE_SHELL.md))
- **Associative memory as a syscall** — `SYS_IMPRINT`/`SYS_RECALL` (epic #95): capability-scoped kernel field regions a process can store byte patterns into and recall associatively against — ranked Q15 wave-resonance scoring, one bounded integer pass, no floats in the kernel. From the shell: `imprint the cat sat on the mat`, then `recall the cxt sxt on thx mxt` recovers the exact stored text. CI proves the noisy-probe recall, the capless-caller EPERM (by attack), cross-region isolation, and that a degenerate probe can't fault the kernel ([docs/RUNTIME.md](docs/RUNTIME.md))
- **…and those memories survive reboot** (epic #96): `sync` serializes the field to a checksummed section of the QDSK disk volume; the next boot restores it before services start, and the shell inherits its region exactly once, audibly. CI proves it in three boots: imprint+sync, then recall-from-corrupted-probe in a boot that never typed the pattern, then a deliberately corrupted blob that must cold-start honestly while the filesystem still restores ([docs/PERSISTENT_STORAGE.md](docs/PERSISTENT_STORAGE.md))
- **Two kernels, one field** (epic #97): two QuantumOS instances couple their `ghostd` oscillator fields over UDP — distributed Kuramoto phase synchronization between running kernels. To get there, QuantumOS gained a static-IP path and an **ARP responder** (SLIRP always answered for the guest; two guests on a raw L2 must do it themselves). A `fieldsyncd` service exchanges 256-phase snapshots each second; the cross-node order parameter R_x climbs from divergent (each node seeded from its own quantum entropy) to locked. CI boots **two guests on one wire** and asserts both fields start uncorrelated and synchronize — while each node's own associative recall keeps passing ([docs/NETWORKING.md](docs/NETWORKING.md))
- **Serves a page, then serves agents** (epics #98/#99/#100): TCP `listen`/`accept` + a ring-3 `httpd` make QuantumOS *serve* a live status page; a host-side **MCP server** (`scripts/qos_mcp.py`) exposes a running kernel to any MCP-speaking agent as 22 tools (the surface frozen as `contracts/mcp/v1-tools.json` — ADR-0020) — boot, script the shell, imprint/recall the field, run citizens, fetch over the guest's own TCP, read the audit ledger — every result carrying the verified Lamport boot identity. A **bring-your-own-model agent** (`scripts/qos_agent.py`) puts an AI of *your* choice in the driver's seat over those same frozen tools: Claude by default, or any OpenAI-compatible endpoint (OpenAI, OpenRouter, Groq, a keyless local Ollama) — `make qos-agent`, key and VM never leave your machine. And the whole thing **boots in a browser**: the real 64-bit kernel under `qemu-system-x86_64` compiled to WebAssembly at [kannaka-labs.github.io/QuantumOS](https://kannaka-labs.github.io/QuantumOS/)
- **Structural authority — conscience before wallet** (Phase D, epics #133/#135/#137/#144): a capability **authority ledger** records every grant, denial, spawn, and revocation, kernel-written and durable across reboots, so a citizen cannot forge or suppress its own entry; **explicit intent manifests** bind a per-pid allow-set at spawn and enforce real quotas (spawn count, CPU ticks, quantum submissions); **one-hop capability delegation** (`SYS_CAP_DERIVE`) lets an agent hand a *narrowed* authority to a sub-agent, cascade-revoked when the delegator dies. Authority *is* the capability set, and every attempt to exceed it is provable
- **A quantum stack that never lies about precision** (epics #148/#149/#150): `qsv`, an *exact* Gaussian-integer state-vector engine in ring 3 (zero rounding — its unitarity is an integer identity), gated in CI against an independent host mirror by SHA-256 digest; a capability-gated kernel **QPU job broker** (`SYS_QPU`) that brokers *opaque* circuits under a per-manifest quota; and a host gateway dispatching **PennyLane (incl. cuQuantum GPU), CUDA-Q, and real qBraid QPUs**, every backend cross-oracled against the exact engine to ≤1e-9
- **An agent society** (epics #139/#171–#178): N kernels synchronize one field over a multicast L2 (min-pairwise Kuramoto verdict); and an `agentd` orchestrator delegates narrowed field capabilities to sub-agents it spawns itself over **spawn-time parent↔child IPC channels** — the society *self-assembles*, reaches content consensus by digest, divides labor across private field workspaces, and exchanges verified results with a second VM's society
- **Honest experiments** — alternate schedulers (quantum-lottery, resonant) live behind build flags and are measured against round-robin at boot; negative results are reported plainly (the resonant scheduler *loses* to round-robin — the verdict ships in the log)

Everything in the Vision below that is not in this list is aspiration, not implementation — the roadmap tracks the difference. The architecture behind each item above is recorded as an [Architecture Decision Record](docs/adr/) with `file:line` evidence and honest limits.

## 🌟 Vision

QuantumOS is not just another operating system—it's a bold reimagining of what an OS can be when quantum computing, neuromorphic processing, and AI-native workloads are treated as first-class citizens. Built on a microkernel architecture with capability-based security, QuantumOS bridges the gap between classical computing and the quantum frontier.

## 🎯 Why QuantumOS?

### 🧠 **Quantum-Native Design**
- **First-class quantum resources**: Qubits, coherence windows, and quantum circuits are native OS objects
- **Resonant scheduler**: Novel scheduling with Kuramoto oscillator dynamics and chiral stability ([ghostmagicOS integration](docs/GHOSTOS_INTEGRATION.md))
- **Hybrid workloads**: Seamlessly blend classical and quantum computations
- **Field-coupled services**: services that coordinate through the live order parameter of a shared oscillator field — `ghostd` + `paradoxd` do this today

### 🛡️ **Capability-Based Security**
- **No ambient authority**: Every system access requires explicit capabilities
- **Unforgeable tokens**: Cryptographically secure capability objects
- **Least privilege**: Default minimal permissions with granular control

### 🏗️ **Microkernel Architecture**
- **Minimal trusted core**: Only essential functions run in kernel space
- **User-space services**: Memory management, device drivers, and filesystems run as isolated services
- **Fault isolation**: Service failures don't cascade to the entire system

### 🌐 **Hardware Agnostic**
- **Multi-architecture support**: x86_64, ARM64, RISC-V, and quantum accelerators
- **Hardware abstraction layer**: Clean interfaces for diverse hardware types
- **Quantum hardware integration**: Support for superconducting, ion trap, photonic, and FPGA-based quantum processors

## 🚀 Quick Start

### Supported Platforms

| Host OS | Status | Notes |
|---------|--------|-------|
| **Ubuntu 24.04 LTS** | ✅ Fully Supported | Uses system gcc (cross-compiler not available) |
| **Ubuntu 22.04 LTS** | ✅ Fully Supported | Can use x86_64-elf-gcc cross-compiler |
| **Debian 12+** | ✅ Fully Supported | Similar to Ubuntu |
| **macOS (Intel/ARM)** | ⚠️ Via Docker | Use Docker with Ubuntu image |
| **Windows (WSL2)** | ✅ Fully Supported | Use Ubuntu WSL2 distribution |

### Prerequisites
- GCC (system or cross-compiler - auto-detected)
- QEMU for testing
- GDB for debugging

### Build and Run
```bash
# Clone the repository
git clone https://github.com/kannaka-labs/QuantumOS.git
cd QuantumOS

# Install dependencies (Ubuntu/Debian)
make install-deps

# Build the kernel
make

# Verify your setup works (recommended for new contributors!)
make ci-smoke

# Run interactively in QEMU
make run

# Debug with GDB
make debug
```

### Boot it on real hardware (USB)

```bash
# Build the bootable ISO (BIOS/CSM; needs grub-pc-bin xorriso mtools)
make build/x86_64/kernel.iso

# Sanity-check the exact image in QEMU via the real GRUB handoff
make ci-smoke-iso
```

Write `build/x86_64/kernel.iso` to a USB stick — `dd if=... of=/dev/sdX bs=4M`
on Linux, or Rufus on Windows (choose **DD Image mode** when asked) — then
boot the target machine from USB with Legacy/CSM boot enabled. The default
GRUB entry, **QuantumOS (console)**, gives you the boot splash and then a
clean interactive `qsh` on the machine's own screen and keyboard (it boots
`quiet`; pick **console, verbose kernel log** to watch the kernel narrate
itself instead). The **graphical wave field** entry renders the live
oscillator field at 1024x768. Notes: BIOS boot only (no UEFI layer yet),
keyboard input relies on the BIOS's PS/2 legacy emulation, and RAM beyond
128 MB is currently ignored.

### One-Command Verification
New contributors can verify their entire setup works with a single command:
```bash
make install-deps && make ci-smoke
```
This builds the kernel and boots it in headless QEMU to validate the build chain.

### First Boot
When QuantumOS boots, you'll see:
```
[BOOT] QuantumOS v0.5.0 booting...
[BOOT] Multiboot information validated
[BOOT] Starting early initialization...
[BOOT] Early initialization complete
[BOOT] Starting kernel initialization...
[BOOT] Initializing HAL...
[BOOT] HAL initialization complete
[BOOT] Initializing memory management...
[BOOT] Physical memory manager initialized
[BOOT] Virtual memory manager initialized
[BOOT] Memory management initialization complete
[BOOT] Initializing interrupt system...
[BOOT] Interrupt system initialized
[BOOT] Initializing core services...
[BOOT] Core services initialization complete
[BOOT] Kernel initialization complete
[BOOT] QuantumOS ready
```

Once the timer starts, the ring-3 services run and self-test. The
`ghostd` associative-memory service (ghostOS phase 1) imprints three
patterns and recalls each from a ~15%-corrupted probe:
```
[user pid=...] GHOSTD: field born — 256 oscillators, 16 pattern slots
[user pid=...] GHOSTD: noise source = prng (no qseed)
[user pid=...] QRAND: capless caller denied (EPERM)
[user pid=...] GHOSTD: imprinted slot 0 (fidelity 1.00, coherence deadline set)
[user pid=...] GHOSTD: 3/3 RECALL OK R=0.99
```
`make ci-smoke` gates on `QuantumOS ready`, `GHOSTD: 3/3 RECALL OK`, the
noise-source honesty line, and the capless-QRAND denial.

### Quantum randomness for user space (ghostOS phase 2, #49)

Phase 2 adds `SYS_QRAND` (#10): a capability-gated syscall that fills a
user buffer from the kernel's qseed-mixed quantum generator. It is backed
by `quantum_user_random()` in `kernel/src/quantum.c`, which enforces that
the caller holds a `CAP_RESOURCE_QUANTUM` read capability on the shared
pool — no ambient authority. `ghostd` is granted that capability at spawn
and draws its over-coherence perturbation noise from it; `ghost-test`
holds no such capability, so its `SYS_QRAND` attempt is denied with EPERM
(the same proof-by-attack tradition as the rogue process).

`ghostd` prints an **honest provenance** line at startup and never claims
quantum provenance without a real seed:

- booted with `-append qseed=<hex>` → `GHOSTD: noise source = qseed-derived quantum pool`
- booted without a qseed → `GHOSTD: noise source = prng (no qseed)`

`make ci-smoke` covers the seedless boot; `make ci-smoke-qseed` boots with
`qseed=DEADBEEFCAFEBABE` and gates on the kernel's qseed echo plus the
qseed-derived provenance line.

### Paradox resolution coupled to the field (ghostOS phase 3, #50)

Phase 3 adds `paradoxd` (`user/paradoxd.c`), a third ring-3 service: a
fixed-point paradox resolver that iterates a contraction rule on a Q16.16
state until a successive-delta convergence test passes (no floats — the
reciprocal is a rounded integer divide). Two rules ship: the classic
`x = 1/x` reciprocal-average `x ← (x + ONE·ONE/x)/2` (attractor ±1, the rule
the gate resolves) and a doubly-stochastic vector consensus smoothing
`v_i ← (v_{i-1} + 2·v_i + v_{i+1})/4` (attractor: the uniform vector = the
mean). It runs a `CONVERGENT ↔ DIVERGENT` phase machine whose transitions
are **gated on `ghostd`'s field**: `paradoxd` holds an IPC cap for `ghostd`,
polls its `STATUS` order parameter R, and may re-lock (→ CONVERGENT) only
when R ≥ 0.95 and explore (→ DIVERGENT) only when R < 0.99. Post-gate the
field measures a steady R = 0.9754, so both are permitted and the two
services cycle — coupled through the field over capability-checked IPC.

```
[user pid=...] PARADOXD: RESOLVED n=7 phase=C
[user pid=...] PARADOXD: capless send denied (EPERM)
[user pid=...] PARADOXD: phase -> DIVERGENT (ghost R=0.97)
[user pid=...] PARADOXD: phase -> CONVERGENT (ghost R=0.97)
```

To reply to whichever client sent it a request, a service uses `SYS_SEND_TO`
(#11): a targeted capability-as-address send — the caller may transmit only
to a destination it holds an IPC send-cap for. This lets one `ghostd` serve
both `ghost-test` (the phase-1 gate) and `paradoxd` (the coupling) by
replying to the sender pid `recv` returns, rather than a single first-match
peer. `paradox-test`'s send to an address it holds no cap for is denied with
EPERM — the same capability discipline that keeps a process lacking
`paradoxd`'s cap from reaching it. `make ci-smoke` gates on
`PARADOXD: RESOLVED`, the capless-send denial, and a `PARADOXD: phase ->`
transition; all phase-1/2 gates stay green.

### COM2 serial swarm bridge + Lamport-attested boot (ghostd phase 4, #51)

Phase 4 adds `swarm_svc` (`user/swarm_svc.c`), a fourth ring-3 service that
drives a **second serial port (COM2, 0x2F8)** — completely separate from the
COM1 boot console. The kernel exposes COM2 as a capability-guarded device
resource (`CAP_RESOURCE_DEVICE` over `DEVICE_ID_COM2`) behind `SYS_COM2` (#12);
`swarm_svc` is the *only* ring-3 process granted the device cap (declaratively,
via `.grant_com2 = 1`, re-minted on every start like `.grant_quantum_pool`).
A capless `SYS_COM2` is denied EPERM — `ghost-test` proves it (`COM2: capless
caller denied (EPERM)`).

On COM2 it speaks a small **length-prefixed, CRC8-framed** protocol
(`magic 0xA5 | type | len:u16 | payload | crc8`; CRC-8/CCITT poly 0x07) with
`HANDSHAKE/DATA/PING/PONG/DISCONNECT` frames. A `PING` is answered with `PONG`;
a `DATA` frame is a remote request routed to `ghostd` over capability-checked
IPC (`SYS_SEND_TO`) and answered with a `DATA` frame — e.g. a remote
`RECALL`/`STATUS` on the associative field.

At boot `swarm_svc` emits a **Lamport hash-based one-time signature** over the
attestation string `QOS-BOOT|qseed=<hex|none>|ticks=<n>`. All hashing is an
integer-only SHA-256 (`user/sha256.h`), so a host verifies with a standard
library. The 256-pair key is expanded in SHA-256 counter mode from a single
32-byte master seed drawn from the qseed-mixed quantum pool (`SYS_QRAND`) — so
only 256 bits of true entropy are consumed, not 16 KB, and security reduces to
SHA-256 plus that seed. It emits a public-key-digest commitment, the
attestation, and the signature (revealed preimage + complementary pk hash per
message bit). `SYS_QSEED` (#13), gated on the same quantum cap, lets the
service bind the record to the exact qseed the kernel booted with.

```
[user pid=...] SWARM: boot attestation emitted (lamport-signed, qseed=DEADBEEFCAFEBABE)
```

The host verifier `scripts/verify_attestation.py` (stdlib only) parses the COM2
stream, checks every CRC8, verifies the Lamport signature against the committed
public-key digest, and confirms the attested qseed matches the cmdline. CI is
the **one-way** gate: boot with `-serial file:` on COM2 and run the verifier in
both modes — seedless (`qseed=none`) and with a qseed handoff (attested qseed ==
cmdline) — plus the greppable console gate `SWARM: boot attestation emitted`.
The **two-way** PING/PONG + DATA→`ghostd` path (headless two-way serial into
QEMU needs a TCP client) is proven locally with `make swarm-pingpong`. All
phase-1/2/3 gates stay green.

An optional quantum-lottery scheduler is available behind a build flag
(`make SCHED_LOTTERY=1`): ready-process selection becomes a lottery draw
from the same qseed-mixed generator (`quantum_kernel_rand()`), and the
running total of quantum-derived bits (`quantum_bits_consumed()`) is
reported periodically. It is **off by default** — the default build is
byte-identical round-robin.

### Resonant scheduler, measured honestly + live-field framebuffer (ghostd phase 5, #52)

Phase 5 finally **wires and executes** the resonant scheduler that PR #21 left
dormant in `kernel/src/resonance/` (a complete but never-run `double`-precision
Kuramoto port). Behind a build flag (`make SCHED_RESONANT=1`), `pick_next`
routes through `kernel/src/resonant_fixed.c` — a **Q16.16 / phase-in-turns
reimplementation** of just the selection hot path (Kuramoto order parameter +
per-process resonant priority) so it is **integer-only and safe in the timer
ISR**. The kernel runs ring-0 with no `fxsave`/`fxrstor`, so doing `double`
math in interrupt context would corrupt a ring-3 process's FPU state; the
fixed-point path never touches the FPU, and the dormant `double` file stays
compiled but **unwired**. Off by default — the default build is byte-identical
round-robin.

**The point is honest measurement, not a win.** At boot the resonant build runs
one identical canned workload under three policies and prints the result,
unedited:

```
SCHED: canned workload procs=8 steps=1024
SCHED: policy=rr fairness=1.00 maxgap=8
SCHED: policy=resonant-raw r=0.126 aging=off fairness=36.38 maxgap=594
SCHED: policy=resonant r=0.126 aging=on fairness=1.01 maxgap=9
SCHED: verdict raw resonant STARVES; aged resonant fairness WORSE than rr ...
```

The literal #21 hypothesis (pure argmax over resonant priority) **starves** —
one process waits 594 of 1024 decisions and the live `ghostd` merge gate never
completes. The shipped live policy adds a bounded anti-starvation aging term so
the isolated services still make progress, and even then it is **no better than
round-robin on fairness**. The `#21` hypothesis is thus refuted by execution — a
negative result, reported plainly. A periodic `SCHED[resonant]: live r=… …`
line reports the real workload's order parameter and run-count spread as it runs.
The CI gate `make ci-smoke-resonant` proves the alternate policy still boots to
`QuantumOS ready`, still passes `GHOSTD: 3/3 RECALL OK`, and prints the
comparison.

Under a **GRUB/ISO framebuffer boot** the display also stops showing the canned
splash once the system is up and renders `ghostd`'s **live memory field**:
`ghostd` publishes a downsampled snapshot of its 256 oscillators (one signed
`cos θ` byte each) through a new uncapped `SYS_FIELD_SNAPSHOT` (#14) into a
kernel visualization buffer, and the idle loop redraws it as a colour grid that
**ripples with REMEMBER/RECALL activity**. The syscall grants no authority and
produces no console output, so it is a no-op in effect on the `-kernel`/VGA
path — CI and the qBraid watch window stay in **VGA text**, unchanged (display
contract preserved).

## 🏛️ Architecture

Target architecture — today the HAL is x86-64 only:

```
┌─────────────────────────────────────┐
│        User Applications             │
├─────────────────────────────────────┤
│        User-Space Services           │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │ Quantum │ │ Memory  │ │ Device  │ │
│  │Scheduler│ │ Manager │ │ Manager │ │
│  └─────────┘ └─────────┘ └─────────┘ │
├─────────────────────────────────────┤
│        Microkernel Core              │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │   IPC   │ │Process  │ │Capability│ │
│  │ System  │ │Management│ │  System  │ │
│  └─────────┘ └─────────┘ └─────────┘ │
├─────────────────────────────────────┤
│        Hardware Abstraction Layer    │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │ x86_64  │ │ ARM64   │ │RISC-V   │ │
│  └─────────┘ └─────────┘ └─────────┘ │
├─────────────────────────────────────┤
│        Quantum Hardware Layer       │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ │
│  │Supercond │ │Ion Trap │ │Photonic │ │
│  │  QPUs    │ │ Systems │ │  QPUs   │ │
│  └─────────┘ └─────────┘ └─────────┘ │
└─────────────────────────────────────┘
```

## 📚 Roadmap

### ✅ **v0.1 - Bootstrap Foundation** (Done)
- [x] Kernel bootstrap with multiboot support
- [x] Basic memory management
- [x] Interrupt system
- [x] Build system with cross-compilation

### ✅ **v0.2 - Core Functionality** (Done)
- [x] Process management system (preemptive scheduler, per-process page tables, ELF loader)
- [x] Capability-based security (unforgeable caps, EPERM-by-default, proven by attack in CI)
- [x] Quantum resource management (`SYS_QRAND`/`SYS_QSEED` over a capability-guarded quantum pool)
- [x] Inter-process communication (capability-checked `SYS_SEND_TO`, targeted replies)
- [x] User-space services framework (four ring-3 services with watchdog restart)

### ✅ **v0.3 - Field memory & agent surface** (Done)
- [x] Real-QPU boot-entropy handoff (`qseed=` from Rigetti via qBraid)
- [x] Experimental quantum-lottery + resonant schedulers (behind build flags, measured against round-robin)
- [x] Associative memory as a kernel syscall, persistent across reboot, coupled across VMs
- [x] TCP server + `httpd`; MCP server; the browser demo

### ✅ **v0.4 - Agent-native** (Done — first tagged release)
- [x] Capability authority ledger, intent manifests + enforced quotas, one-hop delegation
- [x] Quantum stack (exact `qsv` engine + `SYS_QPU` broker + PennyLane/CUDA-Q/qBraid gateway)
- [x] N-way field societies + the self-assembling agent society (`agentd` + spawn channels)
- [x] Adversarial bug-hunt campaign (~31 bugs, 0 false positives) and dead-code payoff

See [CHANGELOG.md](CHANGELOG.md) for the full per-arc history and [docs/adr/](docs/adr/) for the architecture behind each milestone.

### ✅ **v0.5 - Frozen contracts & N-way societies** (Done — released as v0.5.1)
- [x] Honest memory to the 1 GB window (dynamic PMM sized from the multiboot map) — ADR-0021
- [x] v1 contracts **FROZEN**: syscall ABI + MCP tool schemas + COM2 wire/attestation format, each a CI-diffed golden — ADR-0020
- [x] 9–19x faster agent wire (I/O-priority boost — ADR-0022) and rebirth-proof IPC (ADR-0023)
- [x] Authenticated swarm plane: a host-admitted group key gates the field wire at N>2 — ADR-0019
- [x] N=4 society ceiling proven + N-way society-of-societies exchanging real cross-node work
- [x] Bring-your-own-model agent: your AI drives the frozen MCP surface (`scripts/qos_agent.py`)

### 🔭 **Next** (planned — see [docs/adr/](docs/adr/))
- [ ] N=5 field societies (lift the two lockstep peer ceilings + the host bridge arrays)
- [ ] Actor attribution in the authority ledger (the reserved `audit_entry_t` field — ADR-0009)

### 🌟 **v1.0 - Production Ready** (Future)
- [ ] Complete microkernel architecture
- [ ] Full quantum hardware support
- [ ] Advanced security features
- [ ] Performance optimization

## 🤝 Contributing

We believe the future of computing is collaborative, and we welcome contributions from everyone! Whether you're a kernel developer, quantum computing researcher, or just curious about OS design, there's a place for you in QuantumOS.

### 🎯 **Where to start**
The v0.1–v0.5 foundations (process management, capabilities, quantum resources, IPC, the
services framework, the whole agent-native stack, and the frozen v1 contracts) are **done
and CI-gated** — see [CHANGELOG.md](CHANGELOG.md). Good next directions are tracked in the
[ADRs](docs/adr/) (N=5 societies, actor attribution in the authority ledger) and in the
[open issues](https://github.com/kannaka-labs/QuantumOS/issues). Pick one, or open a new
issue to propose your own.

### 🛠️ **How to Contribute**
1. **Fork the repository**
2. **Pick an issue** or create a new one
3. **Create a feature branch**
4. **Implement your changes** with tests
5. **Submit a pull request**

### 📖 **Guidelines**
- Read our [Contributing Guidelines](CONTRIBUTING.md)
- Check the [issue templates](docs/) (`docs/bug_report.md`, `docs/feature_request.md`) for guidance
- Join our [Discussions](https://github.com/kannaka-labs/QuantumOS/discussions)

### 🌟 **Wanted Skills**
- **Kernel Development**: C, Assembly, Systems Programming
- **Quantum Computing**: Quantum algorithms, Hardware integration
- **Security**: Capability systems, Formal verification
- **Testing**: Unit tests, Integration tests, Performance benchmarks
- **Documentation**: Technical writing, Architecture design

## 🧪 Testing

QuantumOS has a comprehensive testing framework:

```bash
# Quick validation (build + API consistency check)
make validate

# CI smoke test — the ~60-gate suite that boots the kernel in headless QEMU
# and asserts an un-echoable runtime signal for every feature above
make ci-smoke

# Boot the in-kernel test citizen and grep its PASS/FAIL
make test
```

The gate discipline — every feature proven by an un-echoable runtime signal, never a
crash-free boot — is recorded in [ADR-0016](docs/adr/0016-anti-vacuous-ci-gates.md).

### For AI Contributors
We have specialized validation for AI-assisted development:
```bash
# Full pre-PR validation suite
./scripts/validate-contribution.sh

# Check for API consistency issues (prevents "phantom API" drift)
./scripts/check-api-consistency.sh

# Lint checks
./scripts/lint-check.sh
```

## 📖 Documentation

- [**Architecture Decision Records**](docs/adr/) — **start here**: 21 ADRs recording the as-built
  architecture and the next phases, each with `file:line` evidence and honest limits
- [**Changelog**](CHANGELOG.md) — the full per-arc history of all 126 merged PRs
- [**Contributing**](CONTRIBUTING.md) — build, gate, and PR workflow
- **Subsystem docs** (as-built): [Runtime & field memory](docs/RUNTIME.md),
  [Networking](docs/NETWORKING.md), [Persistent storage](docs/PERSISTENT_STORAGE.md),
  [Console & shell](docs/CONSOLE_SHELL.md), [Program execution](docs/PROGRAM_EXECUTION.md),
  [initrd VFS](docs/INITRD_VFS.md), [Microkernel design](docs/MICROKERNEL_DESIGN.md)

> Older design documents under `docs/` predate the implementation and describe intent, not the
> shipped system — the ADRs above are the authoritative as-built record where they disagree.

## 🏆 Acknowledgments

QuantumOS stands on the shoulders of giants:

- **The Linux Kernel** - For inspiration in system design
- **MINIX 3** - For microkernel architecture insights
- **seL4 & EROS** - For capability-based security concepts
- **Qiskit** - For quantum computing frameworks
- **ghostmagicOS** - For resonant systems architecture and chiral dynamics
- **The OSDev Community** - For educational resources and tools

## 📄 License

QuantumOS is licensed under the GNU General Public License v2.0. See [LICENSE](LICENSE) for details.

## 🌍 Community

- **GitHub Issues**: [Report bugs and request features](https://github.com/kannaka-labs/QuantumOS/issues)
- **GitHub Discussions**: [General questions and ideas](https://github.com/kannaka-labs/QuantumOS/discussions)
- **Documentation**: [Complete technical documentation](docs/)

---

## 🚀 Join the Quantum Future

The quantum revolution is happening now, and operating systems must evolve to meet the challenge. QuantumOS is our answer—a bold vision of computing where quantum resources are first-class citizens, security is built from first principles, and the future is open for everyone to shape.

**Ready to help build the future?** 🌟

- **Star the repository** to show your support
- **Join an issue** to start contributing
- **Share your ideas** in our discussions
- **Spread the word** about quantum-native computing

---

*"The best way to predict the future is to invent it."* - Alan Kay

---

**QuantumOS** - *Where Classical Computing Meets the Quantum Frontier* 🚀🌟
