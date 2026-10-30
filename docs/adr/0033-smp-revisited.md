# 33. SMP revisited: single-core stays, with the staged path and the triggers that would reopen it

Date: 2026-10-30
Status: Proposed (answers #261; ADR-0005 stands until this is accepted)

## Context

ADR-0005 (2026-07-11) made single-core a versioned invariant and rejected SMP for that phase.
Issue #261 asks the question again, because of three changes since July: the kernel now runs
on Firecracker microVMs and cloud VMs that can hand out several vCPUs; virtio-net multiqueue
waits on a second core; and #263 (ADR-0032, shipped) put Rust citizens inside the guest, which
is where heavier workloads would run. The issue asks for a decision, not code, and lists what
the decision must answer. Everything below was measured on main at `8b7792b` on 2026-10-30;
the July numbers were estimates ("roughly sixty", "~16 translation units").

### Inventory: what single-core currently carries

The kernel is 50 C translation units and 3 assembly files under `kernel/src`.

**Interrupt-off brackets.** Eleven subsystems each define their own self-contained
`pushfq; pop; cli` … `push; popfq` pair, and there are three raw brackets outside a helper:
36 call sites through the helpers, 39 brackets in all, in 14 files:

| file | helper | call sites | what it guards, and against whom |
|---|---|---|---|
| kernel/src/manifest.c:23-28 | `man_irq_save` | 9 | manifest mutators; syscall vs the IF=1 health monitor |
| kernel/src/audit.c:33-38 | `audit_irq_save` | 6 | the audit ring; syscall vs monitor and reaper |
| kernel/src/memory.c:36-41 | `irq_save` | 5 | PMM bitmap and kheap free list; any allocator caller vs IRQ and threads |
| kernel/src/ipc/ipc.c:136-141 | `ipc_irq_save` | 3 | IPC endpoints; syscall vs peer rewire (ADR-0023) |
| kernel/src/service.c:164-169 | `svc_irq_save` | 2 | `start_slot`'s spawn → grant → record-pid window (service.c:156-171) |
| kernel/src/rtl8139.c:106-111 | `irq_save` | 2 | rx queue head; IRQ producer vs net-thread consumer |
| kernel/src/e1000.c:166 | `irq_save` | 2 | descriptor rings; IRQ vs net thread |
| kernel/src/egress.c:79-85 | `irq_save` | 2 | egress grant table (ADR-0030) |
| kernel/src/console.c:62-68 | `irq_save` | 2 | one input ring with two IRQ producers, COM1 RX and the keyboard (console.c:4) |
| kernel/src/capability.c:47-52 | `cap_irq_save` | 2 | capability table mutations; reads stay lock-free |
| kernel/src/qpu.c:42-47 | `qpu_irq_save` | 1 | QPU job queue |
| kernel/src/virtio_net.c:666-669 | raw, with an explicit `sti` | 1 | virtqueue notify window |
| kernel/src/vga.c:449 | raw | 1 | cursor update |
| kernel/src/boot.S:294 | raw | 1 | early boot |

Three headers also open brackets inline (kernel/include/kernel/manifest.h, audit.h), so
the number of places that *assume* interrupts off is larger than the number of brackets.

**IF=1 kernel threads.** Everything the scheduler starts runs with `eflags = 0x202`
(kernel/src/process.c:362). That is not three threads, as ADR-0005 listed; it is every
service: `svc-monitor` (service.c:876), the net thread (main.c:653), the two demo threads
(main.c:645-648), and one kernel thread per service slot (service.c:260), of which
`citizens.c` declares 20 entries against `MAX_SERVICES 32`. Services are ring-0 kernel
threads, not ring-3 processes. Ring 3 is the user programs and the Rust citizens of
ADR-0032, which have no threads of their own.

**Single-producer rings.** Per-connection TCP rx and tx rings (net_tcp.c:116-126:
net thread produces rx, the RECV syscall consumes; SEND produces tx, the net thread
consumes), the UDP rings (net_udp.c), the rtl8139 and e1000 receive queues (e1000.h:13),
the QPU queue (qpu.c:207) and the console input ring. The publish pattern is plain stores,
a compiler barrier, then one volatile index store. On x86's TSO memory model that pattern
is also correct between two cores for a true single producer and single consumer; the
console ring is the exception, two interrupt producers serialised today only because one
CPU takes one interrupt at a time.

**Per-CPU state that exists once.** One TSS and one `rsp0` (gdt.c:25,43), one 16 KB
interrupt stack, one `current_process` (scheduler.c:440), the CPUKILL guard that trusts
`(state->cs & 3) != 0` to mean "no live kernel stack is being discarded" (scheduler.c:378,
interrupts.c:286), and `vmspace_destroy` (vmspace.c:189) with `invlpg` that flushes only the
executing core's TLB (memory.c:594,635). There is no TLB shootdown because there is no
second TLB.

**Syscalls.** `int 0x80` is still an interrupt gate (syscall.c:1967), so every syscall body
runs with interrupts off and may use static kernel scratch such as `g_field_snap`
(syscall.c:1398).

**The hardware layer, as of today.** Since #253 the kernel does touch the local APIC: MSI-X
vectors are delivered through it and `pci_msix.c:50-55` programs `LAPIC_EOI` and the
spurious vector, with `IA32_APIC_BASE` left as firmware set it. Legacy interrupts still go
through the PIC-8259, the tick is the PIT (interrupts.c:473-479), there is no IOAPIC code,
no ACPI parsing of any kind (no RSDP or MADT reader, so the kernel does not know how many
CPUs exist), and boot is the Multiboot1 BSP-only 32-bit entry that ADR-0031 kept.

**CI.** No QEMU or Firecracker invocation in the repository passes `-smp` or
`vcpu_count` above 1 (scripts/firecracker_net_selftest.sh:40,
scripts/bench_console_output.py:65). The Makefile holds 228 `grep -q` assertions across 83
targets, and the boot gates read console lines in order. `[user pid=N]` framing
(syscall.c:133-158) is "atomic enough for the boot console" because nothing else prints
mid-line.

### The cheaper intermediate: a big kernel lock

The issue asks whether "one CPU in the kernel at a time, ring 3 in parallel" keeps the
current reasoning true. Measured against the inventory:

**Kept true.** Clause (a) of ADR-0005: syscall bodies stay single-instance, so every static
scratch buffer stays legal. All brackets whose two sides are both kernel code (manifest,
audit, capability, ipc, service, qpu, egress, memory: 30 of the 36 helper sites) are
covered by the lock, because kernel threads hold it whenever they run. The
ordering invariants (capture-before-free, caps-cleared-before-UNUSED) stay ordering
arguments inside the lock.

**Broken even under the lock.**

- Every bracket whose other side is an interrupt handler: rtl8139, e1000, virtio_net, the
  console ring, the timer tick's accounting. With two cores, a `cli` on core A does not stop
  core B taking the NIC interrupt. Each of those 8 sites becomes a spinlock plus irqsave, or
  the handler must take the big lock, which it cannot do if the holder is waiting for that
  interrupt. The cheapest shape is *BSP-only device interrupts*: the PIC and PIT stay wired
  to the boot CPU, application processors take only IPIs and their LAPIC timer, and those 8
  sites stay as they are.
- The scheduler: `current_process` and the run queue become per-CPU; the round-robin of
  #336 becomes a per-CPU cursor plus migration; the CPUKILL guard must send an IPI to the
  core running the over-budget process instead of trusting `cs & 3` on the interrupted
  frame.
- `vmspace_destroy`: a process with no threads runs on one core at a time, so user mappings
  need no shootdown as long as migration reloads CR3. Kernel mappings (heap growth, MMIO
  maps) do need one, so memory.c:594 and :635 gain an IPI flush.
- Per-CPU GDT, TSS, `rsp0` and interrupt stacks, allocated per application processor.
- CI determinism: two ring-3 programs printing at once interleave lines, and 228 ordered
  assertions were written against one stream. The console write path needs a per-line
  lock and the gates need to match on pid-tagged lines rather than position, before any
  `-smp 2` leg can be trusted.

**Who benefits.** Under the lock, kernel threads never run in parallel, so the 20
services gain nothing. Only ring 3 runs in parallel, which today means the user programs
and the Rust citizens. A citizen that wants two cores would also need threads, which
ADR-0032's ring 3 does not have. The parallelism the lock buys has no consumer yet.

### The hardware floor

The minimum for even the big-lock variant: an ACPI RSDP scan and MADT walk to count CPUs
(Multiboot1 does not list them); LAPIC enable and ID on every core, building on
`pci_msix.c`; an AP start-up trampoline below 1 MiB (INIT, SIPI, SIPI) that climbs from real
mode to long mode and into the per-CPU stack, written for both the BIOS and the UEFI/GOP
path of ADR-0031; a LAPIC timer or TSC deadline per core, since the PIT is one device;
IPIs for kill, reschedule and TLB flush; and either an IOAPIC or the BSP-only interrupt
routing above.

### Cost

A rough count, in the size of the kernel PRs of October 2026: ACPI + LAPIC bring-up (1),
trampoline and per-CPU structures (2), the lock and the eight IRQ-side spinlocks (2),
per-CPU scheduler, migration and IPI kill (2), kernel TLB shootdown (1), console locking
and gate rewrite (2), and a re-run of the adversarial bug-hunt that found the IF=1 race
family, because every one of its fixes was argued on one core. Ten to twelve PRs and a
campaign, which is the whole of November's kernel capacity. It would displace the cloud
storage work (#281), the remaining hub slices (#297) and the next Rust tool ports (#263
follow-ups). This is an estimate from the inventory, not a measurement.

## Decision

1. **ADR-0005 stands. QuantumOS stays single-core.** The reasons in July still hold and the
   inventory shows the surface grew, not shrank: 39 interrupt-off brackets in 14 files,
   every service an IF=1 kernel thread, and the CI suite's 228 ordered assertions. The parallelism SMP would
   add has no consumer: services could not use it under a big kernel lock, and ring 3 has
   no threads.
2. **Horizontal stays the scaling axis.** On debain2 and the cloud machines a second
   single-core VM costs what a second vCPU costs, and the attested wire (ADR-0014) and the
   hub (ADR-0027) already couple VMs. Heavy work that does not fit one core goes to the host
   through the provider broker (ADR-0026), which is how the QPU and Lab compute paths work
   today.
3. **If SMP is reopened, the only acceptable first shape is the big kernel lock with
   BSP-only device interrupts**, built in the order of the cost section, with the CI
   determinism work (console line lock, pid-tagged gates, a 10-run `-smp 2` leg) landing
   *before* the first application processor is started. No PR may convert an irqsave
   bracket into a spinlock piecemeal ahead of that plan.
4. **Triggers that reopen this ADR**, any one of which is enough:
   - a workload inside the guest, measured over 10 runs per the repository's experiment
     rule, that is CPU-bound on the console machines and cannot be moved to the host broker;
   - a citizen that needs threads by design, which would first be its own ADR against
     ADR-0032;
   - a deployment target that bills per VM rather than per vCPU, making horizontal scaling
     the expensive axis.
5. **Corrections to ADR-0005's record.** The LAPIC is no longer absent: `pci_msix.c` uses it
   for MSI-X delivery and EOI since #253. The IF=1 thread list is every service, not three
   threads. ADR-0005 gets an update note pointing here; its clauses are unchanged.

## Consequences

### Positive

- The trust core keeps its proof-by-inspection atomicity arguments and the 31-bug
  campaign's fixes stay valid as written.
- November's kernel capacity goes to #281, #297 and the Rust tool ports instead of a
  bring-up whose parallelism nothing would consume.
- The staged plan and the triggers are written down, so the next time the question
  comes up the answer starts from this inventory rather than from scratch.

### Negative

- Every real machine and every multi-vCPU VM leaves cores idle by design, still.
- virtio-net multiqueue stays out of reach, and a long cli'd syscall still holds off the
  timer for everyone.
- A CPU-bound citizen time-slices at 100 Hz; the only relief is to move the work out of
  the guest.

### Residual risks

- The invariant was comment-enforced until #355: `scripts/check_irq_primitives.py` now fails
  the build when the per-file counts of `cli`, `sti`, `pushfq` and `popfq` in the kernel's
  inline asm and `.S` files differ from the measured table, up or down, so the inventory
  above maintains itself. Helper call sites are not counted; a new primitive must cite the
  ADR-0005 clause it preserves and update the table in the same PR.
- The console ring's two interrupt producers are correct only because one CPU takes one
  interrupt at a time. That is the first thing to break on any `-smp` boot, and the first
  thing a future bring-up must lock.
- The cost estimate is unmeasured. If SMP is reopened, the first PR should be the ACPI and
  LAPIC bring-up alone, so the estimate can be corrected against a real one.

## Evidence

- Measured on main `8b7792b`, 2026-10-30: `rg '\b[a-z_]*irq_save\(\)' kernel/src` minus the
  definitions gives 36 call sites; `rg pushfq kernel/src` lists the 11 helpers and 3 raw
  brackets; `rg -- '-smp' Makefile scripts tools .github` finds nothing; `grep -c 'grep -q'
  Makefile` gives 228.
- Anchors: kernel/src/process.c:362 (threads start with IF set); kernel/src/service.c:260
  and :876 (services and the monitor are kernel threads); kernel/src/citizens.c (20 entries);
  kernel/include/kernel/service.h:30 (`MAX_SERVICES 32`); kernel/src/pci_msix.c:50-55 (LAPIC
  in use since #253); kernel/src/interrupts.c:473-479 (PIT tick); kernel/src/gdt.c:25,43 (one
  TSS); kernel/src/scheduler.c:378 and kernel/src/interrupts.c:286 (`cs & 3` guards);
  kernel/src/memory.c:594,635 (local `invlpg` only); kernel/src/vmspace.c:189;
  kernel/src/syscall.c:1967 (interrupt-gate syscall), :1398 (static scratch), :133-158
  (`[user pid=N]` framing); kernel/src/net_tcp.c:116-126 and kernel/src/console.c:4 (rings).
- Related: ADR-0005 (the invariant), ADR-0014 (attested wire), ADR-0024 (virtio transports),
  ADR-0026 (provider broker), ADR-0027 (hub), ADR-0031 (Multiboot1 kept, UEFI path),
  ADR-0032 (Rust citizens, no threads in ring 3); issues #261, #263, #281, #297.
