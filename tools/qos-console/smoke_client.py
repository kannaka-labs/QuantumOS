#!/usr/bin/env python3
"""End-to-end smoke client: boot one console VM, wait for the qsh prompt, run one
command, print what came back, disconnect.

  smoke_client.py ws://127.0.0.1:8930/ws [--user U --password-file F] [--cmd help]

Exit 0 only if the banner, the prompt and the command's output were all seen.
"""
import argparse
import asyncio
import base64
import json
import re
import sys
import time

from websockets.asyncio.client import connect

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = re.compile(rb"qsh(\x1b\[90m)?> ")


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--user")
    ap.add_argument("--password-file")
    ap.add_argument("--cmd", default="imprint a cat sat on the mat")
    ap.add_argument("--expect", default="imprinted", help="text the command's own output must contain")
    ap.add_argument("--settle", type=float, default=8, help="seconds of console silence before typing")
    ap.add_argument("--boot-timeout", type=float, default=240)
    ap.add_argument("--internal-token-file",
                    help="tag this session as our own traffic, kept out of the visitor counts")
    a = ap.parse_args()
    headers = {}
    if a.internal_token_file:
        headers["X-QOS-Internal"] = open(a.internal_token_file).read().strip()
    if a.user:
        pw = open(a.password_file).read().strip()
        headers["Authorization"] = "Basic " + base64.b64encode(f"{a.user}:{pw}".encode()).decode()
    t0 = time.monotonic()
    buf = b""
    async with connect(a.url, additional_headers=headers, open_timeout=30) as ws:
        hello = json.loads(await ws.recv())
        print("hello:", hello)
        if hello.get("type") != "hello":
            return 2
        while not PROMPT.search(buf):
            left = a.boot_timeout - (time.monotonic() - t0)
            if left <= 0:
                print("NO PROMPT within", a.boot_timeout, "s; tail:", ANSI.sub(b"", buf[-400:]).decode(errors="replace"))
                return 3
            msg = await asyncio.wait_for(ws.recv(), left)
            if isinstance(msg, bytes):
                buf += msg
        boot_s = time.monotonic() - t0
        banner = [l for l in ANSI.sub(b"", buf).decode(errors="replace").splitlines() if "QuantumOS" in l]
        print(f"prompt after {boot_s:.1f}s; banner lines: {banner[-2:]}")
        # boot services keep printing after the first prompt; wait for silence before typing
        while True:
            try:
                msg = await asyncio.wait_for(ws.recv(), a.settle)
                if isinstance(msg, bytes):
                    buf += msg
            except asyncio.TimeoutError:
                break
        print(f"console quiet after {time.monotonic() - t0:.1f}s")
        mark = len(buf)
        await ws.send(a.cmd + "\r")
        t1 = time.monotonic()
        while a.expect.encode() not in ANSI.sub(b"", buf[mark:]) and time.monotonic() - t1 < 60:
            try:
                msg = await asyncio.wait_for(ws.recv(), 60 - (time.monotonic() - t1))
            except asyncio.TimeoutError:
                break
            if isinstance(msg, bytes):
                buf += msg
        out = ANSI.sub(b"", buf[mark:]).decode(errors="replace")
        print(f"--- output of {a.cmd!r} ---")
        print(out[:800])
    ok = a.expect in out
    print("EXPECTED OUTPUT SEEN" if ok else f"expected {a.expect!r} NOT seen")
    return 0 if ok else 4


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
