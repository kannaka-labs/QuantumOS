# ADR-0031: UEFI boot — Multiboot1 kept, the screen console on the GOP framebuffer

**Date:** 2026-10-29
**Status:** Accepted (#264)

## Context

The real-hardware path (epic #101) boots from a USB stick on BIOS/CSM machines: GRUB
loads the 32-bit Multiboot1 image, leaves the machine in VGA text mode, and the kernel
runs its screen console in the 80x25 cell buffer at `0xB8000`. Many current laptops are
UEFI-only. Such firmware has no VGA text mode; the only display it offers is a GOP linear
framebuffer, and `0xB8000` is plain RAM.

The deferred-panel verdict in `docs/adr/README.md` was "UEFI boot: revisit at a real
CSM-less target". Issue #264 asked first for a measurement: boot the ISO under OVMF and
record how far it gets. It got nowhere, three times over, and each stop was a distinct
fault:

1. **The ISO had no EFI boot image at all.** `grub-mkrescue` adds the EFI El Torito entry
   only when `grub-efi-amd64-bin` is installed; the build host and CI installed
   `grub-pc-bin` alone. OVMF reported the DVD boot option "Not Found" and dropped to its
   shell. The "hybrid ISO" the issue assumed had never been built.
2. **GRUB could not place the kernel.** With the EFI modules present, GRUB started and
   showed the menu, then `multiboot /boot/kernel-console.elf` failed with
   `error: out of memory`. GRUB's own trace (`set debug=relocator,multiboot_loader`)
   showed segments 0 and 1 (code, data) allocating, then segment 2 — a `PT_LOAD` of
   64 MB + 8 KB at physical 17 MB with `filesz 0` — refused. `kernel/link.ld` declared
   the stack and heap as `(NOLOAD)` output sections, but ld still emits a `PT_LOAD` for an
   allocated NOLOAD section, and a Multiboot loader must then reserve that exact physical
   range before handing over. On BIOS the range is free and the reservation succeeds; on
   UEFI the loader asks the firmware for it page by page, and with boot-services
   allocations scattered through 17–81 MB the request fails.
3. **The framebuffer mapping vanished mid-boot.** With the segment gone the kernel booted
   to `qsh` over serial, took the framebuffer, drew the splash — and page-faulted on the
   first screen clear after interrupts came up. A page walk printed from the fault
   handler showed CR3 = the boot PML4, PDPT slot 2 → `fb_map_pd`, and `fb_map_pd[0] == 0`.
   A gdb watchpoint on that word caught the writer: `service_list`, called from the
   boot-time service self-test, with `RSP = 0x3E55D8` — below the bottom of the 16 KB boot
   stack at `0x3E7000`. The init chain runs about 23.5 KB deep (measured: high-water
   24104 bytes), and the overflow landed on the page tables that sat just below the
   stack in `.bss`: `fb_map_pd`, then `boot_pd[187..511]`. This happened on **every**
   boot, BIOS and `-kernel` included, and was invisible: text-mode boots never use
   `fb_map_pd`, the BIOS framebuffer at `0xFD000000` maps through slot 488 of it, and a
   128 MB machine never touches `boot_pd` entries above 374 MB. OVMF's framebuffer at
   `0x80000000` maps through slot 0 — the exact word the overflow zeroed.

With all three fixed, one question remained: how does the console image, which asks for
no video mode, get a display on UEFI at all? Measured under OVMF with the fixed kernel:

| GRUB entry for the console image | OVMF | SeaBIOS |
|---|---|---|
| `multiboot` alone | no framebuffer info, kernel writes to `0xB8000` (blind) | VGA text |
| `insmod all_video` | no framebuffer info | VGA text |
| `insmod all_video` + `set gfxpayload=1024x768x32` | **1024x768x32 framebuffer** | **VGA text** (header video request still wins) |

## Decision

1. **Stay on Multiboot1.** Nothing in the UEFI path needed Multiboot2: GRUB's EFI
   `multiboot` loader hands over a correct MB1 info block (memory map, cmdline,
   framebuffer) once the kernel image stops demanding a 64 MB reservation. The stack and
   heap are now linker **symbols** in the `ram` region, not output sections, so the ELF
   carries only the two real `PT_LOAD`s. The kernel never relied on the loader for that
   region: `kheap_init` writes its own block header and `pmm_init` reserves the frames.
2. **Build the ISO hybrid.** `grub-efi-amd64-bin` joins the build dependencies (CI, README,
   `BOOTSTRAP_GUIDE.md`); the ISO rule warns loudly when `xorriso` finds no UEFI El
   Torito entry. The console menu entries add `insmod all_video` and
   `set gfxpayload=1024x768x32`, which changes nothing on BIOS (table above).
3. **The kernel decides from its own header.** `boot.S` exports `mb1_header`. If a
   framebuffer arrives and header flag bit 2 was never set — the console image — the
   firmware had no text mode to give, and `fb_text_console_mode()` is true: the splash
   stays light (banner and bar, no interference frames), the screen console renders on the
   framebuffer, the live field view is not drawn, and the panic banner goes there too. The
   video-requesting image behaves as before (field view, text on COM1).
4. **One line discipline, two cell stores.** `vga.c` keeps the wrap console, SGR colours
   and pen; `cons_cell`/`cons_clear_row`/the cursor dispatch to `fb_cons_cell` /
   `fb_cons_cursor` in `fb.c` when on the framebuffer. Cells are the 8x8 glyph with
   doubled rows (8x16), scaled 2x on panels 1600 px and wider; the grid is capped at
   160x64 so the shadow store the cursor needs stays a bounded static array. The font is
   now the full printable ASCII set (public-domain IBM VGA 8x8, MSB-left), and
   `scripts/check_fb_font.py` fails the build if any byte 32..126 is missing or out of
   order. 1024x768 gives 128x48 cells.
5. **The boot stack is 64 KB, sits below the page tables, and is checked.** `.bss` order
   in `boot.S` is now stack first, tables above it, so an overflow can never reach a page
   table again. At the end of `kernel_init`, `boot_stack_and_tables_check()` reports
   `STACK: boot stack high-water N of M bytes`, panics if the bottom 4 KB guard zone was
   touched, and verifies every `boot_pd` entry is still `(i * 2 MB) | P|RW|PS` (A/D bits
   masked), reporting `PGTBL: low identity map intact`. The page-fault handler now prints
   CR3 and the page walk of the faulting address for kernel-mode faults.
6. **A CI gate under real UEFI firmware.** `make ci-smoke-iso-uefi`
   (`scripts/test_qos_iso_uefi.py`) boots the same ISO under OVMF via pflash, waits for the
   shell before typing (bytes piped from t=0 would land in the firmware countdown or the
   GRUB menu), and asserts: an EFI El Torito entry, GRUB started, the
   framebuffer-console line, the stack and page-table lines, the shell session and the
   citizen verdict, the quiet contract, and the **absence** of `error: out of memory`,
   the VGA-text line, `No framebuffer`, any page fault and the overflow line. It runs in
   the `Real-Hardware Boot Path` job next to `ci-smoke-iso`.

## Consequences

**Positive.** A UEFI-only machine with no CSM can now boot the stock ISO to an interactive
`qsh` on its own screen, with the same shell, kernel log and panic banner the BIOS path
shows. The latent boot-stack overflow — corrupting page tables on every boot for as long
as the service self-test has existed — is fixed and can no longer recur silently. The
kernel image lost a 64 MB loader demand it never needed, which also makes it friendlier
to any loader that honours `PT_LOAD` ranges strictly.

**Negative / honest limits.**
- **Keyboard.** QEMU/OVMF emulates an i8042, so the gate's PS/2 path is not proof for
  real machines. On real UEFI-only hardware the built-in laptop keyboard is usually an
  i8042 device and works; an external **USB keyboard** depends on the firmware's legacy
  PS/2 emulation, which UEFI-only firmware with CSM disabled often does not provide. There
  is no USB HID driver (a separate, larger issue). The fallback is a serial console on
  COM1 where the board has one. Hardware reports go to #265.
- **Resolution.** The console entries request 1024x768x32; GRUB picks the nearest GOP
  mode the firmware offers, so cell geometry varies by machine (the kernel reports it).
  Panels at 1600 px and wider get 2x cells; the grid is capped at 160x64.
- **The graphical entry on UEFI** keeps its text on COM1, as on BIOS: the field view
  still has no text console.
- **The stack check is a sample**, taken at the end of init; later kernel-thread stacks
  are kmalloc'd and separate. The guard zone catches gross overflows, not a near miss.
- The 64 KB stack adds 48 KB of `.bss`.
- `grub-efi-amd64-bin` is one more build dependency; a BIOS-only ISO still builds, with a
  warning, and the UEFI gate refuses it.

## Evidence

- Linker: `kernel/link.ld` (`__stack_top`, `__heap_start`, `__heap_end` as symbols);
  `readelf -lW build/x86_64/kernel-console.elf32` shows two `LOAD` segments.
- Header export and stack/table layout: `kernel/src/boot.S` (`mb1_header`,
  `BOOT_STACK_SIZE 65536`, `.bss` order).
- Mode detection and cell backend: `kernel/src/fb.c` (`fb_text_console_mode`,
  `fb_cons_cell`, `fb_cons_cursor`, `font_ascii`); console dispatch: `kernel/src/vga.c`
  (`cons_cell`, `cons_clear_row`, `vga_console_enable`, `vga_console_sync`,
  `panic_row`); boot-time checks and the `CONS:` line: `kernel/src/main.c`
  (`boot_stack_and_tables_check`); page-walk dump: `kernel/src/interrupts.c`.
- ISO: `Makefile` (`kernel.iso` rule, `ci-smoke-iso-uefi`); gate:
  `scripts/test_qos_iso_uefi.py`; font contract: `scripts/check_fb_font.py`.
- Measured on debain2 (QEMU 10.0, OVMF 2025.02): three consecutive UEFI gate runs green,
  framebuffer 1024x768 → 128x48 cells, boot to the citizen gate in ~14 s; the BIOS gates
  (`ci-smoke-iso`, `ci-smoke-screen`, `ci-smoke-kbd`, `ci-smoke-noserial`) unchanged and
  green; `STACK: boot stack high-water 24104 of 65536 bytes` on both firmwares.
