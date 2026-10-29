# Console Input & the qsh Shell

Epic #62, phase 1 (issue #63). This document describes the interactive
console — the OS's first *input* path — and `qsh`, the ring-3 shell built
on it.

## Why

Until this phase, everything QuantumOS ever did was decided at build
time: it booted a fixed constellation of services, ran its gates, and
idled. Output existed (the COM1 boot console); input did not. The
console input path plus a shell turn the demo kernel into a system a
human — or CI — can actually sit at: inspect live state, exercise
capabilities, and watch supervision work.

## Console input path (`kernel/src/console.c`)

One 1 KiB ring buffer, two interrupt-driven producers:

- **COM1 RX (IRQ4)** — bytes typed into the serial console: QEMU
  `-serial stdio`, the qBraid watch window, or CI's piped stdin.
- **PS/2 keyboard (IRQ1)** — scancode set 1 translated to ASCII (US
  layout, shift tracked; extended `0xE0` keys ignored for now), for
  interactive/graphical boots.

The COM1 *transmit* path is untouched — it remains `early_console_write`
in `boot.S`. `console_write()` adds a raw byte sink (no `[user pid]`
prefix) so a shell can own its own line discipline; every write runs
with interrupts disabled so a timer-tick log line can never split it.

### Public API (`kernel/include/kernel/console.h`)

| Function | Purpose |
|---|---|
| `console_init()` | Program COM1 line settings, rescue any byte already received, enable the RX interrupt, reset PS/2 state |
| `console_com1_irq()` | IRQ4 body: drain received bytes into the ring |
| `console_kbd_irq()` | IRQ1 body: translate one scancode, push ASCII |
| `console_read(buf, len)` | Non-blocking drain of buffered input (0 if none) |
| `console_write(buf, len)` | Raw, interrupt-atomic COM1 output |

`DEVICE_ID_CONSOLE` (= `0x3F8`, the port base) identifies the console as
a capability resource, the same self-identifying convention as
`DEVICE_ID_COM2`.

### The init-ordering subtlety

CI pipes shell input from t=0 — QEMU delivers the first byte into the
receiver long before the kernel reaches `console_init()`. Two rules keep
every byte alive:

1. **Never clear (or even toggle) the FIFOs.** QEMU forces a FIFO clear
   on any change of the FIFO-enable bit, and QEMU refills the receive
   register asynchronously the moment the guest drains it, so there is
   *no* point in the init sequence where a clear is provably safe (a
   real byte was lost to exactly this race during development: `help`
   arrived as `hep`). Per-byte RX interrupts are ample for a console.
2. **Drain before enabling the interrupt.** Any byte already waiting is
   pushed into the ring first; a byte that lands between the drain and
   the IER write still raises the IRQ, because Data-Ready is
   level-evaluated when IER changes.

The smoke test gates this with the *input integrity* check: the first
piped command must execute intact (`qsh: commands: help`).

## Syscalls

### `SYS_CONS` (15)

`rdi` = op (`SYS_CONS_READ`/`SYS_CONS_WRITE`), `rsi` = buffer,
`rdx` = length. Raw console I/O, moved through a bounded kernel bounce
buffer (`CONS_MAX_BYTES` = 256) and clamped to the caller's mapped user
half. Gated on a `CAP_RESOURCE_DEVICE` capability over
`DEVICE_ID_CONSOLE` — `CAP_READ` to read, `CAP_WRITE` to write, EPERM
without. Reads are non-blocking (0 when nothing is buffered). Only
`qsh` is granted the console, and the capless `ghost_test` proves the
denial by attack every boot (`CONS: capless caller denied (EPERM)`).

### `SYS_SYSINFO` (16)

`rdi` = op (`SYSINFO_PS`/`SYSINFO_MEM`), `rsi` = buffer, `rdx` = length.
Kernel-formatted, read-only introspection text — uncapped like
`SYS_GETPID`/`SYS_TICKS`, because it names no authority:

- `SYSINFO_PS` — one `PS: <pid> <name> <STATE>` line per live process,
  formatted by `process_format_ps()` in `process.c` (which owns the
  table). The kernel formats so the user side needs no struct ABI.
- `SYSINFO_MEM` — one `MEM: heap free=<n> bytes, frames free=<a>/<b>`
  line from the kernel heap and physical frame allocator.

Output is bounded by `SYSINFO_MAX_BYTES` (1024); the PS formatter stops
at whole rows.

## qsh (`user/qsh.c`)

A ring-3 user-process service, watchdog-monitored like every other
citizen. Declarative grants (re-minted on every start): the console
device capability and a quantum-pool read capability. One IPC send-cap
each way wires it to `ghostd`.

Line discipline lives in the shell, not the kernel: qsh echoes, handles
backspace, assembles lines. Every logical output line is written with a
single `SYS_CONS` call so tick logs never split it.

Builtins: `help`, `echo <text>`, `ps`, `free`, `uptime`, `pid`,
`qrand`, `qseed`, `ghost` (STATUS query to `ghostd` over capability
IPC), `imprint [--energy <0-100>] <text>` / `recall <probe>` (the kernel
holographic field, epic #95: the shell holds the `CAP_RESOURCE_FIELD`
capability over region 0, so the operator can store text and recover it
later from a corrupted probe — `recall the cxt sxt` returns
`FIELD: winner="the cat sat"` with the exact stored bytes; the optional
`--energy` percent sets the slot's importance, which drives recall
ranking and eviction), `field` (epic #127 B1: `SYS_FIELD_INFO`, a
READ-ONLY enumeration of the region — `FIELDINFO: region=0 live=N cap=8`
then a `FIELDSLOT:` line per live slot with its energy, effective
energy, retrievals, age, and a bounded preview; unlike `recall` it does
NOT reinforce, so it never perturbs the field), `fieldtest` (asserts the
cross-region EPERM and degenerate-probe n=0 contracts from the
cap-holding side; `ghost_test` proves the capless side), `audit`
(epic #133 Phase D: `SYS_AUDIT`, the capability AUTHORITY LEDGER — the
KERNEL records every capability GRANT, DENY, and SPAWN, so a citizen
cannot forge or suppress its own entry; `audit` prints `AUDIT: total=…`
then an `AUDIT: seq=… pid=… kind=GRANT|DENY|SPAWN|MDENY|QUOTA res=TYPE:id
perms=… verdict=OK|EPERM` line per event — read-only introspection that
makes "authority IS the capability set, and every attempt to exceed it is
provable" observable), `manifest` (epic #135 Phase D: `SYS_MANIFEST`, the
per-pid INTENT MANIFEST — a policy layer above raw capabilities that the
KERNEL builds from each citizen's grant flags, so a citizen cannot forge
its own row; `manifest` prints a `MANIFEST: pid=… bound=1 spawn=used/max
cpu_ticks=…` line per bound citizen plus a `MANIFEST: pid=… allow
res=TYPE:id perms=…` line per declared resource — declared intent made
inspectable, and the spawn quota's enforcement observable), `clear`,
`exit`.

`exit` is a supervised death: the shell terminates, its heartbeat goes
silent, and the service watchdog restarts it (~2 s); the reborn shell
introduces itself with `QSH: reborn (restart=N)`. That banner is the
merge-gate proof that the operator surface survives its own crash.

`qseed` probes the quantum-pool capability **out of band** (via
`qrand_seed_present()`, whose success space is only `{0,1}`) before reading
the seed. `SYS_QSEED` returns an unconstrained 64-bit value, so testing the
returned value directly against the `-4` (EPERM) errno sentinel — as it once
did — misreported the single valid boot seed `0xFFFFFFFFFFFFFFFC` as a denial.
The `ci-smoke-qseed` boot pins that exact collision seed and asserts `qseed`
prints it rather than `denied (EPERM)`.

## CI gates (all in `make ci-smoke` and the Integration job)

The smoke test pipes `help ps free uptime ghost qrand exit` into QEMU's
stdin and asserts, from the same boot:

1. `QSH: QuantumOS interactive shell ready` — shell up with its console cap
2. `qsh: commands: help` — first piped command intact (input integrity)
3. `qsh RUNNING` — `ps` shows the shell observing itself in the table
4. `MEM: heap free=` — live memory stats
5. `qsh: ghost R=` — field status fetched from ghostd over IPC
6. `qsh: uptime ` and `qsh: qrand ` — kernel ticks + quantum-pool draw
7. `CONS: capless caller denied (EPERM)` — capability gate, by attack
8. `QSH: reborn` — watchdog restarted the shell after `exit`

## Quiet boot: a clean interactive console (`quiet`)

The demo kernel narrates itself: a per-second `Timer tick:` heartbeat, the
`paradoxd`/`ghostd` services logging their steady-state dynamics, the
per-process lifecycle (`Process created/destroyed`, `reaped process`,
`syscall: user process exited`), the per-service `service started` /
health-monitor lines, the demo kernel-threads' `alive` heartbeat, and the
`watched-svc` watchdog demo's `online`/`going silent`. Because the
flaky-demo service crashes and restarts on a ~1.5 s cycle, most of this
*repeats forever*. All of it lands on the *same* serial console `qsh` uses,
so for hands-on interactive work (typing `http`, `nslookup`, …) the prompt
scrolls away under the chatter.

Adding the token `quiet` to the kernel command line (`-append quiet`,
alongside `qseed=…`) silences that steady-state output:

- the kernel's periodic timer-tick heartbeat is suppressed (the one-time
  "interrupts are live" line still prints);
- `boot_log_v()` — a `boot_log` variant that no-ops under `quiet` — carries
  the kernel's repeating lifecycle/demo lines (process create/destroy/reap,
  service start, the `alive` thread heartbeats); label+hex pairs are gated
  as whole blocks so quiet never emits an orphaned hex value;
- `SYSINFO_QUIET` — a bufferless `SYS_SYSINFO` op — exposes the flag to
  ring 3, and the chatty ring-3 services (`paradoxd`, `ghostd`,
  `watched-svc`) query it once and go silent on their console output.

One-time boot milestones (`…: online`, `QuantumOS ready`, the `NET:`
self-test) still print, so a quiet boot is legible; only the repeating
chatter is gone. Measured over a 9 s boot, the ~76 repeating lines a normal
boot emits drop to a single one-time line, with `qsh` fully usable.

The services still run and do their work; they just stop narrating. The
default boot is unchanged — `quiet` is strictly opt-in, so every CI gate
that depends on the normal boot output still holds. `make ci-smoke-quiet`
boots `-append quiet`, types `help`, and asserts *both* that the shell
still answers *and* that the heartbeat/chatter is gone. (The kannaka lab
harness exposes this as `lab-qos-boot --quiet`, which pairs with
`--network` for a clean networked shell on a cloud box.)

## The screen console: output on real hardware (epic #101)

On a laptop with no serial port, everything above still *works* — and is
completely invisible: input arrives via the PS/2 keyboard, but the boot
log, `qsh`, and even panics used to speak COM1-only. The screen console
(`kernel/src/vga.c`) makes the machine's own display a first-class
output device.

**API** (`kernel/include/kernel/vga.h`):

| Function | Role |
|---|---|
| `vga_console_enable()` | Take over the 80x25 text screen from the boot splash; called after `splash_ready()` on text-mode boots only (`!fb_available()`) |
| `vga_console_active()` | True once enabled — the tee points check this |
| `vga_console_putc(c)` | Write one char (`\n`/`\r`/`\b`/tab handled). Does NOT move the HW cursor — see `vga_console_sync` |
| `vga_console_puts(s)` | Whole string under an IRQ guard (interrupt-context `boot_log`s would otherwise tear lines), then one cursor sync |
| `vga_console_sync()` | Move the HW cursor to the pen position — 4 port writes, batched once per write because every port write is a trap under QEMU TCG |
| `vga_panic_banner(msg)` | Red panic banner, independent of console state; called from `boot_panic` (boot.S) so early bring-up failures are visible without a serial cable. Skipped when a linear framebuffer owns the display |

**Tee points**: `console_write()` (the `SYS_CONS` sink), `boot_log()`,
and `user_console_write()` (the `SYS_WRITE` sink — every ring-3
program's `[user pid=N]` output) write serial first, then the screen
when the console is active. The `SYS_WRITE` tee was the third
real-laptop finding: citizens ran to a clean exit 0 while their output
went only to a serial port that did not exist. Serial remains
authoritative — every CI gate reads it, none changed.
`console_write` also *latches off* a dead COM1 (transmit register never
drains within the spin cap): a machine with no UART behind 0x3F8 pays
the spin once, not per byte forever, and the screen carries on alone.

**Why it wraps instead of scrolling** — two designs were rejected with
evidence. A memmove scroll costs ~4000 `0xB8000` accesses per line, each
an MMIO callback under QEMU TCG, all with interrupts off: it starved the
ring-3 services outright (ghostd's field never synchronized, heartbeats
missed, watchdog reborn-storms) and broke the paradoxd/ghostd coupling
CI gate on ~half of runs. CRTC start-address panning is cheap but QEMU's
text renderer places the origin at **2x** the programmed start value
while real VGA uses 1x (character units) — verified by poking the CRTC
from the QEMU monitor and screendumping — so no single value renders
correctly on both. The wrap console (pen returns to the top and clears
ahead of itself; a moving blank separator marks the newest line) costs a
flat 160 cell writes per line and no CRTC state at all.

**Booting into it**: `make iso` builds a GRUB ISO with three menu
entries backed by two images of the same kernel. GRUB honours the
multiboot header's video request over `gfxpayload` (verified:
`text`/`keep` still produced a linear framebuffer), so the console
entries boot `kernel-console.elf` — built with `-DMB1_TEXT_ONLY`, no
video request → VGA text → screen console. The default
**QuantumOS (console)** entry adds `quiet`: on the first real-laptop
boot the demo kernel's narration outran the shell prompt faster than a
human could type, so the default is the usable interactive machine and
**console, verbose kernel log** is the debugging entry. The
**graphical wave field** entry boots the video-requesting image
(1024x768 splash + live field view; its text stays on COM1).

**Hardware without a COM1 UART** — the first real-laptop boot froze at
the 45% splash stage: with no UART behind 0x3F8 every port read floats
to `0xFF`, so LSR permanently reads "data ready" and `console_init`'s
RX rescue drain spun forever (reproduced exactly in QEMU with
`-serial none`). `console_init` therefore probes first: a scratch-
register echo test (`0x5A`/`0xA5` written to SCR and read back — a
floating bus can't echo) sets the presence flag exposed as
`console_com1_present()`. When absent, all COM1 setup is skipped, the
transmit tee is latched off, IRQ4 stays masked, and the boot log says
so honestly: `CONS: no COM1 UART detected — screen/keyboard console
only`. The RX drain is bounded (64 bytes, bails on a floating `0xFF`
LSR) and the PS/2 stale-byte drain gets the same treatment — nothing in
the console layer may ever spin on a status bit the hardware can float.

**CI gates** (the `Real-Hardware Boot Path` job):

- `make ci-smoke-iso` — boots the ISO with `-cdrom` (the real GRUB
  handoff, not QEMU's `-kernel` shortcut) and asserts the boot gates,
  `CONS: screen console active (VGA text 80x25)`, the shell session, and
  a citizen gate, all from one boot.
- `make ci-smoke-kbd` — drives `qsh` purely via PS/2 scancodes injected
  through the QEMU monitor (`sendkey`); serial carries **no input** that
  run, so the executed `help` proves the i8042/IRQ1 path a real laptop
  keyboard uses.
- `make ci-smoke-noserial` — boots with `-serial none` (no COM1 device
  at all: ports float exactly like serial-less hardware) and verifies
  the Lamport boot attestation arriving on COM2 — proof that full
  service bring-up completes with no console serial port.
- `make ci-smoke-screen` — dumps the live VGA text cells through the
  QEMU monitor (`scripts/check_vga_text.py`) and asserts a ring-3
  `[user pid=` line is actually ON the screen. Serial gates can't see
  the display; this one reads what the operator reads.
- `make ci-smoke-iso-uefi` — the same ISO under OVMF (edk2 UEFI
  firmware, no CSM; `scripts/test_qos_iso_uefi.py`). UEFI has no VGA
  text mode, so the console image gets the GOP framebuffer instead and
  the kernel, seeing a framebuffer its header never asked for, renders
  the screen console on it: `CONS: screen console active (framebuffer
  1024x768, 128x48 cells)`. Same shell + citizen gates, plus the
  boot-stack high-water and identity-map lines, and the absence of GRUB's
  `out of memory`, the VGA-text line and any page fault. ADR-0031 has the
  three faults this gate stands on.

**UEFI (ADR-0031).** The console entries add `insmod all_video` and
`set gfxpayload=1024x768x32`: on BIOS GRUB that changes nothing (the
header's missing video request still wins, VGA text), on UEFI it is the
only way GRUB passes framebuffer info at all. `vga.c` keeps the whole line
discipline (wrap console, SGR, pen); `cons_cell`/`cons_clear_row`/the
cursor dispatch to `fb_cons_cell`/`fb_cons_cursor` (`fb.c`) when
`fb_text_console_mode()` is true. Cells are the 8x8 glyph with doubled
rows (8x16; 2x on panels 1600 px and wider), the full printable-ASCII
font (`scripts/check_fb_font.py` enforces coverage), grid capped at
160x64. The panic banner takes the same route, so an early failure is
visible on a UEFI screen too.

## Known limits / follow-ups

- The graphical (framebuffer) boot has no text console — its shell
  output stays on COM1 on both firmwares; the on-screen console belongs
  to the console image (VGA text on BIOS, framebuffer cells on UEFI).
- Keyboard on UEFI-only machines: the built-in laptop keyboard is
  normally an i8042 device and works; an external USB keyboard needs the
  firmware's PS/2 legacy emulation, which UEFI-only firmware with CSM
  off often does not provide. There is no native USB HID driver (a
  separate, larger issue); fall back to COM1 where the board has one.
  QEMU/OVMF emulates an i8042, so the CI gate does not prove this on
  real hardware — reports go to #265.
- The multiboot memory map is not read; the PMM assumes 128 MB
  (`memory.c` hardcodes it), so RAM beyond that is ignored on real
  machines.
- Peer IPC capabilities (qsh ↔ ghostd) are not re-minted on watchdog
  restart — only declared *resource* caps are. A reborn shell keeps its
  console but loses `ghost` until this service.c limitation is fixed
  (shared with the other demo peers).
- Extended PS/2 keys (arrows, etc.) are swallowed; no line history.
- Next phases of epic #62: an embedded initrd + read-only VFS (`ls`,
  `cat`, issue #64), then `SYS_SPAWN`/`SYS_WAITPID` so the shell can run
  programs from the filesystem (issue #65).
