# QuantumOS Bootstrap Guide

Getting from a clean checkout to a booting kernel. For the architecture see
[`docs/adr/`](docs/adr/); for the contribution workflow see [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Prerequisites

- **GCC** — the system compiler is auto-detected; an `x86_64-elf-*` cross-compiler is used if
  present but is **not** required (Ubuntu 24.04 builds with system gcc).
- **QEMU** (`qemu-system-x86`) for booting and the CI gates.
- **GNU make**, `nasm`. For the GRUB ISO: `grub-pc-bin`, `xorriso`, `mtools`.
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
make build/x86_64/kernel.iso    # GRUB ISO (BIOS/CSM; needs grub-pc-bin xorriso mtools)
make ci-smoke-iso               # sanity-check the exact image via the real GRUB handoff
```

Write the ISO to a USB stick (`dd` on Linux, Rufus "DD Image" mode on Windows) and boot with
Legacy/CSM enabled. The default **console** entry drops to an interactive `qsh` on the machine's
own screen and keyboard. Limits: BIOS boot only (no UEFI yet), PS/2 keyboard, RAM above 128 MB
currently ignored.

## First boot

Once the timer starts, the ring-3 citizens run and self-test. A healthy boot ends with
`QuantumOS ready` and lines like `GHOSTD: 3/3 RECALL OK`. `make ci-smoke` gates on exactly those
un-echoable signals — if it passes, your toolchain is good.

## Where next

- Build a richer picture from the [Architecture Decision Records](docs/adr/).
- Drive a running kernel from the host with the `quantumos-host-tools` package (`scripts/`).
- Try the zero-install [browser demo](https://kannaka-labs.github.io/QuantumOS/).
