//! rfieldd: a Rust SERVICE of QuantumOS that allocates and holds a capability
//! (#340 slices 2 and 3, ADR-0032).
//!
//! Where /bin/rhello (#263) is a spawned program with no heap and no
//! authority, rfieldd is embedded in the kernel image like kannakad and
//! started by `citizens.c` with a declarative grant: CAP_RESOURCE_FIELD over
//! region 7, re-minted (and the region scrubbed) on every start, the way every
//! C service gets its authority. It is monitored, so it heartbeats after its
//! one-shot demo. What it proves, each as a gate line `make ci-smoke` checks:
//!
//!   1. slice 2, the allocator: a `Vec` grown to 1,000 entries, shrunk to 10,
//!      regrown and dropped, through `qsys::heap::LibqAlloc`, the `GlobalAlloc`
//!      over libq's own arena (`user/libq/heap.c`, linked into this program);
//!   2. slice 3, the capability: three imprints land in region 7, a
//!      byte-corrupted probe recalls the EXACT stored text (the completion,
//!      not the probe), SYS_FIELD_INFO counts the three, and a recall against
//!      kannakad's region 1 is refused with exactly -4: the grant names one
//!      region, and Rust is held to it like C.
//!
//! No `core::fmt`: lines are built with `qsys::Line` (ADR-0032's 170 KB trap).
//!
//! SPDX-License-Identifier: GPL-2.0-only

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use qsys::{FieldInfoOut, FieldRecallOut, Line};

#[global_allocator]
static ALLOC: qsys::heap::LibqAlloc = qsys::heap::LibqAlloc;

/// The field region this service's cap names (citizens.c: field_region = 7).
const REGION: u32 = 7;
/// kannakad's region: the cross-region negative control.
const OTHER_REGION: u32 = 1;
const EXIT_PANIC: i64 = 99;

const SEEDS: [&[u8]; 3] = [
    b"a rust citizen remembers the field",
    b"the kernel keeps the physics",
    b"an allocator over the libq arena",
];
const ENERGIES: [i32; 3] = [0x7000, 0x5000, 0x4000];
/// Seed 0 with four bytes corrupted; the field must return seed 0 verbatim.
const PROBE: &[u8] = b"a rust citjzen remembers the fjeld";

/// Slice 2: grow, shrink, regrow and drop a Vec through the libq arena.
fn alloc_demo(line: &mut Line) -> bool {
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1000u32 {
        v.push(i * 7);
    }
    let mut sum: u64 = 0;
    for &x in v.iter() {
        sum += x as u64;
    }
    let grown = v.len();
    v.truncate(10);
    v.shrink_to_fit();
    let shrunk_cap = v.capacity();
    for i in 0..990u32 {
        v.push(i);
    }
    let regrown = v.len();
    drop(v);
    // A second full cycle: free() gave the arena back, so this must succeed too.
    let mut w: Vec<u64> = Vec::with_capacity(1000);
    for i in 0..1000u64 {
        w.push(i);
    }
    let second = w.len();
    drop(w);

    let ok = grown == 1000 && sum == 3_496_500 && shrunk_cap < 1000 && regrown == 1000 && second == 1000;
    if ok {
        line.str("RFIELD: alloc ok: vec grown to 1000 (sum=").dec(sum as i64)
            .str("), shrunk to cap ").dec(shrunk_cap as i64)
            .str(", regrown to 1000, dropped, 1000 again")
            .write();
    } else {
        line.str("RFIELD: FAIL alloc: grown=").dec(grown as i64).str(" sum=").dec(sum as i64)
            .str(" shrunk_cap=").dec(shrunk_cap as i64).str(" regrown=").dec(regrown as i64)
            .str(" second=").dec(second as i64).write();
    }
    ok
}

/// Slice 3: imprint, recall through the corruption, count, and be refused elsewhere.
fn field_demo(line: &mut Line) -> bool {
    let mut imprinted = 0;
    for (seed, &energy) in SEEDS.iter().zip(ENERGIES.iter()) {
        if qsys::imprint_text(REGION, seed, energy) >= 0 {
            imprinted += 1;
        }
    }
    line.str("RFIELD: imprinted ").dec(imprinted).str("/3 into region ").dec(REGION as i64).write();

    let mut out = FieldRecallOut::zeroed();
    let r = qsys::recall_text(REGION, PROBE, 3, &mut out);
    let winner_ok = r == 0 && out.n > 0 && out.winner_bytes() == SEEDS[0];
    if r == 0 && out.n > 0 {
        line.str("RFIELD: winner=\"").bytes(out.winner_bytes()).str("\" slot=").dec(out.rank[0].slot as i64)
            .str(" score=").dec(out.rank[0].score_q15 as i64).str(" (n=").dec(out.n as i64).str(")").write();
    } else {
        line.str("RFIELD: FAIL recall returned ").dec(r).str(" n=").dec(out.n as i64).write();
    }

    let mut info = FieldInfoOut::zeroed();
    let ri = qsys::field_info(REGION, &mut info);
    let info_ok = ri == 0 && info.region == REGION && info.live == 3;
    line.str(if info_ok { "RFIELD: field_info live=" } else { "RFIELD: FAIL field_info ret=" })
        .dec(if info_ok { info.live as i64 } else { ri })
        .str(" capacity=").dec(info.capacity as i64).write();

    let mut other = FieldRecallOut::zeroed();
    let rx = qsys::recall_text(OTHER_REGION, PROBE, 1, &mut other);
    let cross_ok = rx == qsys::EPERM;
    if cross_ok {
        line.str("RFIELD: cross-region denied (EPERM)").write();
    } else {
        line.str("RFIELD: FAIL cross-region NOT denied (ret ").dec(rx).str(")").write();
    }

    imprinted == 3 && winner_ok && info_ok && cross_ok
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let mut line = Line::new();
    line.str("rfieldd: rust service holding CAP_RESOURCE_FIELD over region ").dec(REGION as i64)
        .str(", allocating over the libq arena (#340)").write();

    let a = alloc_demo(&mut line);
    let f = field_demo(&mut line);
    if a && f {
        line.str("rfieldd: FIELD VERIFIED").write();
    } else {
        line.str("rfieldd: FIELD FAILED").write();
    }

    // Service liveness: heartbeat so the watchdog keeps us resident.
    loop {
        qsys::heartbeat();
        qsys::yield_now();
    }
}

/// A panic is a visible failure, not a silent spin: say so and exit 99.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    qsys::write_cstr(b"RFIELD: PANIC\0");
    qsys::exit(EXIT_PANIC)
}
