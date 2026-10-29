//! qsys: the ring-3 syscall layer for Rust citizens (#263 slice 1, ADR-0032).
//!
//! The Rust twin of `user/usys.h`. Same `int 0x80` gate, same register
//! convention (number in rax, arguments in rdi/rsi/rdx, result in rax), same
//! numbers, same request/response layouts. Every struct shared with the
//! kernel is `#[repr(C)]` and its size is pinned by a compile-time assert, the
//! same way usys.h pins its twins with `_Static_assert`: a layout drift fails
//! the build, not the boot.
//!
//! Nothing here allocates, floats or unwinds. The target is
//! `x86_64-unknown-none` (soft-float, no SSE, no red zone), which matches the
//! C citizens' `-mno-sse -mno-red-zone` and ADR-0004.
//!
//! SPDX-License-Identifier: GPL-2.0-only

#![allow(dead_code)]

use core::arch::asm;

pub const SYS_WRITE: i64 = 1;
pub const SYS_GETPID: i64 = 2;
pub const SYS_EXIT: i64 = 4;
pub const SYS_SYSINFO: i64 = 16;
pub const SYS_RECALL: i64 = 30;
pub const SYS_AUDIT: i64 = 32;

/// -4: refused by the capability system (usys.h's EPERM contract).
pub const EPERM: i64 = -4;

pub const AUDIT_OP_READ: i64 = 0;
pub const AUDIT_OP_STATS: i64 = 1;
pub const SYSINFO_QUIET: i64 = 3;

/// The kernel preserves every register except rax across `int 0x80`, which is
/// what the C wrappers assume too (they declare only rax as output). Memory
/// may be read or written by the call, so the default asm! memory model stays.
#[inline(always)]
pub unsafe fn sys0(n: i64) -> i64 {
    let r: i64;
    asm!("int 0x80", inlateout("rax") n => r, options(nostack));
    r
}

#[inline(always)]
pub unsafe fn sys1(n: i64, a1: i64) -> i64 {
    let r: i64;
    asm!("int 0x80", inlateout("rax") n => r, in("rdi") a1, options(nostack));
    r
}

#[inline(always)]
pub unsafe fn sys2(n: i64, a1: i64, a2: i64) -> i64 {
    let r: i64;
    asm!("int 0x80", inlateout("rax") n => r, in("rdi") a1, in("rsi") a2, options(nostack));
    r
}

#[inline(always)]
pub unsafe fn sys3(n: i64, a1: i64, a2: i64, a3: i64) -> i64 {
    let r: i64;
    asm!("int 0x80", inlateout("rax") n => r, in("rdi") a1, in("rsi") a2, in("rdx") a3,
         options(nostack));
    r
}

/// SYS_WRITE takes a NUL-terminated string; `s` must end in a 0 byte.
pub fn write_cstr(s: &[u8]) {
    debug_assert!(s.last() == Some(&0));
    unsafe {
        sys1(SYS_WRITE, s.as_ptr() as i64);
    }
}

pub fn getpid() -> i64 {
    unsafe { sys0(SYS_GETPID) }
}

pub fn exit(code: i64) -> ! {
    unsafe {
        sys1(SYS_EXIT, code);
    }
    loop {}
}

pub fn sysinfo_quiet() -> i64 {
    unsafe { sys3(SYS_SYSINFO, SYSINFO_QUIET, 0, 0) }
}

/// Read the authority ledger (uncapped, read-only). Returns bytes copied.
pub fn audit(op: i64, buf: &mut [u8]) -> i64 {
    unsafe { sys3(SYS_AUDIT, op, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

pub const FIELD_PAT_MAX: usize = 64;
pub const FIELD_RANK_MAX: usize = 8;

/// Twin of `field_recall_req_t` (76 bytes).
#[repr(C)]
pub struct FieldRecallReq {
    pub region: u32,
    pub len: u32,
    pub k: u32,
    pub probe: [u8; FIELD_PAT_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FieldRank {
    pub slot: u32,
    pub score_q15: i32,
}

/// Twin of `field_recall_out_t` (136 bytes).
#[repr(C)]
pub struct FieldRecallOut {
    pub n: u32,
    pub rank: [FieldRank; FIELD_RANK_MAX],
    pub winner_len: u32,
    pub winner: [u8; FIELD_PAT_MAX],
}

const _: () = assert!(core::mem::size_of::<FieldRecallReq>() == 76, "field recall req twin ABI drift");
const _: () = assert!(core::mem::size_of::<FieldRecallOut>() == 136, "field recall out twin ABI drift");

impl FieldRecallOut {
    pub const fn zeroed() -> Self {
        FieldRecallOut {
            n: 0,
            rank: [FieldRank { slot: 0, score_q15: 0 }; FIELD_RANK_MAX],
            winner_len: 0,
            winner: [0; FIELD_PAT_MAX],
        }
    }
}

/// SYS_RECALL: 0 with `out.n` rankings, or -4 EPERM for a caller holding no
/// capability over exactly `req.region`.
pub fn recall(req: &FieldRecallReq, out: &mut FieldRecallOut) -> i64 {
    unsafe { sys2(SYS_RECALL, req as *const _ as i64, out as *mut _ as i64) }
}

/// A fixed-capacity console line: bytes and decimals appended, then written
/// NUL-terminated through SYS_WRITE. No allocator, no core::fmt (which would
/// pull in the formatting machinery this slice deliberately does without).
pub struct Line {
    buf: [u8; 160],
    len: usize,
}

impl Line {
    pub const fn new() -> Self {
        Line { buf: [0; 160], len: 0 }
    }

    pub fn str(&mut self, s: &str) -> &mut Self {
        for &b in s.as_bytes() {
            if self.len < self.buf.len() - 1 {
                self.buf[self.len] = b;
                self.len += 1;
            }
        }
        self
    }

    pub fn dec(&mut self, v: i64) -> &mut Self {
        let mut tmp = [0u8; 21];
        let mut i = tmp.len();
        let neg = v < 0;
        let mut u = v.unsigned_abs();
        loop {
            i -= 1;
            tmp[i] = b'0' + (u % 10) as u8;
            u /= 10;
            if u == 0 {
                break;
            }
        }
        if neg {
            i -= 1;
            tmp[i] = b'-';
        }
        for &b in &tmp[i..] {
            if self.len < self.buf.len() - 1 {
                self.buf[self.len] = b;
                self.len += 1;
            }
        }
        self
    }

    pub fn write(&mut self) {
        self.buf[self.len] = 0;
        write_cstr(&self.buf[..=self.len]);
        self.len = 0;
    }
}
