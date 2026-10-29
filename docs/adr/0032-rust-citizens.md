# 32. Rust citizens: the path for running the Kannaka tools inside QuantumOS

Date: 2026-10-29
Status: Accepted (slice 1 shipped; later slices are separate PRs)

## Context

Issue #263 asks for the Kannaka tools (kannaka-memory, kannaka-tui, kannaka-crystal,
kannaka-bench, Kannaka Scientist) to run *inside* QuantumOS, and for the path to be chosen
before the first line of tool code is ported. Today the guest has the kernel holographic field
(`SYS_IMPRINT`/`SYS_RECALL`, ADR-0006) with `user/kannakad.c` as its C reference client, and
epic #127 bridges the field to the host's Kannaka store over MCP. The tools themselves are Rust
with `std` and sizeable dependency trees; QuantumOS ring 3 is freestanding C on `libq`, no FPU
(ADR-0004), no threads, a 512 KB image bound per program (`user/user.ld`), and 128 MiB on the
console machines.

Three paths were on the table:

1. **Rust `no_std` citizens.** A bare-metal Rust target, a thin syscall crate twinning
   `user/usys.h`, later an allocator over the heap arena. Port the parts of kannaka-memory that
   are integer-only or can be made so.
2. **Thin native clients.** C programs in ring 3 that talk to Kannaka running elsewhere over
   the TCP stack or COM2. Fast, but the work does not run inside QuantumOS.
3. **A larger ring-3 runtime** (a WASM interpreter as a citizen) that the tools are compiled to.

## Decision

**Path 1.** Rust `no_std` programs are first-class citizens: built by the Makefile with plain
`rustc` for `x86_64-unknown-none`, linked by the same gcc command and `user.ld` as every C
citizen, carried on the initrd, started by the shell through `SYS_SPAWN`. No cargo, no
`std`, no allocator in slice 1, nothing the kernel can tell apart from a C program.

Why 1 over 2: path 2 is already what epic #127 does (the host runs Kannaka; the guest asks over
a bridge). It answers "can the guest use Kannaka" and not "can Kannaka run in the guest", which
is the question #263 poses. Why 1 over 3: a WASM runtime would itself have to be written or
ported as a `no_std` citizen first, so path 3 contains path 1 as its first step and adds an
interpreter's memory and a second ABI on top. If a tool ever needs it, it is a later ADR.

### Slice 1, as shipped (this ADR's evidence)

- `user/rust/qsys.rs`: the syscall layer. `int 0x80`, number in rax, arguments in rdi/rsi/rdx,
  result in rax, the kernel preserving every other register, exactly as the C wrappers assume.
  The field request/response twins are `#[repr(C)]` with their sizes pinned at compile time
  (76 and 136 bytes), the Rust form of usys.h's `_Static_assert` lines, so a layout drift fails
  the build on both sides.
- `user/rust/rhello.rs`: `/bin/rhello`. It prints its pid, has `SYS_AUDIT` copy ledger stats
  into a buffer on its own stack (the kernel-to-user pointer direction), then calls `SYS_RECALL`
  with a twin request and expects exactly -4: a spawned program holds no capability over the
  field, and the refusal is the proof that the gate guards Rust callers as it guards C ones. It
  exits 7. A panic prints `RHELLO: PANIC` and exits 99 rather than spinning.
- `Makefile`: `RUST_PROGS`, one pattern rule to a static archive, one link rule; `rhello` joins
  `INITRD_BIN_PROGS`. `make install-deps` installs rustup and the target.
- `make ci-smoke` and the Integration job run `run /bin/rhello` and gate on all three lines plus
  `exited (code 7)`; any `RHELLO: FAIL` or `RHELLO: PANIC` line fails the gate.

### Toolchain facts, measured on debain2 (rustc 1.99.1, gcc, binutils ld)

| what | result |
|---|---|
| `x86_64-unknown-none` object code model | links at `USER_VBASE` (0x40000000) with no flags beyond the C citizens' link line; relocation-model=static made no difference (byte-identical ELF) |
| SSE / x87 instructions in the linked program | 0 (the target is soft-float, no SSE, no red zone: ADR-0004's rules in Rust's terms) |
| `rhello.elf` text, linked like a C citizen | 179,576 bytes |
| the same with `-Wl,--gc-sections` | 9,653 bytes (hello.elf, C: 9,576 bytes) |
| the same at `-C opt-level=z` | 7,477 bytes |
| file size without `--strip-debug` | 986,576 bytes (core's prebuilt DWARF that `user.ld` keeps) |
| ELF loader | accepted unchanged: ET_EXEC, two PT_LOAD segments at and above `USER_VBASE` |
| first boot | all three gate lines and the exit code, no kernel change |

The 179 KB figure is the number to remember: every slice index in `no_std` Rust has a panic
path, every panic path builds `core::fmt::Arguments`, and the staticlib therefore references the
whole formatting machinery, floats included. `--gc-sections` removes what is not reachable from
`_start`; what remains reachable is the bounds-check panics' formatting, about 2 KB. Programs
that want the floor lower use `opt-level=z` or write their hot loops index-free.

### What the ADR answers for #263

- **Memory budget.** A Rust citizen with no allocator costs the same as a C one (above). The
  image bound of 512 KB in `user.ld` and the 16 KB stack apply unchanged. kannaka-memory's core
  math is already in the kernel as the field (Q16/Q15 integers, 64-byte patterns); a ring-3 port
  of the host's store would need an allocator over `libq`'s heap arena (slice 2) and must stay
  within the bound, which rules out the host's `Vec<f32>` representations outright and makes the
  integer field the representation, not a port target.
- **The FPU question.** Closed by the target: `x86_64-unknown-none` is soft-float. Float
  arithmetic in Rust source would compile to software routines rather than clobbering a
  register set the kernel never saves (ADR-0004); it is slow but not corrupting. The project
  rule stays "integer-only" and `qsys` exposes nothing with a float in it.
- **Capabilities.** Unchanged and already demonstrated: a Rust caller holds what the spawn gives
  it, nothing. A Rust tool that needs the field must be a *service* with a declarative grant
  (`CAP_RESOURCE_FIELD` over its region), the way `kannakad` is, or receive a derived capability
  from the shell through `SYS_CAP_DERIVE`. Neither needs a new kernel mechanism.
- **Effort.** Slice 1 is one PR. Slice 2 (allocator + a `qsys` crate proper, cargo-built) is one
  PR. The first Kannaka tool slice is the one whose scope the follow-up issue sets.

## Consequences

**Positive.**
- The CI proves, every run, that Rust code runs in ring 3 and obeys the same ABI and the same
  capability gate as C. Every later Rust citizen inherits that proof.
- No kernel change, no new syscall, no new loader path. The contract freeze (ADR-0020) is
  untouched.
- `rustc` alone, no cargo and no lockfile, keeps the build deterministic and the dependency
  surface at `core` + `compiler_builtins`.

**Negative and residual.**
- A second toolchain. `make` now needs stable `rustc` with the bare-metal target; the Makefile
  says how to get it and `install-deps` does it, but a box without Rust no longer builds the
  kernel at all. Making the Rust citizens optional would make the gate optional, which is worse.
- No allocator yet, so nothing with a `Vec` runs. That is slice 2, deliberately separate.
- `panic=abort` with a handler that only prints: no unwinding, no backtrace. Bugs surface as
  `RHELLO: PANIC` and an exit code, which is what a kernel without unwinding support can offer.
- The 179 KB trap is one `--gc-sections` away from returning whenever a link line is copied
  without it; the Makefile comment and this table are the guard.
- Rust's bounds-check panics mean a Rust citizen can still exit through a path the C citizens do
  not have. The handler makes it loud rather than silent.

## Evidence

- PR for #263 slice 1: `user/rust/qsys.rs`, `user/rust/rhello.rs`, `Makefile` (RUST_PROGS rules,
  INITRD_BIN_PROGS, the `RHELLO` gate block in `ci-smoke`, `install-deps`),
  `.github/workflows/ci.yml` (the `rustup target add` step in every job that builds, the
  Integration session and SHELL_GATES).
- Gate lines: `RHELLO: rust no_std citizen on QuantumOS pid=`, `RHELLO: SYS_AUDIT stats copied `,
  `RHELLO: SYS_RECALL capless -> -4 (EPERM)`, `exited (code 7)`.
- Measurements above: debain2, 2026-10-29, QEMU 10.0.11 TCG, `-m 128M`.
