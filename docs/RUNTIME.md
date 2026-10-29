# Ring-3 programs: the syscall ABI and the libq runtime

This is the contract for writing a user program that runs on QuantumOS —
the stable syscall interface every ring-3 process sees, and `libq`, the
freestanding C runtime (heap + libc-lite + printf) that programs link
against. Native app "citizens" are built on this; it is also the substrate
a future real-binary port (e.g. a Rust `no_std` target) would target.

## The program model

A user program is a freestanding, statically-linked ELF loaded at a fixed
address. It has no libc and no CRT: it defines its own entry point
`void _start(void)` (the linker script's `ENTRY(_start)`), does its work
through syscalls, and ends by calling `exit_(code)`. There is no return to
a caller — `_start` never returns (a trailing `for (;;) {}` after `exit_`
keeps the compiler happy).

Two kinds of program exist:

- **Kernel-embedded services** (init, ghostd, paradoxd, swarm_svc, qsh, …):
  linked into the kernel image and started at boot with capabilities.
- **Filesystem programs** (`/bin/hello`, `/bin/args`, `/bin/libqtest`):
  ride the initrd and are started from the shell with `run /bin/<name>`
  through `SYS_SPAWN`. Their exit code comes back to the shell via
  `SYS_WAITPID`. A spawned program reads its argument vector with
  `get_args()`.

To add a filesystem program: drop `user/<name>.c` in, add `<name>` to
`USER_PROGS` (and to `INITRD_BIN_PROGS` for it to appear under `/bin`) in
the Makefile. The build compiles it with the `USER_CFLAGS` (freestanding,
`-O2`, `-Werror`) and links `libq.a` after it automatically.

## Memory layout

Each process gets a private address space in `[0x40000000, 0x80000000)`:

| region      | address                     | notes                          |
|-------------|-----------------------------|--------------------------------|
| image       | `0x40000000` (`USER_VBASE`) | text, rodata, data, bss        |
| args page   | `0x40090000`                | read-only packed argv          |
| user stack  | `0x40100000` down, 16 KiB   | `USER_STACK_TOP`, 4 pages      |

The ELF loader maps each `PT_LOAD` segment's full `p_memsz` (bss zeroed),
so a static bss array — like the libq heap arena — is backed at exec. The
loader does **no** overlap check, so the total image (including the heap
arena) must fit below the args page. `user/user.ld` enforces this with a
mandatory `ASSERT(_end <= 0x40080000)`, leaving ≥64 KiB of margin.

The args page is populated by copying the whole kernel-side `kuser_args_t`
into it, and that struct is a **reused static** in `sys_spawn`. Because
`SYS_SPAWN` copies the entire `sizeof(struct)` — argv-offset table plus the
480-byte string pool — a child could otherwise read, past its own `argc`, the
high-water-mark residue of a *prior* spawn's command line (a cross-process
disclosure of kernel `.bss`). `parse_cmdline` therefore zeroes the whole struct
before its partial field-by-field fill, so only the current spawn's argv is
ever visible. The boot self-test `spawn_argv_leak_selftest` parses a
secret-bearing long line then a short one and asserts no residue survives,
gating on `ARGVLEAK: reused spawn buffer carries no stale argv residue`.

## The syscall ABI

Syscalls use `int 0x80`: the number in `rax`, up to three arguments in
`rdi`, `rsi`, `rdx`, and the result in `rax`. `user/usys.h` provides the
raw wrappers (`usys0`..`usys3`) and typed helpers over them; it is the
canonical, stable definition. The number space (frozen; new syscalls
append):

| #  | name           | helper                    | purpose                          |
|----|----------------|---------------------------|----------------------------------|
| 1  | SYS_WRITE      | `write_str`               | write a framed console line      |
| 2  | SYS_GETPID     | `getpid`                  | caller pid                       |
| 3  | SYS_YIELD      | `yield`                   | cooperative reschedule           |
| 4  | SYS_EXIT       | `exit_`                   | terminate with a code            |
| 5  | SYS_TICKS      | `ticks`                   | monotonic tick count             |
| 6–7| SYS_SEND/RECV  | `send_msg`/`recv_msg`     | capability-routed IPC            |
| 8–9| SYS_HEARTBEAT… | `heartbeat`/`svc_restarts`| watchdog liveness / rebirth      |
| 10 | SYS_QRAND      | `qrand_fill`              | quantum-seeded randomness (cap)  |
| 11 | SYS_SEND_TO    | `send_to`                 | targeted capability IPC          |
| 12 | SYS_COM2       | `com2_read/write_bytes`   | swarm-bridge UART (cap)          |
| 13 | SYS_QSEED      | `qseed_value`             | boot qseed provenance (cap)      |
| 14 | SYS_FIELD_SNAP | `field_snapshot`          | framebuffer field viz            |
| 15 | SYS_CONS       | `cons_read/write`         | raw console I/O (cap)            |
| 16 | SYS_SYSINFO    | `sysinfo`/`sysinfo_quiet` | ps/mem/time/quiet/peer introspection |
| 17–20 | SYS_OPEN…READDIR | `open_`/`read_`/`close_`/`readdir_` | initrd + RAM overlay read |
| 21–22 | SYS_SPAWN/WAITPID | `spawn_`/`waitpid_`  | exec a /bin ELF, reap it         |
| 23–25 | SYS_FWRITE/UNLINK/SYNC | `fwrite_`/`unlink_`/`sync_` | RAM overlay writes + disk persist (cap) |
| 26 | SYS_RESOLVE    | `resolve_`                | DNS hostname → ip (cap)          |
| 27 | SYS_UDP        | `udp_`                    | ring-3 UDP sockets (cap)         |
| 28 | SYS_TCP        | `tcp_`                    | ring-3 TCP client (cap)          |
| 29 | SYS_IMPRINT    | `imprint_`                | store into a kernel field region (cap) |
| 30 | SYS_RECALL     | `recall_`                 | ranked associative recall (cap)  |
| 31 | SYS_FIELD_INFO | `field_info_`             | read-only field enumeration (cap) |
| 32 | SYS_AUDIT      | `audit_`                  | read the capability authority ledger (uncapped RO) |
| 33 | SYS_MANIFEST   | `manifest_`               | read the per-pid intent manifests (uncapped RO) |
| 34 | SYS_CAP_DERIVE | `cap_derive_`             | delegate a narrowed capability to a sub-agent (cap) |
| 35 | SYS_QPU        | `qpu_`                    | submit/poll an opaque quantum circuit to the kernel QPU broker (cap+quota) |

**Cross-ring capability delegation (epic #137).** `SYS_CAP_DERIVE` lets a
citizen holding `CAP_GRANT` hand a strictly-**narrowed** slice of one of
its own capabilities to a sub-agent — "an agent hands a narrowed intent to
a sub-agent." The caller names its parent capability by
`(resource_type, resource_id)` (never a ring-3 handle, so no handle
forgery) and the narrowed permission subset; `CAP_GRANT`/`CAP_REVOKE` are
refused in the handed permissions, so delegation is provably **one-hop**
(the sub-agent cannot itself re-delegate). The kernel bounds the derive by
AND reflects it in the intent manifest (epic #135): the caller may only
delegate a resource it is itself declared to touch (a transitive outer
bound, `manifest_check` on the delegator), and the derive **extends the
recipient's manifest** (`manifest_grant`) so the delegated cap is actually
usable — a delegated cap whose resource is absent from the recipient's
manifest would otherwise be denied. The target must be an IPC peer of the
caller (so a `CAP_GRANT` holder can inject a cap only into a process it was
explicitly wired to), a live ring-3, non-self, manifest-bound, non-monitored
process. The op is idempotent (a covering cap already held → success without
minting). Delegation narrows PERMISSIONS and EXPIRY, not the resource
(region granularity is the unit). When the delegator dies, cascade
revocation kills the derived cap — the un-fakeable proof that authority was
*delegated* (a static grant survives an unrelated process's death). Proven
every boot by the `delegation-test` → `subagentd` demo.

**The intent manifest + spawn quota (epic #135).** Above raw
capabilities sits a per-pid **intent manifest**: the allow-set of
`{resource_type, resource_id}` a citizen was *declared* to touch, built
from the same grant flags that mint its caps and bound in the same
interrupt-off window (`manifest_bind`), checked after `cap_find` at every
capability-gated syscall (`manifest_check` → `AUDIT_MDENY` on a held cap
that exceeds declared intent). The manifest also carries the **first
enforced quota**: `spawn_max` limits successful `SYS_SPAWN`s per
incarnation (checked before any spawn side effect, charged only on
success → `AUDIT_QUOTA` on refusal). A per-pid **`cpu_limit`** (epic
#144) is the second enforced quota: `cpu_ticks` counts timer ticks the
process is scheduled in, and once it exceeds a declared `cpu_limit` the
kernel **terminates** the process from the timer tick — a busy-spin
runaway that ignores cooperative scheduling cannot hog the machine. The
kill is guarded by `(cs & 3)` so a process caught mid-syscall in ring 0 is
skipped and caught the next tick it is back in ring 3 (the cumulative-tick
condition is a latch); it records an `AUDIT_CPUKILL` ledger entry and the
pid vanishes from the process/manifest tables. Enforcement is **opt-in**
(`cpu_limit == 0` = unlimited), so every shipped long-runner (qsh, ghostd,
…) is untouched — only a citizen that declares a finite budget is
eligible (proven every boot by the `cpu-hog` citizen). On the
shipped system caps and manifests are minted from the same grants, so the
intent check refuses nothing today — its value is inspectability
(`SYS_MANIFEST`) and the outer bound capability delegation will be checked
against; the spawn quota is the non-vacuous enforcement now (proven every
boot by the `quota-test` citizen). A boot self-test drives the deny path
live so it cannot ship as dead code. IPC caps stay OUTSIDE the manifest
(pair-wise runtime wiring, not declarative intent). The ledger records
the full **revocation lifecycle** with honest kinds: `AUDIT_REVOKE` for
an explicit `cap_revoke` (+ its cascade), `AUDIT_REAP` for slots freed
because their owner died (the reaper's cleanup + cascade), and
`AUDIT_UNLINK` for a **spawn-channel** cap freed under its *surviving*
owner because the channel's TARGET died (epic #175 — never conflated
with REAP, which must keep meaning "owner died"; the dead pid rides in
`resource_id`) — a verifier never mistakes garbage collection for
exercised revocation authority. Each cascade-freed delegated cap records
the *recipient's* pid (the holdings that changed), proven every boot by
the delegation demo: the exited delegator's own FIELD cap REAPs under
its pid, and subagentd's derived READ cap REAPs under subagentd's.

**Spawn-time parent↔child IPC channels (epic #175).** A citizen holding
the **`grant_spawn_channel`** opt-in (alongside `grant_spawn`) gets a
bidirectional capability-checked IPC pair minted at every `SYS_SPAWN` —
the pipe-at-fork analogue, satisfying `SYS_CAP_DERIVE`'s IPC-peer
requirement for processes it creates. The mint is all-or-nothing (cap
capacity is prechecked before the child exists, so a returned pid always
carries a fully wired channel) and adds **no manifest rows** (IPC stays
outside the manifest). The pair is tagged at mint; when either end dies,
`process_destroy` frees the survivor's half **before the pid slot is
reusable** (pids recycle first-fit — an untagged leftover would hand the
parent a live channel into an unrelated future process), recording
`AUDIT_UNLINK`. The teardown is scoped by tag AND resource type, so
kernel hand-minted pairs survive a monitored peer's watchdog restart; a
boot self-test (`CAPUNLINK`, ci-smoke-gated) asserts the dead-target cap
is freed while a live-target tagged cap, a colliding-`resource_id`
non-IPC cap, and an untagged pair all survive. The opt-in is deliberately
NOT set for qsh: untargeted `send_msg` routes first-match over the
sender's IPC caps, so blanket minting would break any spawner relying on
a singleton IPC cap (qsh's `ghost` builtin) and would un-capless every
`run` child. Only `agentd` holds it.

**qsv — exact integer quantum computation (epic #148, quantum-stack
Phase 1).** A ring-3 citizen holding a 12-qubit state vector whose
amplitudes are exact Gaussian integers (int32 re/im) under ONE implicit
global scale `2^(-h/2)`, `h` = running Hadamard count. H on a pair is
`(a,b) → (a+b, a−b)` — pure integer, zero rounding *ever* (no rounded
binary fixed point can be exact: no nontrivial 2×2 real unitary has
dyadic entries — so exactness comes from not normalizing at all).
X/Z/CNOT/CZ are permutations/sign flips; S multiplies the |1⟩ half by i;
T is excluded from v1 (irrational over Z[i]). Probabilities are exact
rationals `|v|²/2^h`, and unitarity is an integer identity checked in
`unsigned __int128`: `Σ|v|² == 2^h` exactly. Every boot it runs Bell,
GHZ, Grover-3q (2 iters → p = **121/128 exactly**) and Grover-4q
(3 iters → amp −16064 at h=28, p = **63001/65536 exactly**), then prints
a sha256 of the final state that CI re-derives with an independent host
Python mirror (`scripts/qsv_mirror.py`) — two implementations must agree
bit-for-bit, and a deliberately corrupted mirror circuit must NOT match.
No capabilities at all (its authority is the null set). This is the
native tier of the quantum stack (#148–#150): air-gapped, boot-verifiable
quantum computation; simulation beyond 12 qubits and real QPUs arrive via
the capability-gated host gateway (Phases 2–3).

**The QPU job broker (epic #148, quantum-stack A2+A3).** `SYS_QPU` makes
circuit submission a capability-gated, quota-bounded, ledger-recorded
authority — the kernel is a **broker**, never a circuit parser. Payloads
are OPAQUE bytes; the kernel enforces WHO (a `CAP_RESOURCE_DEVICE`
capability over `DEVICE_ID_QPU`, checked through the two-layer
`authorize()` gate), HOW MUCH (a manifest `qsub` quota mirroring
`spawn_max`, plus a per-owner in-flight cap so a lifetime counter cannot
be gamed to pin the pool), and the job-slot lifecycle; the quantum
computation runs in ring 3 (`qpud`, the executor, on the exact integer
engine shared with `qsv`). Authority is a **two-perm partition**:
`CAP_WRITE` submits and polls, `CAP_READ|CAP_EXECUTE` fetches and
completes — minted disjoint (a submitter that could complete would forge
its own results; an executor that could submit would spend quota it does
not hold). The four ops are `SUBMIT` (→ a job id; charges quota, records
`AUDIT_QSUBMIT`), `FETCH` (executor; stamps its `(pid, generation)` for
forgery defense and death-reclamation), `COMPLETE` (executor; the caller
must BE the fetcher), and `POLL` (owner; DONE copies the bounded result
out and frees the slot). Job slots are a per-process kernel resource like
fds and caps, so a `process_destroy` sweep frees a dead owner's jobs and
fails a dead executor's `RUNNING` jobs closed to `EXECFAIL` — no
`RUNNING`-forever leak, no permanent `EAGAIN`. `qpu-test` proves the whole
path every boot un-echoably: it submits opaque Bell and Grover-3q circuits
and checks the EXACT integer results `qpud` returned through the broker
(`p=1/2`, `p=121/128`) — a submitter that does not include the engine —
then proves the quota (a third submit refused) and both cross-perm
denials. Result lines name the backend that ran the circuit
(`QPU_BACKEND_LABEL` in `user/qpu_circuit.h`; today "QSIM (in-guest simulator, not a
QPU)"). `ghost_test` proves a capless submit is refused. Later backends
(simulation past 12 qubits, real QPUs — Phases 2–3) are dispatched from
`qpud` to the host gateway; the kernel surface is unchanged. That host
gateway (`scripts/qsv_gateway.py`) terminates the SAME opaque circuit wire
format and dispatches to PennyLane `lightning.qubit` (cuQuantum on
`lightning.gpu`, real QPUs via qBraid/Braket later); a hermetic CI
cross-oracle (`scripts/test_qsv_oracle.py`) asserts the exact integer
engine and PennyLane agree to 1e-9 on identical circuits — two independent
implementations, so neither can fake the other. A host agent can also reach
the *in-OS* broker over the attested COM2 bridge: `swarm_svc` gains a
`SWARM_OP_QSUBMIT` frame that forwards a host-framed circuit to `SYS_QPU`
and returns the exact result — the submit is non-blocking (the job is
polled one step per main-loop pass so a slow circuit can never starve the
watchdog and kill the sole COM2 bridge). The `ci-smoke-qsubmit` gate proves
Bell and Grover-3q come back over the wire the host never typed,
cross-checked against the PennyLane gateway, with the qsub quota enforced.
The gateway dispatches the SAME opaque circuit to any backend by an opaque
device string (Phase 3, `#150`): `lightning.gpu` (NVIDIA cuQuantum —
verified locally on a GTX 1650 at 2²² amplitudes, far beyond the native
12-qubit engine, agreeing with the CPU reference to machine precision);
`cudaq` (NVIDIA CUDA-Q, a third independent engine, cross-checked against
the exact integer engine on its free local CPU target); and
`qbraid:<machine>` for a real QPU (Rigetti/IonQ/IQM/QuEra — credentialed
and metered, so a maintainer-invoked `workflow_dispatch`, never an
autonomous gate). The in-OS `AUDIT_QSUBMIT` ledger entry lets an auditor
reconcile a real-QPU submission's upstream job id against the OS-side
authority record — who submitted, under what grant and quota. Every backend
reports probabilities in the exact engine's **little-endian** basis order
(qubit 0 = LSB); the cross-oracle tests include asymmetric circuits, so a
big-endian readout (which symmetric Bell/GHZ hide) is caught, not trusted.
The executor rejects a circuit whose amplitudes overflow int32 rather than
report a wrapped value, and the broker's process-death sweep runs cli'd so a
watchdog teardown can never race a concurrent submit.

**The kernel holographic field (epic #95).** `SYS_IMPRINT` stores a
byte pattern (≤ 64 bytes, with a Q15 importance) into one of the
kernel's fixed field REGIONS; `SYS_RECALL` scores every live slot in a
region against a probe — Q15 cosine of centered unit wavefronts times
energy, one bounded integer pass, no floats — and returns the top-K
`(slot, score)` rankings plus the winner's stored bytes: a ~15%-
corrupted probe recovers the exact original. Both calls take request
structs (`field_imprint_req_t` / `field_recall_req_t` +
`field_recall_out_t` in `usys.h`; layouts `_Static_assert`-pinned
against the kernel twins) whose `region` field must be named EXACTLY by
the caller's `CAP_RESOURCE_FIELD` capability — granted declaratively to
services (`grant_field` + `field_region`, scrubbed and re-minted on
every restart so a reborn or successor service never inherits its
predecessor's memories), never to spawned `/bin` programs. A capless
caller is denied `-4` before the kernel reads its request; a
wrong-region request by a cap holder is also `-4` (that comparison IS
the isolation boundary). Recall's retrieval reinforcement (an energy
boost on the winner) only happens when the caller also holds the write
right — a read-only capability never mutates. A degenerate
all-equal-bytes probe returns `n=0` (success); as a pattern it is `-1`
EINVAL. This is the RANKED holographic memory kind; `ghostd`'s
iterative attractor field is a deliberately different, living memory
that stays in ring 3.

**Conventions.** A non-negative return is success (often a count or fd);
a negative return is an errno: `-4` EPERM (no capability authorises it),
`-6` ENOENT, `-5` EIO, `-1` EINVAL, `-2` EFAULT. Non-blocking calls
(`SYS_RESOLVE`, `SYS_UDP`, `SYS_TCP`) return `-11` (WOULDBLOCK) while
pending — poll again after a `yield`. Privileged operations require a
capability the process was granted at spawn; a program without it gets
EPERM rather than the resource. `SYS_WRITE` frames each call as
`[user pid=N] <text>\r\n` and truncates at 127 bytes.

## The libq runtime (`user/libq/`)

`#include "libq/libq.h"` — the only include a program needs; it also pulls
in `usys.h`. `libq.a` is linked automatically. The linker pulls only the
archive members a program references, so a program that never allocates
gets neither the heap arena nor printf in its image.

- **mem** (`mem.c`): `memcpy`, `memmove`, `memset`, `memcmp`. These are
  the functions gcc itself emits calls to for struct/array copies, so they
  must exist as real symbols. `mem.c` and `str.c` are compiled with
  `-fno-tree-loop-distribute-patterns -fno-builtin` so `-O2` cannot rewrite
  a copy/fill loop into a call to the function being defined (a silent
  infinite recursion); a pre-boot objdump gate enforces this.
- **str** (`str.c`): `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`,
  `strchr`.
- **heap** (`heap.c`): `malloc`, `free`, `calloc`, `realloc` over a 64 KiB
  per-process bss arena (override with `-DLIBQ_HEAP_SIZE`). First-fit
  explicit free list with boundary tags and immediate bidirectional
  coalescing; every payload is 16-byte aligned; `free`/`realloc` gate the
  pointer (bounds, alignment, size/footer agreement) so a stray or
  double-freed pointer fails safe. No `SYS_BRK` yet — the arena is fixed;
  the growable-heap path is deferred but the API is stable across the swap.
- **printf** (`printf.c`): `printf`, `vprintf`, `snprintf`, `vsnprintf`.
  Integer-only (`%d %i %u %x %X %s %c %p %%`, the `0` flag, numeric width,
  `l`/`ll`) — no floating point, because the user ABI is `-mno-sse` and a
  float conversion would drag in soft-float. `snprintf`/`vsnprintf` are
  pure and follow C99 truncation. `printf`/`vprintf` format into a 128-byte
  stack buffer and hand the line to `SYS_WRITE`, so printf is
  **line-oriented** (one framed line per call, ~127-char cap), not
  byte-accurate stream output.
- **fx** (`fx.c`): fixed-point math — `fx_sin`/`fx_cos` (phase is a uint32
  "turn", the full circle is 2^32; return Q15, a value `v` meaning `v/32768`)
  and `fx_isqrt` (integer floor sqrt). QuantumOS saves no FPU/SSE state across
  a context switch, so a preemptible ring-3 program must not use float/double;
  citizens do their trig and roots through `fx`, exactly as `ghostd` does.

`user/libqtest.c` (`/bin/libqtest`) exercises the whole runtime at `-O2` in
ring 3 and prints `LIBQ: self-test OK`; the ci-smoke and integration boot
gates assert it, so a regression — including a reintroduced self-recursion
trap — turns CI red.

## Native app citizens

Native C "citizens" embody the constellation's apps on top of libq (the
`ghostd`/`paradoxd` pattern, now with a heap and printf). The first is
`user/consciousnessd.c` (`/bin/consciousnessd`) — the essence of
`consciousness-core`: a field of coupled Kuramoto phase oscillators
synchronizes, the order parameter `r` climbs from incoherent toward locked,
and `r` maps to a consciousness verdict (Dormant → Transcendent). It runs
entirely in `fx` fixed-point; the mean-field update `K·r·sin(ψ−θ)` expands to
`(K/N)·[Σsin·cosθ − Σcos·sinθ]`, so the `r` and mean-phase `atan2` cancel out
of the drive and are needed only to report `r`. A ci-smoke gate asserts the
field actually synchronizes (`r` past 0.8 → `CONSCIOUSNESS EMERGED`), not just
that the program ran.

`user/kannakad.c` is the essence of `kannaka-memory` — and since epic #95
phase 2, a **kernel-embedded service and the kernel field's reference
client**. Its holographic ranked-recall math (one resonance pass scoring
every stored memory by `similarity × energy` — not a search) was promoted
INTO the kernel as `SYS_IMPRINT`/`SYS_RECALL`; kannakad now exercises that
surface rather than carrying a private store. Declared with
`grant_field = 1, field_region = 1`, so the service framework scrubs and
re-mints its region-1 `CAP_RESOURCE_FIELD` cap on every start. At boot it
imprints seven importance-weighted seeds, recalls a byte-corrupted probe
back to the **exact stored text**, and proves retrieval reinforcement
raised the winner's score through the syscall's write-side contract (the
old in-process dream/hallucination pass died with the private store —
consolidation belongs to the kernel field's future). Where `ghostd` is a
single-basin binary attractor that relaxes onto one pattern, the kernel
field kannakad speaks to is the ranked, importance-weighted cousin. A
ci-smoke gate asserts all three behaviours (`RESONANCE VERIFIED`).

`user/rust/rkannaka` is its Rust sibling and the first Kannaka *tool* inside
the guest (#340, ADR-0032): the same kind of service, declared with
`field_region = 7`, allocating over libq's arena, that replays
`/etc/kannaka.session` from the initrd — kannaka-memory's verbs (`imprint`
with importance, `recall` with top-k, `expect`, `info`) as data a person can
edit on the RAM overlay and CI replays as a gate (`SESSION VERIFIED`).

`user/agentd.c` (the orchestrator) + its capless sub-agent `user/agentsub.c` are
the **agent-native society demo** — one ring-3 citizen exercising the whole
agentic stack in a single story, each step with a verifiable outcome (agentd
carries no engine, so a correct result can only have come back through the
kernel's brokers): it **submits a Bell circuit** through the `SYS_QPU` broker and
checks `qpud`'s exact `p(00) = 1/2`; **imprints and recalls** a phrase in
holographic field region 3 from a deliberately corrupted probe; **spawns**
`/bin/hello` and waits for it to exit (real spawn authority); and **delegates** a
strictly-narrowed READ slice of its region-3 field cap to a **society of
sub-agents** — `AGENT_SUBS` (3) instances of `agentsub`, each addressed by the
kernel-vouched sender pid of its "ready" and each handed its own derived cap via
`SYS_CAP_DERIVE` over a capability-checked IPC pair (a one-hop fan-out tree —
`CAP_GRANT` is never delegated, so no sub can re-delegate). Every sub-agent
recalls the phrase from its **own pid-salted noisy probe** (each society member
reconstructs from different noise), verifies the exact bytes, confirms WRITE was
withheld, and acks the **FNV-1a digest of what it actually recalled**; the
orchestrator independently digests the phrase and requires a matching,
digest-bearing ack from **each distinct delegate** (deduped by the
kernel-vouched sender pid — N acks from one sub are not a society). On all four
it prints `AGENTD: DEMO OK qpu+field+spawn+society` plus
`AGENT: society consensus 3/3 …`; ci-smoke gates assert both. Being a
heavyweight showcase, it is **opt-in** via the `agentdemo` cmdline token (off by
default) so its extra boot work never perturbs the timing-sensitive proofs the
other CI gates poll for. The default ci-smoke boot passes `agentdemo` (and gates
it), and the GRUB ISO's **"QuantumOS (agent-native demo)"** entry boots
`quiet agentdemo` — the token is deliberately decoupled from `quiet`, so the
showcase runs on a clean console. The society is **self-assembled** (epic
#175): `user_agent_demo_init()` registers ONE citizen — the orchestrator —
and agentd spawns its own `AGENT_SUBS` sub-agents from `/bin/agentsub` on the
initrd. Each spawn mints the bidirectional parent↔child IPC pair (agentd
holds `grant_spawn_channel`), so the roster is the spawn returns, "ready"
acks are matched against it, and `SYS_CAP_DERIVE`'s peer requirement is
satisfied by a channel the orchestrator itself created — no kernel
hand-wiring anywhere in the story.

The society practices **division of labor** (epic #177): field regions 4–6
are the three specialists' PRIVATE workspaces (`FIELD_REGION_COUNT` is 8; 7
is spare). agentd holds a **`field_region_span`** grant — a service
definition may now grant a REGION RANGE `[field_region, field_region+span)`,
each region getting its own cap + manifest row + scrub-at-mint
(`field_inherit` consulted per-region); `start_slot` refuses an
out-of-bounds span whole and drops manifest rows fail-closed past
`MANIFEST_MAX_ENTRIES` (a span makes the row count def-dependent). Per
specialist, agentd derives TWO narrowed slices over the spawn-minted
channel: shared region 3 READ-only, and workspace 4+i READ|WRITE (never
CAP_GRANT). The `"g<digit>"` go message names the workspace. Each
specialist recalls the shared phrase from its own noisy probe, proves the
shared slice still refuses writes (exactly EPERM), **publishes a distinct
recomputable result** (`w<8-hex FNV-1a(phrase‖digit)>`) in its workspace,
proves the NEXT sibling workspace (4→5→6→4) refuses its write (exactly
EPERM, sibling asserted before probing), and acks its consensus digest.
The orchestrator then verifies each workspace holds EXACTLY one live slot
(`SYS_FIELD_INFO`) and recalls the exact recomputed result — inside the
same step, so `AGENTD: DEMO OK` stays a conjunction over every proof; the
`AGENT: division of labor 3/3` line is ci-smoke-gated.

On disk, the audit-ledger blob home is **frozen** at its legacy absolute
position (`AUDIT_HOME_TOP_OFFSET` = 27 sectors from the top,
static-asserted) and the field blob now lives BELOW it inside
`FIELD_HOME_RESERVE_SECTORS` (16, static-asserted ≥ `FIELD_BLOB_SECTORS`):
growing the field geometry (as epic #177's 4→8 region bump did) can never
again relocate the format-unchanged authority ledger's home — existing
disks keep their durable ledger across the upgrade, and only the field
section honestly cold-starts at the superblock geometry guard.

**Society of societies (epic #178).** In a coupled boot each kernel runs its
OWN complete agent society, and the two societies **exchange results**: after
the division-of-labor verification, the orchestrator computes its society's
AGGREGATE (FNV-1a over the three specialist results, salted with FNV-1a over
the boot qseed's bytes so two members' aggregates provably differ), prints it
inside the `DEMO OK` conjunction, and hands it to `fieldsyncd` over an IPC
pair minted only in `agentdemo` boots (agentd discovers the pid via the
uncapped `SYSINFO_PS` text — the qtop pattern). `fieldsyncd` broadcasts it to
every configured peer as a fixed 16-byte **FSYP** frame riding the same ~1 Hz
cadence and peer-source validation as the phase frames — a **continuous
idempotent resend**, so a peer that boots later or loses datagrams still
converges — and prints each DISTINCT received value once:
`FIELDSYNC: peer aggregate a<hex>`. Received aggregates are **never imprinted
into the field**: the design review rejected field-content replication
(unauthenticated wire data must not become recallable or persistable field
content); the printed line is verified HOST-side by the
`test_qos_society_agents.py` gate, which recomputes both expected values from
the compile-time scheme + each boot's qseed and asserts cross-appearance on
both consoles (`ci-smoke-society-agents`). The same change fixed a latent
`fieldsyncd` demux bug: the receive drain treated any datagram under 260
bytes as WOULDBLOCK and stalled, so a short frame both vanished and ended the
drain — dispatch is now magic-first with per-type size checks.

`user/quantumd.c` is the essence of `kannaka-quantum`, and — unlike the two
above — a **kernel-embedded service**, not a `/bin` program. `SYS_QRAND` and
`SYS_QSEED` are capability-gated (a capless `/bin` caller gets EPERM by
design), so quantumd is declared with `grant_quantum_pool = 1` in
`user_quantum_demo_init()`; the service framework mints its quantum-pool read
cap on every start. It draws **real collapse-derived entropy** — a quantum
coin, a rejection-sampled die, a qrng readout — and performs recall as
**amplitude amplification**: candidate resonance scores are amplitude-encoded,
Grover-amplified in closed form (`P_target = sin²((2m+1)·asin(√p))`, via
`fx_asin`/`fx_sin`), then sampled from the pool; the amplified argmax must
agree with the classical argmax (`QUANTUM VERIFIED`). It refuses to fall back
to a software PRNG — if the pool is denied it fails loud, honouring
kannaka-quantum's entropy discipline.

`user/qtop.c` (`/bin/qtop`) is the essence of `kannaka-tui`: a live system
dashboard. The original is a full-screen ratatui UI, but raw console control
(`cons_write`) needs the console-device capability only qsh holds, so qtop is
an honest **snapshot** dashboard built from the uncapped `SYS_SYSINFO` surface
and printed as framed lines — a memory gauge from the real frame allocator, the
wall clock, a decorative resonance-field wave (`fx_sin`, phase-shifted by
uptime), and the live process table. It renders once and exits — a `top` for
QuantumOS. A ci-smoke gate asserts `DASHBOARD RENDERED`.

`SYS_SYSINFO` also carries the uncapped bufferless config queries a coupling
service reads: `SYSINFO_QUIET` (booted `quiet`?), `SYSINFO_PEER` (arg 2 =
index → the packed `peer=` IP at that index, 0 out of range), and
`SYSINFO_PEER_COUNT` (how many peers `peer=` configured — 1 for a 2-VM
society, up to `MAX_PEERS` for the N-way mean-field society of epic #139).
These name no authority; they only report boot config.

Together the four citizens map the constellation onto QuantumOS:
consciousness-core → consciousnessd, kannaka-memory → kannakad,
kannaka-quantum → quantumd, kannaka-tui → qtop.
