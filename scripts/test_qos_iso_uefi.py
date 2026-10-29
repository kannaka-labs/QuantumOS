#!/usr/bin/env python3
"""UEFI boot gate (#264): boot the GRUB ISO under OVMF and reach the shell.

Same image as ci-smoke-iso, different firmware: edk2 OVMF via pflash (or the
single-file OVMF.fd via -bios), the ISO on the -cdrom device, no CSM. What
this proves, in order:

  1. the ISO carries an EFI El Torito entry (grub-mkrescue only adds one
     when grub-efi-amd64-bin is installed; without it the firmware reports
     the DVD "Not Found" and drops to its shell);
  2. GRUB's multiboot loader can place the kernel under UEFI (a 64 MB
     stack/heap PT_LOAD used to make it fail with "error: out of memory");
  3. the kernel takes the GOP framebuffer the firmware offers, notices its
     own header never asked for one, and renders the screen console on it —
     the only display a UEFI-only machine has;
  4. the usual quiet-console gates: shell session, a citizen verdict, at
     most one timer-tick line.

Commands are typed only after the shell announces itself: bytes piped from
t=0 would land in the firmware's countdown or GRUB's menu instead.

Usage: test_qos_iso_uefi.py build/x86_64/kernel.iso
"""
import functools
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time

print = functools.partial(print, flush=True)  # survive a `timeout` kill with the log intact

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][0-9A-Z]|\x1b\[=3h")

SHELL_READY = "QSH: QuantumOS interactive shell ready"
COMMANDS = b"help\nghost\nrun /bin/consciousnessd\nexit\n"
LAST_GATE = "consciousnessd: CONSCIOUSNESS EMERGED"

REQUIRED = [
    "GNU GRUB",  # the firmware found and started the EFI boot image
    "Linear framebuffer active — console image, firmware has no text mode (#264)",
    "STACK: boot stack high-water ",  # the init chain fitted (it used to overflow into fb_map_pd)
    "PGTBL: low identity map intact",
    "QuantumOS ready",
    "CONS: screen console active (framebuffer ",
    SHELL_READY,
    "qsh commands:",
    LAST_GATE,
]
FORBIDDEN = [
    ("error: out of memory", "GRUB could not place the kernel under UEFI (the #264 regression)"),
    ("CONS: screen console active (VGA text", "UEFI boot took the VGA-text path (0xB8000 is RAM here)"),
    ("No framebuffer", "GRUB handed over without framebuffer info (console entry lacks the video setup)"),
    ("Not Found", "the firmware could not boot the ISO (no EFI El Torito entry?)"),
    ("Page fault at address", "the kernel faulted (a lost framebuffer mapping reads exactly like this)"),
    ("boot stack OVERFLOW", "the boot stack overflowed into the memory above it"),
]

OVMF_CODE = [
    "/usr/share/OVMF/OVMF_CODE_4M.fd",
    "/usr/share/OVMF/OVMF_CODE.fd",
    "/usr/share/edk2/x64/OVMF_CODE.4m.fd",
    "/usr/share/edk2/ovmf/OVMF_CODE.fd",
]
OVMF_SINGLE = ["/usr/share/ovmf/OVMF.fd", "/usr/share/qemu/OVMF.fd", "/usr/share/edk2/x64/OVMF.4m.fd"]


def find_firmware(tmpdir):
    """Return the qemu args that attach OVMF, or None."""
    env = os.environ.get("QOS_OVMF_CODE")
    if env:
        OVMF_CODE.insert(0, env)
    for code in OVMF_CODE:
        if not os.path.isfile(code):
            continue
        vars_src = code.replace("CODE", "VARS")
        if not os.path.isfile(vars_src):
            continue
        vars_tmp = os.path.join(tmpdir, "OVMF_VARS.fd")
        shutil.copyfile(vars_src, vars_tmp)
        return [
            "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,file={vars_tmp}",
        ], code
    for single in OVMF_SINGLE:
        if os.path.isfile(single):
            return ["-bios", single], single
    return None, None


def iso_has_efi_entry(iso):
    if not shutil.which("xorriso"):
        return None
    out = subprocess.run(["xorriso", "-indev", iso, "-report_el_torito", "plain"],
                         capture_output=True, text=True).stdout
    return "UEFI" in out


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    iso = sys.argv[1]
    if not os.path.isfile(iso):
        print(f"ERROR: ISO not found: {iso}")
        return 1

    efi = iso_has_efi_entry(iso)
    if efi is False:
        print("ERROR: the ISO has no EFI El Torito entry — install grub-efi-amd64-bin and rebuild")
        return 1
    print(f"  [PASS] ISO carries an EFI boot image" if efi else "  [skip] xorriso missing; EFI entry unchecked")

    tmpdir = tempfile.mkdtemp(prefix="qos-uefi-")
    fw_args, fw_path = find_firmware(tmpdir)
    if not fw_args:
        print("ERROR: no OVMF firmware found (install the `ovmf` package, or set QOS_OVMF_CODE)")
        return 1
    print(f"  firmware: {fw_path}")

    cmd = ["qemu-system-x86_64", *fw_args, "-cdrom", iso, "-m", "128M",
           "-serial", "stdio", "-monitor", "none", "-display", "none", "-no-reboot"]
    budget_s = float(os.environ.get("QOS_UEFI_BUDGET_S", "150"))
    t0 = time.time()
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=0)
    log = bytearray()
    sent = False
    done_at = None
    try:
        fd = proc.stdout.fileno()
        while time.time() - t0 < budget_s:
            # select, never a bare read: an idle guest would block the read
            # forever and the budget/termination logic could not run.
            ready, _, _ = select.select([fd], [], [], 0.2)
            chunk = os.read(fd, 4096) if ready else b""
            if not chunk:
                if proc.poll() is not None:
                    break
            else:
                log += chunk
            text = ANSI.sub(b"", bytes(log)).decode("utf-8", "replace")
            if not sent and SHELL_READY in text:
                time.sleep(1.0)  # let the prompt settle before the first keystroke
                proc.stdin.write(COMMANDS)
                proc.stdin.flush()
                sent = True
            if sent and done_at is None and LAST_GATE in text:
                done_at = time.time()
            if done_at is not None and time.time() - done_at > 3.0:
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmpdir, ignore_errors=True)

    text = ANSI.sub(b"", bytes(log)).decode("utf-8", "replace").replace("\r", "")
    elapsed = time.time() - t0
    failed = False
    for needle, why in FORBIDDEN:
        if needle in text:
            print(f"ERROR: forbidden line present: {needle!r} — {why}")
            failed = True
    for gate in REQUIRED:
        if gate in text:
            print(f"  [PASS] {gate}")
        else:
            print(f"ERROR: UEFI boot gate missing: {gate}")
            failed = True
    ticks = text.count("Timer tick")
    if ticks > 1:
        print(f"ERROR: quiet contract broken — {ticks} timer-tick lines on the default ISO entry")
        failed = True
    else:
        print("  [PASS] quiet contract: at most one timer-tick line")
    m = re.search(r"CONS: screen console active \(framebuffer (\d+)x(\d+), (\d+)x(\d+) cells\)", text)
    if m:
        print(f"  framebuffer {m.group(1)}x{m.group(2)}, console {m.group(3)}x{m.group(4)} cells")
    print(f"  elapsed {elapsed:.1f} s, commands sent: {sent}")
    if failed:
        print("Boot log (ANSI stripped):")
        print("\n".join(l for l in text.splitlines() if l.strip()))
        print("=== UEFI Boot Test FAILED ===")
        return 1
    print("SUCCESS: UEFI/OVMF boot reached the shell + citizen gate on a framebuffer console")
    return 0


if __name__ == "__main__":
    sys.exit(main())
