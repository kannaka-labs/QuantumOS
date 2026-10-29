# Program Execution: SYS_SPAWN & SYS_WAITPID

Epic #62, phase 3 (issue #65). The closing loop: boot → shell → load a
program **off the filesystem** → run it → collect its exit code. This is
what turns a fixed constellation of build-time services into a system
that runs programs on demand.

## Programs on the filesystem, not in the kernel

The service ELFs (ghostd, paradoxd, …) are embedded in the kernel image
and started at boot. `/bin/hello` is different: it is compiled, then
placed on the **initrd** (`build/x86_64/rootfs-stage/bin/hello`), and the
kernel loads it only when the shell asks. The Makefile stages a copy of
`rootfs/` plus the compiled `/bin` programs into `rootfs-stage/` and tars
that — so the kernel never carries `/bin/hello`'s bytes as a linked
symbol; they live in the archive and are found by path at spawn time.

## Syscalls

### `SYS_SPAWN` (21)

`rdi` = command line (NUL-terminated, user memory): the first
whitespace-separated token names the initrd file to load, and the whole
token list becomes the new process's argv (`argv[0]` = the path). The
kernel looks up the file, loads its ELF into a fresh per-process address
space through the same `user_process_spawn_elf` the boot services use,
and returns the new pid.

**Argument vector.** The kernel splits the command line into at most
`KARGS_MAX` (8) tokens totalling `KARGS_STRBYTES` (480) bytes, packs them
into a `kuser_args_t`, and maps it **read-only** at a fixed VA
(`USER_ARGS_VADDR` = `0x40090000`) in the new address space. A program
reads it with `get_args(argv, max)` from `usys.h`, which returns `argc`
and fills `argv[]` with pointers into the args page. A process spawned
with no arguments (every boot service, the flat-blob demos) sees
`argc == 0`. The layout is duplicated between `usys.h` (reader) and
`syscall.c` (writer) — there is no shared header across the ring
boundary, so the two must stay byte-identical. `/bin/args a b c` echoes
its argv and exits with `argc` as its code.

Spawning is **real authority** — a process that can start programs can
multiply — so unlike the VFS reads, `SYS_SPAWN` is capability-gated:
`CAP_RESOURCE_PROCESS` with `CAP_EXECUTE` over `SPAWN_RESOURCE_ID`,
declaratively granted to `qsh` alone (`grant_spawn`, re-minted on every
start). The capless `ghost_test` proves the denial by attack every boot
(`SPAWN: capless caller denied (EPERM)`). Returns ENOENT for an unknown
path, EIO if the ELF load fails.

### `SYS_WAITPID` (22)

`rdi` = target pid. Returns the exit code (0–255) once the process has
exited, `WAITPID_RUNNING` (256) while it lives, ENOENT for an unknown
pid. Non-blocking — the caller polls, yielding between tries (and
heartbeating, so a watchdog-monitored shell isn't killed mid-wait).

**The exit ledger.** The idle-loop reaper `memset`s a terminated PCB
long before a shell gets around to polling it, so the exit code can't
live only in the PCB. `sys_exit` records `(pid, code)` in a small kernel
ring (`EXIT_LEDGER_SIZE` = 16); `SYS_WAITPID` consults the ledger
newest-first (so a recycled pid still resolves to the exit the waiter is
after), then the live table. Single CPU, syscalls run interrupts-off, so
no locking is needed.

## Shell integration

`qsh` gains `run <path>`: spawn the program, print its pid, poll to
completion, and report the exit code. `/bin/hello` prints a greeting and
exits 42; `run /bin/hello` reports `qsh: pid N exited (code 42)`.

## CI gates (in `make ci-smoke` + Integration Test 1h)

The piped session ends with `run /bin/hello` before `exit`:

1. `HELLO: greetings from /bin/hello` — the program ran, loaded off the initrd
2. `exited (code 42)` — the shell collected the exit code (SYS_WAITPID)
3. `SPAWN: capless caller denied (EPERM)` — the capability gate, by attack

## Rust citizens (#263 slice 1, ADR-0032)

A program on the initrd does not have to be C. `user/rust/rhello` is a
`no_std` Rust program: the Makefile builds the `user/rust` cargo workspace
for `x86_64-unknown-none` (soft-float, no SSE, no red zone — the C
citizens' flags in Rust's terms; `--offline`, a committed `Cargo.lock`, no
external dependencies) into one static archive per citizen, then links each
with the same gcc command and `user.ld` as every C citizen, plus
`--gc-sections` (without it the panic paths behind each slice index pull
in all of `core::fmt`: 179 KB of text instead of 9.6 KB, measured). The
loader sees an ordinary ET_EXEC at `USER_VBASE`.

`user/rust/qsys` is the syscall layer, the Rust twin of `usys.h`: the same
`int 0x80` convention, the same numbers, the full wrapper surface (process,
console, files, spawn/waitpid, audit, manifest, the field, cap_derive), and
`#[repr(C)]` request and response structs whose sizes are pinned at compile
time (76, 136, 40, 332 and 24 bytes), so a layout drift fails the build on
both sides. `qsys::heap::LibqAlloc` is a `GlobalAlloc` over libq's own
arena (`user/libq/heap.c`, linked into every Rust citizen; only one that
declares the allocator pulls the 64 KB arena in), so `Vec` and `String`
work; `alloc::format!` stays out, for the same `core::fmt` reason.

Rust citizens can also be *services*: `rkannaka` (`user/rust/rkannaka`,
#340), the Rust kannaka client of the kernel field, is embedded in the
kernel like `kannakad` and started by `citizens.c` with a declarative
`CAP_RESOURCE_FIELD` grant over region 7. At boot it proves the allocator
(`RKAN: alloc ok`: a `Vec` grown to 1,000, shrunk, regrown and dropped
through the libq arena) and the gate (`RKAN: cross-region denied (EPERM)`:
region 1 is kannakad's, and the grant names one region), then replays
`/etc/kannaka.session` from the initrd (the RAM overlay shadows it, so a
session can be replaced at runtime): `imprint <importance> <text>`,
`recall <k> <probe>`, `expect <text>` (the last winner must be exactly
this) and `info`. Each verb prints an `RKAN:` line; the shipped session's
corrupted probes recall the exact stored text, and the verdict is
`rkannaka: SESSION VERIFIED (3 imprints, 2 recalls, 2 expects ok)`. Text
longer than 64 bytes is truncated and said so (ADR-0032 records why).

The session runs `run /bin/rhello` and gates on four lines:

1. `RHELLO: rust no_std citizen on QuantumOS pid=` — it runs at all
   (toolchain, link, ELF loader)
2. `RHELLO: SYS_AUDIT stats copied N bytes into a Rust buffer` — the ABI,
   kernel-to-user pointer direction
3. `RHELLO: SYS_RECALL capless -> -4 (EPERM)` — the ABI, user-to-kernel
   twin struct, and the capability gate: a spawned program holds no field
   capability, and the refusal is exactly the one a C caller gets
4. `exited (code 7)` — it ended on purpose; a panic prints `RHELLO: PANIC`
   and exits 99, and any `RHELLO: FAIL`/`PANIC` line fails the gate

Building needs a stable Rust toolchain (`rustc` + `cargo`) with the
bare-metal target (`rustup target add x86_64-unknown-none`;
`make install-deps` does it).

## Watchdog hardening (bundled from the phase-1 adversarial review)

A multi-agent adversarial review of the phase-1 shell surfaced three
kernel-framework defects that `SYS_SPAWN` makes more reachable (user-driven
spawning recycles pids at runtime). Fixed here:

- **`start_slot` grant ordering** — the reborn service is now spawned,
  granted its caps, and has its pid recorded all under one `cli` window,
  so the scheduler can never run a watchdog-reborn service before its
  console/quantum/spawn caps and `slot->info.pid` exist. Previously a
  timer preemption in that window could schedule a capless shell (console
  EPERM → suicide) or leak caps to a half-built, recyclable pid.
- **Recycled-pid teardown** — a per-slot generation counter
  (`process_get_generation`) is recorded at spawn and checked in
  `service_stop`, so the ~2 s-late watchdog can no longer `process_destroy`
  an innocent process that recycled the freed slot. This is the defect
  `SYS_SPAWN` most directly worsens.
- **Bounded console TX** — `console_write` caps its interrupts-off wait
  for the UART transmit register, so a wedged host-side serial reader
  drops a byte instead of freezing the whole kernel.

## Known limits / follow-ups

- argv is delivered (above); no environment strings yet, and no `fork` —
  spawn always loads a fresh ELF (which suits a no-shared-memory
  microkernel). argv caps at 8 tokens / 480 bytes.
- Exit ledger is 16 deep; a program whose exit is never waited on ages
  out of the ledger (harmless — `SYS_WAITPID` then returns ENOENT once
  the PCB is also reaped).
- `run` is foreground-only; no job control or `&`.
