//! qsys: the ring-3 syscall layer for Rust citizens (ADR-0032; #263 slice 1,
//! #340 slice 2).
//!
//! The Rust twin of `user/usys.h`. Same `int 0x80` gate, same register
//! convention (number in rax, arguments in rdi/rsi/rdx, result in rax), same
//! numbers, same request/response layouts. Every struct shared with the
//! kernel is `#[repr(C)]` and its size is pinned by a compile-time assert, the
//! same way usys.h pins its twins with `_Static_assert`: a layout drift fails
//! the build, not the boot.
//!
//! Nothing here floats or unwinds. The target is `x86_64-unknown-none`
//! (soft-float, no SSE, no red zone), which matches the C citizens'
//! `-mno-sse -mno-red-zone` and ADR-0004. Allocation is opt-in: `heap::LibqAlloc`
//! is a `GlobalAlloc` over libq's arena (`user/libq/heap.c`, linked by the
//! Makefile), for citizens that declare it.
//!
//! SPDX-License-Identifier: GPL-2.0-only

#![no_std]
#![allow(dead_code)]

use core::arch::asm;

// --- syscall numbers (usys.h) -----------------------------------------------
pub const SYS_WRITE: i64 = 1;
pub const SYS_GETPID: i64 = 2;
pub const SYS_YIELD: i64 = 3;
pub const SYS_EXIT: i64 = 4;
pub const SYS_TICKS: i64 = 5;
pub const SYS_SEND: i64 = 6;
pub const SYS_RECV: i64 = 7;
pub const SYS_HEARTBEAT: i64 = 8;
pub const SYS_SVC_RESTARTS: i64 = 9;
pub const SYS_QRAND: i64 = 10;
pub const SYS_SEND_TO: i64 = 11;
pub const SYS_COM2: i64 = 12;
pub const SYS_QSEED: i64 = 13;
pub const SYS_FIELD_SNAPSHOT: i64 = 14;
pub const SYS_CONS: i64 = 15;
pub const SYS_SYSINFO: i64 = 16;
pub const SYS_OPEN: i64 = 17;
pub const SYS_READ: i64 = 18;
pub const SYS_CLOSE: i64 = 19;
pub const SYS_READDIR: i64 = 20;
pub const SYS_SPAWN: i64 = 21;
pub const SYS_WAITPID: i64 = 22;
pub const SYS_FWRITE: i64 = 23;
pub const SYS_UNLINK: i64 = 24;
pub const SYS_SYNC: i64 = 25;
pub const SYS_RESOLVE: i64 = 26;
pub const SYS_UDP: i64 = 27;
pub const SYS_TCP: i64 = 28;
pub const SYS_IMPRINT: i64 = 29;
pub const SYS_RECALL: i64 = 30;
pub const SYS_FIELD_INFO: i64 = 31;
pub const SYS_AUDIT: i64 = 32;
pub const SYS_MANIFEST: i64 = 33;
pub const SYS_CAP_DERIVE: i64 = 34;
pub const SYS_QPU: i64 = 35;

// --- the negative-errno band (usys.h) ---------------------------------------
pub const EINVAL: i64 = -1;
pub const EFAULT: i64 = -2;
/// -4: refused by the capability system (usys.h's EPERM contract).
pub const EPERM: i64 = -4;
pub const EIO: i64 = -5;
pub const ENOENT: i64 = -6;
/// -7: refused by the egress grant (ADR-0030).
pub const EEGRESS: i64 = -7;
/// -11: SYS_RESOLVE / SYS_UDP / SYS_TCP: not yet, poll again after a yield.
pub const WOULDBLOCK: i64 = -11;
/// SYS_WAITPID: the target is still running.
pub const WAITPID_RUNNING: i64 = 256;

// --- sub-operations ------------------------------------------------------------
pub const AUDIT_OP_READ: i64 = 0;
pub const AUDIT_OP_STATS: i64 = 1;
pub const SYSINFO_PS: i64 = 0;
pub const SYSINFO_MEM: i64 = 1;
pub const SYSINFO_TIME: i64 = 2;
pub const SYSINFO_QUIET: i64 = 3;
pub const SYSINFO_SCHED: i64 = 6;
pub const CONS_READ: i64 = 0;
pub const CONS_WRITE: i64 = 1;
pub const COM2_READ: i64 = 0;
pub const COM2_WRITE: i64 = 1;
pub const O_RDONLY: i64 = 0;
pub const O_WRONLY: i64 = 1;
pub const O_CREAT: i64 = 2;
pub const O_TRUNC: i64 = 4;

// --- raw gate ------------------------------------------------------------------
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

// --- process ---------------------------------------------------------------------
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

pub fn yield_now() {
    unsafe {
        sys0(SYS_YIELD);
    }
}

pub fn ticks() -> i64 {
    unsafe { sys0(SYS_TICKS) }
}

pub fn exit(code: i64) -> ! {
    unsafe {
        sys1(SYS_EXIT, code);
    }
    loop {}
}

/// Service liveness: a monitored service heartbeats so the watchdog keeps it.
pub fn heartbeat() {
    unsafe {
        sys0(SYS_HEARTBEAT);
    }
}

/// Restart count of the caller's service slot, or -1 for a non-service.
pub fn svc_restarts() -> i64 {
    unsafe { sys0(SYS_SVC_RESTARTS) }
}

/// SYS_SPAWN: `cmdline` is NUL-terminated, the first token names the initrd
/// program. Returns the new pid, ENOENT, EIO, or EPERM without the spawn right.
pub fn spawn(cmdline: &[u8]) -> i64 {
    debug_assert!(cmdline.last() == Some(&0));
    unsafe { sys1(SYS_SPAWN, cmdline.as_ptr() as i64) }
}

/// SYS_WAITPID: the exit code (0..255), WAITPID_RUNNING while it lives, ENOENT.
pub fn waitpid(pid: i64) -> i64 {
    unsafe { sys1(SYS_WAITPID, pid) }
}

// --- introspection (uncapped, read-only) --------------------------------------
/// Read the authority ledger. Returns bytes copied.
pub fn audit(op: i64, buf: &mut [u8]) -> i64 {
    unsafe { sys3(SYS_AUDIT, op, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

/// Read every bound intent manifest as text. Returns bytes copied.
pub fn manifest(buf: &mut [u8]) -> i64 {
    unsafe { sys2(SYS_MANIFEST, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

/// Kernel-formatted introspection text (SYSINFO_PS / _MEM / _TIME / _SCHED).
pub fn sysinfo(op: i64, buf: &mut [u8]) -> i64 {
    unsafe { sys3(SYS_SYSINFO, op, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

/// 1 if the kernel booted `quiet`, else 0.
pub fn sysinfo_quiet() -> i64 {
    unsafe { sys3(SYS_SYSINFO, SYSINFO_QUIET, 0, 0) }
}

// --- console (capability: DEVICE_ID_CONSOLE) -----------------------------------
pub fn cons_read(buf: &mut [u8]) -> i64 {
    unsafe { sys3(SYS_CONS, CONS_READ, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

pub fn cons_write(buf: &[u8]) -> i64 {
    unsafe { sys3(SYS_CONS, CONS_WRITE, buf.as_ptr() as i64, buf.len() as i64) }
}

// --- files (initrd + RAM overlay; writes need the filesystem-write capability) -
pub fn open(path: &[u8], flags: i64) -> i64 {
    debug_assert!(path.last() == Some(&0));
    unsafe { sys2(SYS_OPEN, path.as_ptr() as i64, flags) }
}

pub fn read(fd: i64, buf: &mut [u8]) -> i64 {
    unsafe { sys3(SYS_READ, fd, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

pub fn close(fd: i64) -> i64 {
    unsafe { sys1(SYS_CLOSE, fd) }
}

pub fn readdir(path: &[u8], buf: &mut [u8]) -> i64 {
    debug_assert!(path.last() == Some(&0));
    unsafe { sys3(SYS_READDIR, path.as_ptr() as i64, buf.as_mut_ptr() as i64, buf.len() as i64) }
}

pub fn fwrite(fd: i64, buf: &[u8]) -> i64 {
    unsafe { sys3(SYS_FWRITE, fd, buf.as_ptr() as i64, buf.len() as i64) }
}

pub fn unlink(path: &[u8]) -> i64 {
    debug_assert!(path.last() == Some(&0));
    unsafe { sys1(SYS_UNLINK, path.as_ptr() as i64) }
}

pub fn sync() -> i64 {
    unsafe { sys0(SYS_SYNC) }
}

// --- the holographic field (capability: CAP_RESOURCE_FIELD over exactly `region`)
pub const FIELD_PAT_MAX: usize = 64;
pub const FIELD_RANK_MAX: usize = 8;
pub const FIELD_SLOTS: usize = 8;
pub const FIELD_PREVIEW: usize = 16;
/// Q15 1.0 and the floor below which the kernel refuses to imprint.
pub const FIELD_ENERGY_MAX: i32 = 0x7FFF;
pub const FIELD_ENERGY_MIN: i32 = 0x0800;

/// Twin of `field_imprint_req_t` (76 bytes).
#[repr(C)]
pub struct FieldImprintReq {
    pub region: u32,
    pub len: u32,
    /// 0 = the kernel default (0x4000)
    pub energy_q15: i32,
    pub pattern: [u8; FIELD_PAT_MAX],
}

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

/// Twin of `field_slot_info_t` (40 bytes).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FieldSlotInfo {
    pub slot: u32,
    pub len: u32,
    pub energy_q15: i32,
    pub eff_energy_q15: i32,
    pub retrievals: u32,
    pub age_ticks: u32,
    pub preview: [u8; FIELD_PREVIEW],
}

/// Twin of `field_info_out_t` (332 bytes).
#[repr(C)]
pub struct FieldInfoOut {
    pub region: u32,
    pub live: u32,
    pub capacity: u32,
    pub slots: [FieldSlotInfo; FIELD_SLOTS],
}

const _: () = assert!(core::mem::size_of::<FieldImprintReq>() == 76, "field imprint req twin ABI drift");
const _: () = assert!(core::mem::size_of::<FieldRecallReq>() == 76, "field recall req twin ABI drift");
const _: () = assert!(core::mem::size_of::<FieldRecallOut>() == 136, "field recall out twin ABI drift");
const _: () = assert!(core::mem::size_of::<FieldSlotInfo>() == 40, "field slot info twin ABI drift");
const _: () = assert!(core::mem::size_of::<FieldInfoOut>() == 332, "field info out twin ABI drift");

impl FieldRecallOut {
    pub const fn zeroed() -> Self {
        FieldRecallOut {
            n: 0,
            rank: [FieldRank { slot: 0, score_q15: 0 }; FIELD_RANK_MAX],
            winner_len: 0,
            winner: [0; FIELD_PAT_MAX],
        }
    }

    /// The winner's stored bytes (the completion the field recovered).
    pub fn winner_bytes(&self) -> &[u8] {
        let n = (self.winner_len as usize).min(FIELD_PAT_MAX);
        &self.winner[..n]
    }
}

impl FieldInfoOut {
    pub const fn zeroed() -> Self {
        const SLOT: FieldSlotInfo = FieldSlotInfo {
            slot: 0,
            len: 0,
            energy_q15: 0,
            eff_energy_q15: 0,
            retrievals: 0,
            age_ticks: 0,
            preview: [0; FIELD_PREVIEW],
        };
        FieldInfoOut { region: 0, live: 0, capacity: 0, slots: [SLOT; FIELD_SLOTS] }
    }
}

/// SYS_IMPRINT: the slot index, EINVAL for a degenerate pattern, EPERM without
/// CAP_WRITE over exactly `req.region`.
pub fn imprint(req: &FieldImprintReq) -> i64 {
    unsafe { sys1(SYS_IMPRINT, req as *const _ as i64) }
}

/// SYS_RECALL: 0 with `out.n` rankings (n = 0: nothing resonates, still
/// success), or EPERM for a caller holding no capability over exactly
/// `req.region`.
pub fn recall(req: &FieldRecallReq, out: &mut FieldRecallOut) -> i64 {
    unsafe { sys2(SYS_RECALL, req as *const _ as i64, out as *mut _ as i64) }
}

/// SYS_FIELD_INFO: 0, EPERM (capless / wrong region), EFAULT, EINVAL.
pub fn field_info(region: u32, out: &mut FieldInfoOut) -> i64 {
    unsafe { sys2(SYS_FIELD_INFO, region as i64, out as *mut _ as i64) }
}

fn copy_pattern(dst: &mut [u8; FIELD_PAT_MAX], src: &[u8]) -> u32 {
    let mut n = 0u32;
    for (d, &s) in dst.iter_mut().zip(src.iter()) {
        *d = s;
        n += 1;
    }
    n
}

/// Imprint `text` (truncated to FIELD_PAT_MAX) with `energy_q15` (0 = default).
pub fn imprint_text(region: u32, text: &[u8], energy_q15: i32) -> i64 {
    let mut req = FieldImprintReq { region, len: 0, energy_q15, pattern: [0; FIELD_PAT_MAX] };
    req.len = copy_pattern(&mut req.pattern, text);
    imprint(&req)
}

/// Recall by `probe` (truncated to FIELD_PAT_MAX), `k` rankings wanted.
pub fn recall_text(region: u32, probe: &[u8], k: u32, out: &mut FieldRecallOut) -> i64 {
    let mut req = FieldRecallReq { region, len: 0, k, probe: [0; FIELD_PAT_MAX] };
    req.len = copy_pattern(&mut req.probe, probe);
    recall(&req, out)
}

// --- one-hop delegation (ADR-0010) ---------------------------------------------
pub const CAP_READ: u32 = 0x01;
pub const CAP_WRITE: u32 = 0x02;
/// cap_resource_type_t: holographic field regions.
pub const CAP_RESOURCE_FIELD: u32 = 6;

/// Twin of `cap_derive_req_t` (24 bytes).
#[repr(C)]
pub struct CapDeriveReq {
    pub resource_type: u32,
    pub resource_id: u32,
    pub permissions: u32,
    pub target_pid: u32,
    /// 0 = none (clamped to the parent's)
    pub expiration: u64,
}
const _: () = assert!(core::mem::size_of::<CapDeriveReq>() == 24, "cap_derive req twin ABI drift");

/// SYS_CAP_DERIVE: 0, or EPERM (not grantable / not an IPC peer), EINVAL.
pub fn cap_derive(req: &CapDeriveReq) -> i64 {
    unsafe { sys1(SYS_CAP_DERIVE, req as *const _ as i64) }
}

// --- the allocator (#340 slice 2) ---------------------------------------------
/// A `GlobalAlloc` over libq's heap arena: the same `malloc`/`free`/`realloc`
/// in `user/libq/heap.c` the C citizens use (a static bss arena, first-fit,
/// 16-byte-aligned payloads, no SYS_BRK). The Makefile links `libq.a` into
/// every Rust citizen; only one that declares
/// `#[global_allocator] static A: qsys::heap::LibqAlloc = qsys::heap::LibqAlloc;`
/// pulls the arena in. Alignments above 16 are refused (null), which `alloc`
/// turns into `handle_alloc_error`, the panic handler, and a loud exit.
pub mod heap {
    use core::alloc::{GlobalAlloc, Layout};

    extern "C" {
        fn malloc(n: usize) -> *mut u8;
        fn free(p: *mut u8);
        fn realloc(p: *mut u8, n: usize) -> *mut u8;
    }

    pub const ALIGN_MAX: usize = 16;

    pub struct LibqAlloc;

    unsafe impl GlobalAlloc for LibqAlloc {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            if layout.align() > ALIGN_MAX {
                return core::ptr::null_mut();
            }
            malloc(layout.size().max(1))
        }

        unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
            free(ptr)
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            if layout.align() > ALIGN_MAX {
                return core::ptr::null_mut();
            }
            realloc(ptr, new_size.max(1))
        }
    }
}

// --- a console line builder ------------------------------------------------------
/// A fixed-capacity console line: bytes and decimals appended, then written
/// NUL-terminated through SYS_WRITE. No allocator, no core::fmt (which would
/// pull in the formatting machinery ADR-0032 measured at 170 KB).
pub struct Line {
    buf: [u8; LINE_MAX],
    len: usize,
}

/// The kernel's `sys_write` copies at most 128 bytes of a line
/// (`kernel/src/syscall.c`), so a longer line would be cut there; a `Line`
/// cuts at the same length, and a 64-byte pattern plus ~60 bytes of framing
/// fits. Two patterns do not: print them on two lines.
pub const LINE_MAX: usize = 128;

impl Line {
    pub const fn new() -> Self {
        Line { buf: [0; LINE_MAX], len: 0 }
    }

    pub fn bytes(&mut self, s: &[u8]) -> &mut Self {
        for &b in s {
            if self.len < self.buf.len() - 1 {
                self.buf[self.len] = b;
                self.len += 1;
            }
        }
        self
    }

    pub fn str(&mut self, s: &str) -> &mut Self {
        self.bytes(s.as_bytes())
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

impl Default for Line {
    fn default() -> Self {
        Self::new()
    }
}
