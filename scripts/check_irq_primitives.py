#!/usr/bin/env python3
"""Every interrupt-flag primitive in the kernel must be one ADR-0005 already knows about.

ADR-0005 makes single-core the kernel's concurrency model: the only concurrency is an
interrupt against mainline, and every shared table is guarded by a self-contained
`pushfq; cli` ... `popfq` bracket in one of eleven per-subsystem helpers, never by a lock.
ADR-0033 measured that inventory (39 brackets in 14 files) and noted that it was enforced by
comments alone: nothing stopped a PR from opening a new bracket, enabling interrupts inside
a syscall with a stray `sti`, or adding a twelfth helper, and the inventory would silently
drift. This check makes that drift a build failure.

It counts the IF primitives (`cli`, `sti`, `pushfq`, `popfq`) in inline-asm string
literals of kernel C and in the assembly files, per file, and compares the table against
EXPECTED below. Any difference, up or down, fails: a new primitive needs the ADR-0005
clause it preserves cited in the PR, and a removed one should be struck from the table in
the same PR, so the table stays the inventory.

    python3 scripts/check_irq_primitives.py          # exit 0 when the tree matches
    python3 scripts/check_irq_primitives.py --print  # print the measured table

Helper call sites (`cap_irq_save()` and friends) are not counted: using the model is fine,
adding to it is what this gate watches.
"""
import re
import sys
from pathlib import Path

ROOTS = ("kernel/src", "kernel/include")
MNEMONICS = ("cli", "sti", "pushfq", "popfq")

# Measured on main 8b7792b (2026-10-30) for ADR-0033. file -> {mnemonic: count}.
EXPECTED = {
    "kernel/src/audit.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/boot.S": {"cli": 5, "popfq": 1, "pushfq": 1},
    "kernel/src/capability.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/console.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/e1000.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/egress.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/interrupts.S": {"cli": 1, "sti": 1},
    "kernel/src/interrupts.c": {"cli": 1, "sti": 1},
    "kernel/src/ipc/ipc.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/manifest.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/memory.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/qpu.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/rtl8139.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/service.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    "kernel/src/vga.c": {"cli": 1, "popfq": 1, "pushfq": 1},
    # virtio_net restores with a conditional `sti` rather than popfq (virtio_net.c:666-669).
    "kernel/src/virtio_net.c": {"cli": 1, "pushfq": 1, "sti": 1},
}

C_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
C_STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
S_COMMENT = re.compile(r"/\*.*?\*/|#[^\n]*", re.S)
WORD = {m: re.compile(r"\b%s\b" % m) for m in MNEMONICS}


def count_c(text):
    """Mnemonics inside string literals, which is where inline asm lives."""
    found = {}
    for lit in C_STRING.findall(C_COMMENT.sub(" ", text)):
        for m, rx in WORD.items():
            n = len(rx.findall(lit))
            if n:
                found[m] = found.get(m, 0) + n
    return found


def count_s(text):
    """Mnemonics as the first token of an assembly line."""
    found = {}
    for line in S_COMMENT.sub(" ", text).splitlines():
        tok = line.strip().split(None, 1)
        if tok and tok[0] in MNEMONICS:
            found[tok[0]] = found.get(tok[0], 0) + 1
    return found


def measure(repo):
    table = {}
    for root in ROOTS:
        for p in sorted((repo / root).rglob("*")):
            if p.suffix not in (".c", ".h", ".S"):
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            found = count_s(text) if p.suffix == ".S" else count_c(text)
            if found:
                table[p.relative_to(repo).as_posix()] = dict(sorted(found.items()))
    return table


def main():
    repo = Path(__file__).resolve().parent.parent
    table = measure(repo)
    if "--print" in sys.argv[1:]:
        print("EXPECTED = {")
        for f in sorted(table):
            print('    "%s": {%s},' % (f, ", ".join('"%s": %d' % kv for kv in table[f].items())))
        print("}")
        return 0

    bad = 0
    for f in sorted(set(table) | set(EXPECTED)):
        got, want = table.get(f, {}), EXPECTED.get(f, {})
        if got == want:
            continue
        bad += 1
        print("%s: expected %s, found %s" % (f, want or "nothing", got or "nothing"))
    if bad:
        print("\n%d file(s) differ from the ADR-0005 inventory (ADR-0033). A new cli/sti/pushfq/"
              "popfq changes the single-core concurrency model: cite the ADR-0005 clause it "
              "preserves and update EXPECTED in this script in the same PR." % bad)
        return 1
    print("interrupt-flag primitives: %d file(s) match the ADR-0005 inventory"
          % len(EXPECTED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
