# Embedded Initrd & the Read-Only VFS

Epic #62, phase 2 (issue #64). QuantumOS's first filesystem: a ustar
archive built from `rootfs/` at compile time, embedded in the kernel
image, and served to ring 3 through four syscalls.

## What is in `/bin`

The initrd carries `rootfs/` (today `/etc/motd` and `/etc/kannaka.session`) plus the
programs the Makefile stages into `/bin` (`INITRD_BIN_PROGS`). `ls /bin` in the shell
lists them; `run /bin/<name> [args...]` starts one as a capless citizen.

| program | what it is | start it yourself? |
|---|---|---|
| `hello` | the first program that lived on the filesystem: prints a greeting, exits 42 | yes |
| `args` | echoes its argument vector; proves the argv path (`run /bin/args a b c`) | yes |
| `libqtest` | the libq runtime self-test at -O2 (string, memory, heap, printf) | yes |
| `consciousnessd` | a population of coupled Kuramoto oscillators synchronizing to order; prints the order parameter as it climbs | yes |
| `qtop` | the kannaka-tui essence as a console dashboard: processes, phase field, consciousness gauges | yes |
| `life` | Conway's Game of Life as a native citizen, no floats, capless | yes |
| `rhello` | the first Rust citizen (ADR-0032): `no_std`, built by cargo in `user/rust`, linked like a C citizen | yes |
| `qprobe` | the spawn target of the quota proof: prints one marker and exits 7 | it is for `quota-test`; harmless by hand |
| `agentsub` | the sub-agent `agentd` spawns in the delegation demo; holds only what it is handed | it is for `agentd`; by hand it has nothing to do |

The Rust citizens that run as *services* (`rfieldd`, `rkannaka`) are embedded in the
kernel image like the C services, not placed in `/bin`.

## Why an embedded ustar

The `-kernel` boot path (CI, the qBraid watch window) loads exactly one
file, so the filesystem rides inside the kernel image the same way the
user ELFs do — `objcopy -I binary` into a linkable object with
`_binary_initrd_tar_{start,end}` symbols. ustar because the format is
trivially parseable in freestanding C (512-byte headers, octal sizes)
and the archive is buildable by stock GNU tar with deterministic output:

```
tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime='@0' -C rootfs -cf build/x86_64/initrd.tar .
```

Identical trees produce identical bytes, so the kernel image stays
reproducible.

## Kernel side (`kernel/src/initrd.c`)

No allocation, no mutation, no index: every operation is a bounded walk
of the archive's header blocks (`tar_walk`), with a corrupt-size guard
so a bad header can never carry the walk past the image. Paths are
normalized — `/etc/motd`, `etc/motd`, and `./etc/motd` (the form GNU
tar writes) all name the same entry.

### Public API (`kernel/include/kernel/initrd.h`)

| Function | Purpose |
|---|---|
| `initrd_init()` | Parse the archive at boot, log the inventory |
| `initrd_lookup(path, &data, &size)` | Find a regular file; points into the embedded image |
| `initrd_format_list(path, buf, max)` | "FS: \<name\> \<size\>" console rows for files under path |

## Syscalls (17–20)

| Call | Signature | Behaviour |
|---|---|---|
| `SYS_OPEN` (17) | rdi = path | fd, or ENOENT (-6); EIO if the fd table is full |
| `SYS_READ` (18) | rdi = fd, rsi = buf, rdx = len | sequential read; 0 = EOF; EINVAL on a bad fd |
| `SYS_CLOSE` (19) | rdi = fd | release the slot |
| `SYS_READDIR` (20) | rdi = path, rsi = buf, rdx = len | kernel-formatted listing text |

File state lives in a per-process fd table in the PCB
(`PROCESS_MAX_FDS` = 8), cleared with the PCB — nothing survives exit,
and the data pointers reference the embedded archive, which lives
forever. Paths are copied out of user memory bounded by `VFS_PATH_MAX`;
reads clamp to the caller's mapped user half like every other syscall.

**Capability note:** these calls are uncapped, like `SYS_SYSINFO` — the
initrd is public, read-only data baked into the kernel image, so
reading it names no authority. Per-file capabilities become meaningful
(and required) the day a *writable* filesystem or private files exist;
that is future work, on the record here.

## Shell integration

`qsh` greets with `/etc/motd` read through the VFS — the first file the
OS ever serves to a user — and gains `ls [path]` and `cat <path>`.

## CI gates (in `make ci-smoke` + Integration Test 1h)

From the same piped session as the phase-1 gates:

1. `Welcome to QuantumOS` — motd served through open/read/close at shell start
2. `FS: etc/motd` — `ls` lists the initrd inventory (SYS_READDIR)
3. `The initrd is real` — `cat /docs/hello.txt` streams a file's actual bytes

## The build-system bug this phase found

Phase 2 grew `process_t` (the fd table) — and the Makefile had no
header dependency tracking, so an incremental build left most objects
compiled against the *old* struct layout. Translation units disagreed
about `sizeof(process_t)` and field offsets; the symptoms were wild,
timing-dependent corruption (syscalls failing EINVAL for a running
process, the shell dying mid-command). The fix shipped with this phase:
`-MMD -MP` dependency emission plus `-include $(OBJECTS:.o=.d)`, so a
header edit now rebuilds every translation unit that includes it. CI
always built clean and never saw it; local incremental builds did.

## Known limits / follow-ups

- Read-only; no write layer, no create/unlink.
- Flat lookup (no directory objects, no `..`/`.` resolution — paths are
  matched literally after normalization).
- ustar name field only (100 chars); no pax/GNU long-name extensions.
- Next phase (#65): `SYS_SPAWN` loads ELFs *from this initrd* — the
  shell will run programs off the filesystem.
