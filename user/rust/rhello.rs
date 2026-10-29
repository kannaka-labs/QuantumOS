//! /bin/rhello: the first Rust citizen of QuantumOS (#263 slice 1, ADR-0032).
//!
//! A `no_std`, `no_main` program on the `x86_64-unknown-none` target, built
//! by the Makefile with plain `rustc` into a static archive and linked by the
//! same gcc command and `user.ld` as every C citizen. It rides the initrd as
//! `/bin/rhello` and the shell starts it with `run /bin/rhello` through
//! SYS_SPAWN, exactly like `/bin/hello`.
//!
//! What it proves, each as a gate line `make ci-smoke` checks:
//!   1. the toolchain: Rust code runs in ring 3 on a kernel with no FPU save
//!      and no libc (soft-float target, no SSE, nothing from `std`);
//!   2. the ABI, pointer direction kernel -> user: SYS_AUDIT copies ledger
//!      stats into a buffer that lives on a Rust stack;
//!   3. the ABI, pointer direction user -> kernel, and the capability gate:
//!      SYS_RECALL with a 76-byte `#[repr(C)]` twin of `field_recall_req_t`
//!      is refused with exactly -4 (EPERM), because a spawned program holds
//!      no capability over the field. The refusal is the proof: the gate
//!      that guards C callers guards Rust callers the same way.
//!
//! SPDX-License-Identifier: GPL-2.0-only

#![no_std]
#![no_main]

mod qsys;

use qsys::{FieldRecallOut, FieldRecallReq, Line};

const EXIT_OK: i64 = 7;
const EXIT_AUDIT_FAILED: i64 = 98;
const EXIT_RECALL_UNEXPECTED: i64 = 97;
const EXIT_PANIC: i64 = 99;

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let mut line = Line::new();

    line.str("RHELLO: rust no_std citizen on QuantumOS pid=").dec(qsys::getpid()).write();

    // 2. kernel -> user copy into a Rust stack buffer (uncapped, read-only).
    let mut stats = [0u8; 256];
    let n = qsys::audit(qsys::AUDIT_OP_STATS, &mut stats);
    if n <= 0 || n as usize > stats.len() {
        line.str("RHELLO: FAIL SYS_AUDIT returned ").dec(n).write();
        qsys::exit(EXIT_AUDIT_FAILED);
    }
    line.str("RHELLO: SYS_AUDIT stats copied ").dec(n).str(" bytes into a Rust buffer").write();

    // 3. user -> kernel twin struct through the capability gate.
    let probe = b"the cat sat on the mat";
    let mut req = FieldRecallReq { region: 0, len: probe.len() as u32, k: 1, probe: [0; qsys::FIELD_PAT_MAX] };
    for (dst, &src) in req.probe.iter_mut().zip(probe.iter()) {
        *dst = src;
    }
    let mut out = FieldRecallOut::zeroed();
    let r = qsys::recall(&req, &mut out);
    if r != qsys::EPERM {
        line.str("RHELLO: FAIL SYS_RECALL returned ").dec(r).str(" (expected -4 EPERM for a capless citizen)").write();
        qsys::exit(EXIT_RECALL_UNEXPECTED);
    }
    line.str("RHELLO: SYS_RECALL capless -> -4 (EPERM): the field capability gates Rust callers too").write();

    qsys::exit(EXIT_OK) // the shell reports this code via SYS_WAITPID
}

/// A panic is a visible failure, not a silent spin: say so and exit 99.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    qsys::write_cstr(b"RHELLO: PANIC\0");
    qsys::exit(EXIT_PANIC)
}
