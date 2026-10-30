# 5. Single-core by design

Date: 2026-07-11 (as-built record)
Status: Accepted

> **Update (2026-07-11).** The dead APIC declarations this ADR recorded as debt
> (`apic_init`/`apic_timer_init`/`apic_send_eoi`/`apic_timer_handler` — declared, never
> defined, never called) were deleted, together with the unwired `cap_transfer` helper
> (ADR-0010/ADR-0020) and five stale duplicate bare headers under `kernel/include/`
> (`boot.h`, `interrupts.h`, `memory.h`, `quantum_types.h`, `types.h` — superseded by the
> `kernel/include/kernel/` copies everything actually includes). A dead-code sweep in the
> #180/#181 tradition; the build + `ci-smoke` staying green is the proof they were dead.

> **Update (2026-10-30).** Revisited in ADR-0033 for #261, with a measured inventory: 39
> interrupt-off brackets in 14 files (36 through eleven per-subsystem helpers), every service
> an IF=1 kernel thread (not the three this ADR lists), and the local APIC in use for MSI-X
> since #253 (`kernel/src/pci_msix.c`), so "no APIC exists" below is no longer literally true;
> the PIC-8259, the PIT tick, the BSP-only Multiboot1 entry and the absence of any ACPI
> reader still are. The clauses of this ADR are unchanged; ADR-0033 records the staged path
> and the triggers under which they would be renegotiated.

## Context

QuantumOS runs on exactly one CPU. This was never an accident of youth: it is the
load-bearing concurrency model of the kernel, and it is currently *distributed* across
roughly sixty source comments rather than stated once. The model has three actors and
two rules:

- **Syscalls run with interrupts off.** `int 0x80` is installed as an interrupt gate
  (`GATE_TYPE_INTERRUPT | DPL_USER`, kernel/src/syscall.c:1921), so every syscall body
  executes cli'd on the one CPU. This is what makes the static bounce buffers in syscall
  handlers legal single-instance scratch — stated explicitly where SYS_UDP bounces user
  payloads: "syscalls run cli'd on one CPU … the net thread must never see a user
  pointer" (kernel/src/syscall.c:869-872), and echoed by the field subsystem's contract
  ("touched ONLY from syscall context (cli'd, single CPU) … therefore no locking",
  kernel/include/kernel/field.h:20-25) and ramfs.
- **The only concurrency is IRQ-vs-mainline.** Kernel threads that run with IF=1 (the
  service health monitor, the net thread, the idle-loop reaper) interleave with cli'd
  syscalls and with the timer IRQ only via interrupt preemption on one core. Every shared
  table both sides touch is protected by a *self-contained* pushfq/cli…popfq bracket, not
  a lock: the PMM bitmap and kheap free-list (kernel/src/memory.c:27-43), the capability
  table (`cap_irq_save`, kernel/src/capability.c:40-54), manifest mutators
  (kernel/include/kernel/manifest.h:34-38), and the same rule in audit.c, qpu.c, ipc.c and
  service.c. This "IF=1 monitor/reaper vs cli'd syscall" family is exactly the race class
  the adversarial bug-hunt campaign mined — e.g. the capture-before-free ordering in
  cascade revoke, where reading a cap's fields after `free_slot` could attribute a ledger
  entry to the *next* capability minted into the recycled slot
  (kernel/src/capability.c:247-258).
- **Single-writer disciplines replace memory-model reasoning.** The net stack uses SPSC
  rings with a plain-stores → compiler-barrier → one-volatile-index-store publish pattern
  (kernel/src/net_udp.c:14-23), proven first by the rtl8139 IRQ-producer/net-thread-consumer
  rx queue (kernel/src/rtl8139.c:115-156). One 16 KB ring-0 interrupt stack and one TSS
  suffice "on one CPU: the interrupt frame is consumed … before the next ring-3 entry can
  occur" (kernel/src/gdt.c:27-30). The CPUKILL quota kill fires only when the tick
  interrupted ring 3 (`(state->cs & 3) != 0`, kernel/src/scheduler.c:117) — on one core
  that guard alone guarantees no live kernel stack is ever discarded. `start_slot` commits
  spawn → grant-caps → record-pid atomically under one cli window
  (kernel/src/service.c:156-171). `vmspace_destroy` frees page-table frames guarded only
  by "never the CR3-loaded space" (kernel/src/process.c:492-499) — true on one core, and
  there is no TLB-shootdown mechanism because none is needed.

The 2026-07-11 next-phase adversarial panel put SMP on the table as an explicit candidate
and rejected it unanimously (kernel-feasibility and security lenses). The enumeration was
sobering: no APIC exists — `apic_init`/`apic_timer_init`/`apic_send_eoi` are declared and
never defined anywhere in the tree (kernel/include/kernel/interrupts.h:141-144; the stale
duplicate header kernel/include/interrupts.h:130-132 declares them a second time); the
kernel is PIC-8259 + PIT with a Multiboot-v1 BSP-only 32-bit entry. SMP would convert
every irqsave bracket into a spinlock, the lock-free `cap_find*` read side of all gated
syscalls into a reader-locked or RCU path, the single interrupt stack into per-CPU
GDT/TSS/stacks, the CPUKILL guard into an IPI protocol, and `vmspace_destroy` into a
TLB-shootdown problem — invalidating single-core atomicity arguments in ~16 translation
units, most of them hardened by the ~31-bug campaign (PRs #156-#167). It would also
destroy CI determinism: the ~60 grep-based smoke gates assume deterministic single-core
interleaving of console lines (cpu-hog's kill must land in the boot window; SYS_WRITE's
per-line `[user pid=N]` framing assumes no mid-line interleave).

## Decision

1. **The cli/IF=1 single-core concurrency model is a versioned architectural invariant**,
   canonized by this ADR. Its clauses: (a) syscall bodies run cli'd on one CPU and may use
   static kernel scratch; (b) the only concurrency is IRQ-vs-mainline, serialized by
   self-contained irqsave brackets around every table shared with an IF=1 thread;
   (c) IRQ↔thread data flow uses SPSC rings with publish barriers; (d) ordering invariants
   (capture-before-free, caps-cleared-before-state-UNUSED, cs&3 kill guard,
   never-destroy-the-loaded-vmspace) substitute for locks and MUST be preserved as
   ordering, not "fixed" by adding locks.
2. **SMP is rejected for this phase**, not deferred by omission. The scaling axis for the
   agent-native mission is horizontal — N single-core VMs coupled into societies over the
   attested wire (ADR-0014) — not vertical cores inside one kernel. A second core adds
   zero agent-native value at ~24 services and a 100 Hz tick.
3. Any future PR that introduces a new IF=1 kernel thread, a new IRQ-context caller into
   a no-locking subsystem (field.c is explicit: "Adding an IRQ-context caller breaks
   this", kernel/include/kernel/field.h:25), or a syscall path outside the int 0x80 gate
   MUST cite this ADR and show which clause it preserves or renegotiate the model here.

## Consequences

### Positive

- Kernel-wide freedom from lock-ordering, memory-model, and TOCTOU-under-parallelism
  reasoning; the trust core's atomicity arguments stay proof-by-inspection.
- The bug-hunt seam stays closed: the campaign found the IF=1 race family and fixed it
  under single-core assumptions (PRs #156/#159); those fixes remain valid.
- CI gates stay deterministic and un-fakeable; no QEMU invocation in the repo passes
  `-smp`, so every gate runs the model it certifies.
- Static bounce buffers keep large copy paths (AUDIT 24 KB, MANIFEST 16 KB) off the one
  16 KB interrupt stack without per-CPU machinery.

### Negative

- Throughput is capped at one core; a compute-heavy citizen (agentd society folds, QPU
  broker work) time-slices at 100 Hz rather than parallelizing.
- cli'd syscalls make kernel latency user-visible: a long syscall (e.g. `persist_sync`'s
  synchronous ATA PIO walk) holds off all interrupts, including the timer.
- Multi-core hardware (every real machine the ISO boots on, per the #103-#105 laptop
  work) leaves N-1 cores idle by design.

### Residual risks

- The invariant is comment-enforced, not mechanism-enforced: nothing stops a PR from
  spawning a new IF=1 thread that touches an un-bracketed table; per-field "cannot tear
  on one CPU" plain reads are individually justified and individually fragile.
- The `apic_*` declared-never-defined prototypes (kernel/include/kernel/interrupts.h:141-144,
  duplicated in kernel/include/interrupts.h:130-132) are recorded debt of the same class
  the #180/#181 dead-optionality sweeps removed; they should be deleted, not implemented,
  while this ADR stands.
- If SMP is ever revisited, this ADR's clause list is the minimum re-verification surface
  — every cited anchor becomes a locking or IPI work item, and the gate suite needs a
  determinism strategy first.

## Evidence

- Shipped in: PR #156 (quantum-stack bug-hunt, 7 fixes incl. IF=1 race brackets), PR #159
  (trust-core bug-hunt, 6 fixes incl. capture-before-free cascade revoke); the model
  itself predates both and ships in every kernel PR since the interrupt-gate syscall ABI.
- Key code: kernel/src/syscall.c:869-872 (static bounce, cli'd contract);
  kernel/src/syscall.c:1918-1924 (int 0x80 interrupt gate); kernel/src/capability.c:40-54
  and :247-258 (cap_irq_save, capture-before-free); kernel/src/memory.c:27-43 (PMM/heap
  irqsave); kernel/include/kernel/manifest.h:34-38; kernel/include/kernel/field.h:20-25
  (no locking by design); kernel/src/net_udp.c:14-23 and kernel/src/rtl8139.c:115-156
  (SPSC + publish barrier); kernel/src/gdt.c:27-30 (single interrupt stack);
  kernel/src/scheduler.c:105-131 (CPUKILL cs&3 guard); kernel/src/service.c:156-171
  (start_slot cli window); kernel/src/process.c:492-499 (no TLB shootdown needed).
- CI gates: `ci-smoke` boot gates run under default single-vCPU QEMU (no `-smp` anywhere
  in Makefile or scripts/qos_bridge.py) — PMMROVER (Makefile:454) exercises the
  irqsave-bracketed allocator, the CPUKILL gate (Makefile:992) proves the cs&3 quota-kill
  path, and the whole ~60-gate grep suite's determinism is itself standing evidence that
  one core is what boots.
