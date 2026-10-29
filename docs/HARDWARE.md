# Hardware reports: which real machines boot QuantumOS?

This page is built from real boots on real machines, successes and
failures alike. Everything else in the repository is measured under QEMU
(SeaBIOS and OVMF); only the rows below say anything about actual
hardware. One machine so far. Add yours (issue #265).

**To add a report:** open a
[Hardware report](https://github.com/kannaka-labs/QuantumOS/issues/new?template=hardware-report.yml)
issue. It asks for exactly the columns of the table below plus a photo of
the screen. A failed boot with a photo of where it stopped is as useful as
a success. A maintainer copies the report into the table.

## Reports

| Machine / board | Firmware mode | QuantumOS version | How far it got | Detected (disk, NIC, keyboard) | Notes |
|---|---|---|---|---|---|
| ASUS laptop (model not recorded) | Legacy/CSM | `main` as of 2026-07-07 (before the `v0.4.0` tag; the kernel log then read `v0.1`), reflashed 2026-07-08 after #112 | `qsh` usable on its own keyboard: all builtins, all four citizens' verdicts on screen; the **graphical wave field** entry boots to the field view | keyboard: built-in i8042, works; serial: no COM1 UART; disk and NIC: not recorded | First real-hardware boot. Four findings in one evening, each fixed and gated in CI within a day: froze at the 45 % splash stage because a missing COM1 reads as 0xFF forever (#107); the narrated boot buried the prompt, so the default entry became `quiet` (#108); citizen output went to the absent serial port only (#109); the field caption read "OST  E D  E" because the font lacked glyphs (#111). Reported by the maintainer. |

## What to look for

The boot climbs a fixed ladder. Report the highest rung you saw, and for
a failure the exact spot. The default GRUB entry, **QuantumOS (console)**,
boots quiet; the **console, verbose kernel log** entry prints every line
and is the one to use when something stops.

1. **GRUB menu.** Four entries, 3 s timeout. If you never see it, the
   firmware did not boot the stick (wrong boot mode, Secure Boot on, or an
   ISO written in a non-DD mode).
2. **Splash.** A progress bar with a percent stage: 5 % hardware
   abstraction layer, 20 % memory management, 35 % interrupt controller,
   45 % descriptor tables + syscalls, 60 % capabilities + quantum
   resources, 75 % scheduler + services, 90 % user-space processes,
   98 % timer + interrupts. A freeze at a stage is a report in itself.
3. **`QuantumOS ready`.** The kernel finished bring-up and the screen
   console is active.
4. **`qsh>` prompt shown.**
5. **`qsh` usable on its own keyboard.** `help` prints the command list
   and `run /bin/consciousnessd` ends with a verdict line on screen.

A red on-screen banner is a kernel panic. Photograph it: the text names
the fault.

## What the kernel reports about the machine

These lines appear in the kernel log (verbose entry; they also reach COM1
at 115200 8N1 where the board has one). Copy the ones you see into the
"Detected" column.

| Subsystem | Lines | Meaning |
|---|---|---|
| Console | `CONS: screen console active (VGA text 80x25)` | BIOS/CSM boot, VGA text mode |
| | `CONS: screen console active (framebuffer WxH, CxR cells)` | UEFI boot, console drawn on the GOP framebuffer |
| | `CONS: no COM1 UART detected — screen/keyboard console only` | no serial port (normal on laptops) |
| Disk | `ATA: disk present (LBA28), sectors: N` | first legacy IDE/ATA device answered IDENTIFY |
| | `ATA: no disk — persistence disabled` and the other `ATA: … — persistence disabled` lines | nothing usable on the legacy IDE ports (see the SATA note below) |
| NIC | `NET: rtl8139 up — MAC: …`, `NET: e1000 up - MAC: …`, or the `virtio-net` lines | a supported NIC was found and brought up |
| | `NET: no network device found` | no supported NIC on the PCI bus |
| RAM | `Total frames:` / `Free frames:` | sized from the firmware's memory map, capped at 1 GB |
| | `PMMHIGH: skipped (no RAM above 128 MB on this boot)` | the map reported 128 MB or less |
| Boot (main after #337) | `STACK: boot stack high-water N of M bytes`, `PGTBL: low identity map intact` | the init chain fitted its stack and the identity map survived |

The kernel does not probe for the keyboard. Whether typing works at the
`qsh>` prompt is the keyboard report.

## What the drivers cover

QuantumOS is deliberately small. On a real machine, expect:

- **Firmware:** BIOS and Legacy/CSM boots land in VGA text mode. UEFI-only
  machines (no CSM option) boot on `main` since #337 and get the same
  console on the firmware's framebuffer
  ([ADR-0031](adr/0031-uefi-boot-framebuffer-console.md)). The `v0.6.2`
  release ISO is the first with an EFI boot image; `v0.6.1`'s carries none,
  so on such a machine it is not even offered as a boot device. Secure Boot
  must be off.
- **Keyboard:** i8042 (PS/2), scancode set 1. A laptop's built-in keyboard
  is normally one. An external USB keyboard works only through the
  firmware's PS/2 legacy emulation, which Legacy/CSM mode usually
  provides and UEFI-only firmware with CSM off often does not. There is no
  USB HID driver.
- **Display:** VGA text (BIOS) or a linear framebuffer at 1024x768x32
  requested from GRUB (the **graphical wave field** entry, and every entry
  on UEFI).
- **Disk:** ATA PIO on the legacy IDE ports. A SATA controller in AHCI
  mode exposes no legacy ports, so the disk is expected to report absent;
  if the firmware has an IDE/compatibility SATA setting, note which you
  used. NVMe and USB mass storage are not supported.
- **Network:** rtl8139, Intel e1000, virtio-net. Anything else reports
  "no network device found". Wi-Fi is not supported.
- **Serial:** a 16550 UART at COM1 (0x3F8) for the log and a second
  console; absent on most laptops, which is fine.
- **RAM:** read from the multiboot memory map (#215), capped at the 1 GB
  identity map.

## A warning before you type `sync`

The ATA driver talks to the first legacy IDE device, which on a laptop can
be the internal drive. `sync` writes a QuantumOS superblock to sector 0
of that device. It refuses unless the volume is already QuantumOS's own
or sector 0 is all zeros (#115), and CI proves the refusal leaves the
image byte-identical. Even so: do not type `sync` on a machine whose disk
you care about. Everything else in `qsh` is read-only toward the disk.

## Writing the stick

Build or download `kernel.iso` (release assets, or
`make build/x86_64/kernel.iso`, which needs `grub-pc-bin`,
`grub-efi-amd64-bin`, `xorriso`, `mtools`). Write it raw: `dd if=kernel.iso
of=/dev/sdX bs=4M` on Linux, or Rufus in **DD Image** mode on Windows. A
stick written in ISO mode boots differently from the tested image. The
README's "Boot it on real hardware (USB)" section has the full steps.
