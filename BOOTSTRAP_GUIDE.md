# QuantumOS Bootstrap Guide

Getting from a clean checkout to a booting kernel. For the architecture see
[`docs/adr/`](docs/adr/); for the contribution workflow see [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Prerequisites

- **GCC** — the system compiler is auto-detected; an `x86_64-elf-*` cross-compiler is used if
  present but is **not** required (Ubuntu 24.04 and 26.04 build with system gcc; CI runs on
  26.04). The flags pin the ISA to plain `x86-64` whatever the compiler's default, and
  `make ci-isa-baseline` checks the binaries (#346).
- **QEMU** (`qemu-system-x86`) for booting and the CI gates.
- **GNU make**, `nasm`. For the GRUB ISO: `grub-pc-bin`, `grub-efi-amd64-bin` (the UEFI half of
  the hybrid image), `xorriso`, `mtools`; `ovmf` to test the UEFI boot in QEMU.
- **Rust** (stable `rustc` + `cargo` via rustup) with the bare-metal target:
  `rustup target add x86_64-unknown-none`. The Rust citizens in `user/rust/` (ADR-0032, a cargo
  workspace built `--offline` with no external dependencies) are part of every build, so the
  kernel does not build without it.
- Windows: use WSL2 (Ubuntu). The kernel builds and boots inside WSL.

```bash
make install-deps      # Ubuntu/Debian: install the toolchain
```

## Build and boot

```bash
make                   # build the kernel (debug); produces build/x86_64/kernel.elf + kernel.elf32
make run               # boot it in QEMU on the serial console (-serial stdio)
make ci-smoke          # verify your setup: the full boot + gate suite
```

QuantumOS boots via **Multiboot v1** (`boot.S` carries the MB1 header). QEMU's `-kernel` refuses a
64-bit ELF, so `make run` boots the 32-bit `kernel.elf32` produced by `objcopy` — that is the
correct artifact for the `-kernel` path, not `kernel.elf`.

The version string in the boot banner comes from the repo-root `VERSION` file (injected as
`-DQOS_VERSION` by the Makefile); it is not hardcoded in the source.

## Boot it on real hardware

```bash
make build/x86_64/kernel.iso    # hybrid BIOS+UEFI GRUB ISO (needs grub-pc-bin grub-efi-amd64-bin xorriso mtools)
make ci-smoke-iso               # sanity-check the exact image via the real GRUB handoff (SeaBIOS)
make ci-smoke-iso-uefi          # the same image under UEFI firmware (OVMF; needs the `ovmf` package)
```

Write the ISO to a USB stick (`dd` on Linux, Rufus "DD Image" mode on Windows) and boot it —
Legacy/CSM or UEFI-only, both work (ADR-0031). The default **console** entry drops to an
interactive `qsh` on the machine's own screen and keyboard. Limits: the keyboard must be an
i8042 device (built-in laptop keyboards are; a USB keyboard needs the firmware's PS/2 legacy
emulation, which UEFI-only firmware often lacks — no USB HID driver yet), and RAM beyond the
firmware's memory map is not used.

## First boot

Once the timer starts, the ring-3 citizens run and self-test. A healthy boot ends with
`QuantumOS ready` and lines like `GHOSTD: 3/3 RECALL OK`. `make ci-smoke` gates on exactly those
un-echoable signals — if it passes, your toolchain is good.

## Where next

- Build a richer picture from the [Architecture Decision Records](docs/adr/).
- Drive a running kernel from the host with the `quantumos-host-tools` package (`scripts/`).
- Try the zero-install [browser demo](https://kannaka-labs.github.io/QuantumOS/).
