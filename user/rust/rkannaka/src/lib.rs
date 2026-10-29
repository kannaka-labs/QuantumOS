//! rkannaka: the Rust kannaka client of the kernel holographic field (#340,
//! ADR-0032): kannaka-memory's verbs, inside the guest, as a SERVICE.
//!
//! The host's kannaka-memory stores f32 vectors; inside QuantumOS the kernel
//! field IS the memory (ADR-0006: 64-byte patterns, Q15 energy, 8 slots per
//! region, ranked recall by resonance). This citizen is the front end to it:
//! `imprint <importance> <text>`, `recall <k> <probe>`, `expect <text>` and
//! `info`, read from `/etc/kannaka.session` on the initrd (the RAM overlay
//! shadows it, so a session can be replaced without a rebuild) and replayed
//! at boot, so CI can replay it too. Importance 0..1000 maps to Q15 energy the
//! way the C kannakad maps it. Text longer than FIELD_PAT_MAX is truncated and
//! said so (the >64-byte question is answered in ADR-0032, not here).
//!
//! It is embedded like kannakad and started by `citizens.c` with a declarative
//! CAP_RESOURCE_FIELD grant over region 7, re-minted and scrubbed at every
//! start (it inherited the region and the slice 2-3 proofs from rfieldd, the
//! service it replaces). Before the session it still proves the allocator (a
//! Vec grown, shrunk, regrown, dropped) and the gate (a recall against region
//! 1 is refused with exactly -4). Monitored: it heartbeats after the replay.
//!
//! No `core::fmt`: lines are built with `qsys::Line` (ADR-0032's 170 KB trap).
//!
//! SPDX-License-Identifier: GPL-2.0-only

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use qsys::{FieldInfoOut, FieldRecallOut, Line, FIELD_PAT_MAX};

#[global_allocator]
static ALLOC: qsys::heap::LibqAlloc = qsys::heap::LibqAlloc;

/// The field region this service's cap names (citizens.c: field_region = 7).
const REGION: u32 = 7;
/// kannakad's region: the cross-region negative control.
const OTHER_REGION: u32 = 1;
const SESSION_PATH: &[u8] = b"/etc/kannaka.session\0";
const SESSION_MAX: usize = 8192;
const EXIT_PANIC: i64 = 99;

/// Slice 2's proof, kept: grow, shrink, regrow and drop a Vec through the libq arena.
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
    let mut w: Vec<u64> = Vec::with_capacity(1000);
    for i in 0..1000u64 {
        w.push(i);
    }
    let second = w.len();
    drop(w);

    let ok = grown == 1000 && sum == 3_496_500 && shrunk_cap < 1000 && regrown == 1000 && second == 1000;
    if ok {
        line.str("RKAN: alloc ok: vec grown to 1000 (sum=").dec(sum as i64)
            .str("), shrunk to cap ").dec(shrunk_cap as i64)
            .str(", regrown to 1000, dropped, 1000 again")
            .write();
    } else {
        line.str("RKAN: FAIL alloc: grown=").dec(grown as i64).str(" sum=").dec(sum as i64)
            .str(" shrunk_cap=").dec(shrunk_cap as i64).str(" regrown=").dec(regrown as i64)
            .str(" second=").dec(second as i64).write();
    }
    ok
}

/// Slice 3's negative control, kept: the grant names ONE region.
fn cross_region_control(line: &mut Line) -> bool {
    let mut other = FieldRecallOut::zeroed();
    let rx = qsys::recall_text(OTHER_REGION, b"anything at all", 1, &mut other);
    if rx == qsys::EPERM {
        line.str("RKAN: cross-region denied (EPERM)").write();
        true
    } else {
        line.str("RKAN: FAIL cross-region NOT denied (ret ").dec(rx).str(")").write();
        false
    }
}

/// Read a whole initrd / RAM-overlay file into a Vec (uncapped read-only).
fn read_file(path: &[u8]) -> Option<Vec<u8>> {
    let fd = qsys::open(path, qsys::O_RDONLY);
    if fd < 0 {
        return None;
    }
    let mut data: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 256];
    loop {
        let n = qsys::read(fd, &mut chunk);
        if n <= 0 || data.len() >= SESSION_MAX {
            break;
        }
        data.extend_from_slice(&chunk[..n as usize]);
    }
    qsys::close(fd);
    Some(data)
}

/// milli-importance (kannaka units, 1000 = 1.0) -> Q15 energy, as kannakad does it.
fn q15_energy(milli: u32) -> i32 {
    ((milli.min(1000) as i64 * 32767) / 1000) as i32
}

fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && (s[a] == b' ' || s[a] == b'\t' || s[a] == b'\r') {
        a += 1;
    }
    while b > a && (s[b - 1] == b' ' || s[b - 1] == b'\t' || s[b - 1] == b'\r') {
        b -= 1;
    }
    &s[a..b]
}

/// Split off the first space-separated word; the rest is trimmed.
fn word(s: &[u8]) -> (&[u8], &[u8]) {
    let s = trim(s);
    let mut i = 0;
    while i < s.len() && s[i] != b' ' && s[i] != b'\t' {
        i += 1;
    }
    (&s[..i], trim(&s[i..]))
}

fn parse_u32(s: &[u8]) -> Option<u32> {
    if s.is_empty() || s.len() > 9 {
        return None;
    }
    let mut v: u32 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (c - b'0') as u32;
    }
    Some(v)
}

/// Truncate to FIELD_PAT_MAX, saying so on the console.
fn pattern<'a>(line: &mut Line, text: &'a [u8]) -> &'a [u8] {
    if text.len() > FIELD_PAT_MAX {
        line.str("RKAN: truncated to ").dec(FIELD_PAT_MAX as i64).str(" bytes: \"")
            .bytes(&text[..FIELD_PAT_MAX]).str("\"").write();
        &text[..FIELD_PAT_MAX]
    } else {
        text
    }
}

struct Session {
    imprints: u32,
    recalls: u32,
    expects_ok: u32,
    fails: u32,
    /// The last recall's winner, for `expect`.
    last_winner: Vec<u8>,
    last_recall_ok: bool,
}

impl Session {
    fn imprint(&mut self, line: &mut Line, args: &[u8]) {
        let (milli, text) = word(args);
        let (Some(milli), false) = (parse_u32(milli), text.is_empty()) else {
            self.fails += 1;
            line.str("RKAN: FAIL imprint: usage: imprint <importance 0..1000> <text>").write();
            return;
        };
        let text = pattern(line, text);
        let energy = q15_energy(milli);
        let r = qsys::imprint_text(REGION, text, energy);
        if r >= 0 {
            self.imprints += 1;
            line.str("RKAN: imprint slot=").dec(r).str(" energy=").dec(energy as i64)
                .str(" \"").bytes(text).str("\"").write();
        } else {
            self.fails += 1;
            line.str("RKAN: FAIL imprint returned ").dec(r).str(" for \"").bytes(text).str("\"").write();
        }
    }

    fn recall(&mut self, line: &mut Line, args: &[u8]) {
        let (k, probe) = word(args);
        let (Some(k), false) = (parse_u32(k), probe.is_empty()) else {
            self.fails += 1;
            line.str("RKAN: FAIL recall: usage: recall <k> <probe>").write();
            return;
        };
        let probe = pattern(line, probe);
        let mut out = FieldRecallOut::zeroed();
        let r = qsys::recall_text(REGION, probe, k.clamp(1, qsys::FIELD_RANK_MAX as u32), &mut out);
        self.last_winner.clear();
        self.last_recall_ok = false;
        if r == 0 && out.n > 0 {
            self.recalls += 1;
            self.last_recall_ok = true;
            self.last_winner.extend_from_slice(out.winner_bytes());
            // Two lines: the kernel caps one SYS_WRITE line, and two 64-byte
            // patterns with their framing do not fit in it.
            line.str("RKAN: recall k=").dec(k as i64).str(" \"").bytes(probe).str("\" n=").dec(out.n as i64).write();
            line.str("RKAN:   winner=\"").bytes(out.winner_bytes()).str("\" slot=").dec(out.rank[0].slot as i64)
                .str(" score=").dec(out.rank[0].score_q15 as i64).write();
            for t in 1..(out.n as usize).min(qsys::FIELD_RANK_MAX) {
                line.str("RKAN:   rank ").dec(t as i64).str(": slot ").dec(out.rank[t].slot as i64)
                    .str(" score ").dec(out.rank[t].score_q15 as i64).write();
            }
        } else if r == 0 {
            self.recalls += 1;
            line.str("RKAN: recall k=").dec(k as i64).str(" \"").bytes(probe).str("\" n=0 (nothing resonates)").write();
        } else {
            self.fails += 1;
            line.str("RKAN: FAIL recall returned ").dec(r).str(" for \"").bytes(probe).str("\"").write();
        }
    }

    fn expect(&mut self, line: &mut Line, args: &[u8]) {
        let want = trim(args);
        if self.last_recall_ok && self.last_winner.as_slice() == want {
            self.expects_ok += 1;
            line.str("RKAN: expect ok \"").bytes(want).str("\"").write();
        } else {
            self.fails += 1;
            line.str("RKAN: FAIL expect: winner=\"").bytes(self.last_winner.as_slice())
                .str("\" wanted \"").bytes(want).str("\"").write();
        }
    }

    fn info(&mut self, line: &mut Line) {
        let mut info = FieldInfoOut::zeroed();
        let r = qsys::field_info(REGION, &mut info);
        if r != 0 {
            self.fails += 1;
            line.str("RKAN: FAIL field_info returned ").dec(r).write();
            return;
        }
        line.str("RKAN: info live=").dec(info.live as i64).str(" capacity=").dec(info.capacity as i64).write();
        for s in info.slots.iter().take((info.live as usize).min(qsys::FIELD_SLOTS)) {
            let n = (s.len as usize).min(qsys::FIELD_PREVIEW);
            line.str("RKAN:   slot ").dec(s.slot as i64).str(" energy=").dec(s.energy_q15 as i64)
                .str(" eff=").dec(s.eff_energy_q15 as i64).str(" retrievals=").dec(s.retrievals as i64)
                .str(" \"").bytes(&s.preview[..n]).str(if s.len as usize > n { "...\"" } else { "\"" }).write();
        }
    }

    fn run(&mut self, line: &mut Line, text: &[u8]) {
        for raw in text.split(|&b| b == b'\n') {
            let l = trim(raw);
            if l.is_empty() || l[0] == b'#' {
                continue;
            }
            let (verb, args) = word(l);
            match verb {
                b"imprint" => self.imprint(line, args),
                b"recall" => self.recall(line, args),
                b"expect" => self.expect(line, args),
                b"info" => self.info(line),
                _ => {
                    self.fails += 1;
                    line.str("RKAN: FAIL unknown verb \"").bytes(verb).str("\"").write();
                }
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let mut line = Line::new();
    line.str("rkannaka: kannaka-memory verbs over the kernel field, region ").dec(REGION as i64)
        .str(", session ").bytes(&SESSION_PATH[..SESSION_PATH.len() - 1]).str(" (#340)").write();

    let alloc_ok = alloc_demo(&mut line);
    let gate_ok = cross_region_control(&mut line);

    let mut s = Session {
        imprints: 0,
        recalls: 0,
        expects_ok: 0,
        fails: 0,
        last_winner: Vec::new(),
        last_recall_ok: false,
    };
    match read_file(SESSION_PATH) {
        Some(text) => {
            line.str("RKAN: session ").dec(text.len() as i64).str(" bytes").write();
            s.run(&mut line, &text);
        }
        None => {
            s.fails += 1;
            line.str("RKAN: FAIL cannot open the session file").write();
        }
    }

    if alloc_ok && gate_ok && s.fails == 0 && s.imprints > 0 && s.recalls > 0 {
        line.str("rkannaka: SESSION VERIFIED (").dec(s.imprints as i64).str(" imprints, ")
            .dec(s.recalls as i64).str(" recalls, ").dec(s.expects_ok as i64).str(" expects ok)").write();
    } else {
        line.str("rkannaka: SESSION FAILED (").dec(s.fails as i64).str(" failures)").write();
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
    qsys::write_cstr(b"RKAN: PANIC\0");
    qsys::exit(EXIT_PANIC)
}
