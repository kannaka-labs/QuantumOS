# 32. Rust citizens: the path for running the Kannaka tools inside QuantumOS

Date: 2026-10-29
Status: Accepted (slice 1 shipped in #339; slices 2 and 3 in #343; the first Kannaka tool slice in
the second #340 PR)

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

### Slices 2 and 3, as shipped (#340)

- **One build path.** `user/rust` is a cargo workspace: the `qsys` crate and one staticlib crate
  per citizen, built by the Makefile with `cargo build --release --offline --target
  x86_64-unknown-none` into `build/x86_64/user/cargo/`, then linked by the same gcc line as
  before. `Cargo.lock` is committed and holds only the workspace's own crates; `--offline` makes a
  stray external dependency a build error rather than a download. The profile (panic=abort, -O2,
  no debug info) lives in the workspace `Cargo.toml`.
- **The full `usys.h` surface in `qsys`:** process, console, files, spawn/waitpid, audit,
  manifest, sysinfo, the field (imprint, recall, field_info) and cap_derive, with every twin's
  size pinned at compile time (76, 136, 40, 332, 24 bytes).
- **The allocator is libq's.** `qsys::heap::LibqAlloc` is a `GlobalAlloc` whose `alloc`,
  `dealloc` and `realloc` call `malloc`, `free` and `realloc` in `user/libq/heap.c`: the same
  static 64 KB bss arena, the same first-fit free list, the same 16-byte alignment (larger
  alignments are refused with null, which `alloc` turns into a loud panic exit). The Makefile
  links `libq.a` into every Rust citizen inside a `--start-group`; only a citizen that declares
  `#[global_allocator]` pulls `heap.o` and the arena in, so `/bin/rhello` keeps its no-heap floor.
  The compiler's own `memcpy`/`memset` (from `compiler_builtins`) are weak symbols, so they
  coexist with libq's without a duplicate-definition error (measured: `W memcpy`, `W memset`,
  `T malloc`, `T free` in the linked service).
- **A Rust service with authority.** `rfieldd` was embedded in the kernel like `kannakad` and
  registered by `citizens.c` with `grant_field = 1, field_region = 7` (the one region no C
  citizen names), re-minted and scrubbed at every start, monitored, heartbeating after its demo.
  Nothing in the kernel treats it as anything but another ELF service. (The next slice's
  `rkannaka` took over the region and these proofs; `rfieldd` itself is gone.)
- **The gate replaces "-4 is the proof" with a successful recall**, as the follow-up issue asked:
  a `Vec` grown to 1,000 entries, shrunk to 10, regrown, dropped and allocated again; three
  imprints into region 7; a byte-corrupted probe recalling the *exact* stored text
  (`RFIELD: winner="a rust citizen remembers the field"`); `SYS_FIELD_INFO` counting the three; and
  the negative control, a recall against region 1 refused with exactly -4. `rfieldd: FIELD
  VERIFIED` is the verdict line; any `RFIELD: FAIL`, `RFIELD: PANIC` or `FIELD FAILED` fails
  `make ci-smoke` and the Integration job.

| rfieldd, measured on debain2 | result |
|---|---|
| text / data / bss | 14,144 / 376 / 65,568 bytes (the bss is libq's arena) |
| SSE / x87 instructions | 0 |
| `/bin/rhello` text, cargo-built | 8,069 bytes (rustc-built in #339: 9,653) |

### The first Kannaka tool, as shipped (#340, the last section of the issue)

**What "kannaka-memory inside QuantumOS" means here.** Not a port of the host store: the kernel
field *is* the memory (ADR-0006: 64-byte patterns, Q15 energy, 8 slots per region, ranked recall
by resonance, retrieval reinforcement). The tool is kannaka-memory's *verbs* over it, in Rust,
inside the guest: `rkannaka` (`user/rust/rkannaka`), a service with the region-7 grant that
reads `/etc/kannaka.session` from the initrd (or the RAM overlay, which shadows it, so a session
can be replaced without a rebuild) and replays it at boot:

| verb | does | prints |
|---|---|---|
| `imprint <importance 0..1000> <text>` | importance → Q15 energy as the C kannakad maps it; SYS_IMPRINT | `RKAN: imprint slot=S energy=E "text"` |
| `recall <k> <probe>` | SYS_RECALL, `k` rankings | `RKAN: recall k=K "probe" n=N` then `RKAN:   winner="text" slot=S score=Q` and the lower ranks |
| `expect <text>` | the last winner must be exactly this | `RKAN: expect ok "text"` or `RKAN: FAIL expect: …` |
| `info` | SYS_FIELD_INFO | `RKAN: info live=L capacity=C` and one line per live slot (energy, effective energy, retrievals, preview) |

The verdict is `rkannaka: SESSION VERIFIED (I imprints, R recalls, E expects ok)`; any `RKAN: FAIL`,
`RKAN: PANIC` or `SESSION FAILED` fails `make ci-smoke` and the Integration job. The shipped session
imprints three memories at importances 900/600/750, recalls a 4-byte-corrupted probe with k=3 and a
2-byte-corrupted one with k=1, expects the exact stored text both times, and lists the region.
The allocator proof and the cross-region refusal from the previous slice run first, every boot.

**The >64-byte decision.** Patterns are 64 bytes and the tool truncates longer text, printing
`RKAN: truncated to 64 bytes: "…"` so the loss is visible. Chunking in ring 3 was rejected: with 8
slots per region, a 65-byte memory would spend a quarter of the region, and recall by resonance
ranks chunks, not records. Longer patterns or more slots are a kernel field change
(`FIELD_PAT_MAX` / `FIELD_SLOTS` feed `FIELD_BLOB_BYTES`, the on-disk blob of ADR-0007) and need
their own ADR when a tool needs them; none of the Kannaka tools in #263 does yet.

**A kernel fact met on the way.** `sys_write` copies at most 128 bytes of a line
(`kernel/src/syscall.c`), so a line carrying two 64-byte patterns is cut by the kernel. `qsys::Line`
now caps at the same 128 and the tool prints a recall as two lines.

| rkannaka, measured on debain2 | result |
|---|---|
| text / data / bss | 20,264 / 480 / 65,568 bytes |
| SSE / x87 instructions | 0 |
| session replay | 3 imprints, 2 recalls (scores 28,6xx and 24,5xx Q15), 2 expects ok, `info live=3` |
| reinforcement, visible in `info` | slot 0 energy 29,490 → 29,694 after its one retrieval |

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
- **Effort.** Slice 1 was one PR (#339). Slices 2 and 3 (the allocator, the `qsys` crate, a
  capability-holding service) were one PR together, because the slice-2 gate already needed a
  granted region. The first Kannaka tool slice is the one whose scope #340 sets.

## Consequences

**Positive.**
- The CI proves, every run, that Rust code runs in ring 3 and obeys the same ABI and the same
  capability gate as C. Every later Rust citizen inherits that proof.
- No kernel change, no new syscall, no new loader path. The contract freeze (ADR-0020) is
  untouched.
- cargo with `--offline` and a committed lockfile that names only the workspace's own crates keeps
  the build deterministic and the dependency surface at `core` + `alloc` + `compiler_builtins`;
  adding an external crate is a visible decision (a lockfile diff) and never a download in CI.
- A kannaka session is data on the initrd, not code: CI replays the same file a person can edit
  on the RAM overlay, and the `expect` verb makes the session its own gate.

**Negative and residual.**
- A second toolchain. `make` now needs stable `rustc` with the bare-metal target; the Makefile
  says how to get it and `install-deps` does it, but a box without Rust no longer builds the
  kernel at all. Making the Rust citizens optional would make the gate optional, which is worse.
- The allocator is libq's 64 KB static arena, per process, with 16-byte alignment: a `Vec` that
  outgrows it gets null, and `alloc` turns that into a panic exit. There is no SYS_BRK; the arena
  is the budget, as it is for C.
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
- PR #343 (#340 slices 2 and 3): `user/rust/Cargo.toml`, `Cargo.lock`, `qsys/`, `rhello/`,
  `rfieldd/`; `Makefile` (the cargo stamp rule, `RUST_SERVICES`, the `RFIELD` gate block);
  `kernel/src/citizens.c` (`user_rfieldd_init`, region 7); `.github/workflows/ci.yml`
  (SHELL_GATES). Gate lines then: `RFIELD: alloc ok: vec grown to 1000 (sum=3496500)`,
  `RFIELD: imprinted 3/3 into region 7`, `RFIELD: winner="a rust citizen remembers the field"
  slot=`, `RFIELD: field_info live=3 capacity=8`, `RFIELD: cross-region denied (EPERM)`,
  `rfieldd: FIELD VERIFIED`.
- The second #340 PR (the first Kannaka tool): `user/rust/rkannaka/` replaces `rfieldd/`,
  `rootfs/etc/kannaka.session`, `citizens.c` (`user_rkannaka_init`, region 7), the `RKAN` gate
  block and SHELL_GATES. Gate lines: `RKAN: alloc ok: vec grown to 1000 (sum=3496500)`,
  `RKAN: cross-region denied (EPERM)`, `RKAN: imprint slot=0 energy=29490 "the swarm remembers
  what the field was told"`, `RKAN: recall k=3 "the swarm remembers what the fjeld was tald" n=3`,
  `RKAN:   winner="the swarm remembers what the field was told" slot=0 score=`, the two
  `RKAN: expect ok` lines, `RKAN: info live=3 capacity=8`, `rkannaka: SESSION VERIFIED (3
  imprints, 2 recalls, 2 expects ok)`.
- Measurements above: debain2, 2026-10-29, QEMU 10.0.11 TCG, `-m 128M`.
