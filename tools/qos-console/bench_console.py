#!/usr/bin/env python3
"""Timing harness for the console: the numbers in README.md "Measured timings".

  bench_console.py ws://127.0.0.1:8931/ws --runs 10 [--keys 40] [--json out.json]

One run = one WebSocket session, measured on the client's monotonic clock:
  connect_to_hello   WebSocket open -> the launcher's "hello" (VM restored or spawned)
  first_output      WebSocket open -> the first guest byte
  to_prompt         WebSocket open -> the first "qsh> " prompt (ANSI colour stripped)
  echo              one key -> its echo, for --keys keys (alternating "x" and DEL, after
                    the console has been quiet for --settle seconds)
  help_bytes_per_s  bytes of `help` output / time from Enter to the next prompt

Runs are sequential with a pause between them (the console allows one VM per address),
and the summary prints the median over runs, plus p50/p90 over all echo samples.
Nothing typed here is secret, and the launcher never logs it anyway.
"""
import argparse
import asyncio
import json
import re
import statistics
import sys
import time

from websockets.asyncio.client import connect

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
PROMPT = b"qsh> "


class Session:
    def __init__(self, ws):
        self.ws = ws
        self.buf = b""

    async def until(self, pred, timeout: float) -> float:
        """Read until pred(clean buffer) is true; return the monotonic time it became true."""
        end = time.monotonic() + timeout
        while not pred(ANSI.sub(b"", self.buf)):
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError(ANSI.sub(b"", self.buf[-300:]).decode(errors="replace"))
            msg = await asyncio.wait_for(self.ws.recv(), left)
            if isinstance(msg, bytes):
                self.buf += msg
        return time.monotonic()

    async def quiet(self, seconds: float) -> None:
        while True:
            try:
                msg = await asyncio.wait_for(self.ws.recv(), seconds)
            except asyncio.TimeoutError:
                return
            if isinstance(msg, bytes):
                self.buf += msg


async def one_run(url: str, keys: int, settle: float, boot_timeout: float, headers: dict | None = None) -> dict:
    r: dict = {}
    t0 = time.monotonic()
    async with connect(url, open_timeout=30, max_size=None, additional_headers=headers or {}) as ws:
        t_open = time.monotonic()
        r["ws_open"] = t_open - t0
        s = Session(ws)
        while True:
            msg = await asyncio.wait_for(ws.recv(), boot_timeout)
            if isinstance(msg, str):
                hello = json.loads(msg)
                if hello.get("type") != "hello":
                    raise RuntimeError(f"not admitted: {hello}")
                r["connect_to_hello"] = time.monotonic() - t_open
                r["restored"] = bool(hello.get("restored"))
                break
        await s.until(lambda b: bool(s.buf), boot_timeout)
        r["first_output"] = time.monotonic() - t_open
        r["to_prompt"] = await s.until(lambda b: PROMPT in b, boot_timeout) - t_open
        await s.quiet(settle)

        echoes = []
        for i in range(keys):
            key, want = (b"x", b"x") if i % 2 == 0 else (b"\x7f", b"\b \b")
            mark = len(s.buf)
            t = time.monotonic()
            await ws.send(key)
            await s.until(lambda b, m=mark, w=want: w in s.buf[m:], 10)
            echoes.append(time.monotonic() - t)
        r["echo"] = echoes

        await s.quiet(0.5)
        mark = len(s.buf)
        t = time.monotonic()
        await ws.send(b"help\r")

        def help_done(_b, m=mark):
            out = ANSI.sub(b"", s.buf[m:])
            return b"qsh commands:" in out and out.rstrip().endswith(b"qsh>")
        await s.until(help_done, 60)
        dt = time.monotonic() - t
        n = len(s.buf) - mark
        r["help_bytes"] = n
        r["help_seconds"] = dt
        r["help_bytes_per_s"] = n / dt
    return r


def pct(vals, p):
    vals = sorted(vals)
    return vals[min(len(vals) - 1, int(p * len(vals)))]


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("url")
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--keys", type=int, default=40)
    ap.add_argument("--settle", type=float, default=3)
    ap.add_argument("--pause", type=float, default=11, help="seconds between runs (nginx allows 6 boots/min)")
    ap.add_argument("--boot-timeout", type=float, default=240)
    ap.add_argument("--json")
    ap.add_argument("--internal-token-file",
                    help="tag these sessions as our own traffic, kept out of the visitor counts")
    a = ap.parse_args()
    headers = {"X-QOS-Internal": open(a.internal_token_file).read().strip()} if a.internal_token_file else {}
    runs = []
    for i in range(a.runs):
        if i:
            await asyncio.sleep(a.pause)
        r = await one_run(a.url, a.keys, a.settle, a.boot_timeout, headers)
        runs.append(r)
        print(f"run {i + 1}: restored={r['restored']} hello={r['connect_to_hello'] * 1000:.0f} ms "
              f"first_output={r['first_output'] * 1000:.0f} ms prompt={r['to_prompt'] * 1000:.0f} ms "
              f"echo_p50={statistics.median(r['echo']) * 1000:.1f} ms "
              f"help={r['help_bytes']} B in {r['help_seconds']:.2f} s", flush=True)
    echo = [e for r in runs for e in r["echo"]]
    summary = {
        "runs": len(runs),
        "restored": sorted({r["restored"] for r in runs}),
        "connect_to_hello_ms_median": round(statistics.median(r["connect_to_hello"] for r in runs) * 1000, 1),
        "first_output_ms_median": round(statistics.median(r["first_output"] for r in runs) * 1000, 1),
        "to_prompt_ms_median": round(statistics.median(r["to_prompt"] for r in runs) * 1000, 1),
        "to_prompt_ms_max": round(max(r["to_prompt"] for r in runs) * 1000, 1),
        "echo_samples": len(echo),
        "echo_ms_p50": round(pct(echo, 0.5) * 1000, 1),
        "echo_ms_p90": round(pct(echo, 0.9) * 1000, 1),
        "help_bytes_median": statistics.median(r["help_bytes"] for r in runs),
        "help_bytes_per_s_median": round(statistics.median(r["help_bytes_per_s"] for r in runs)),
    }
    print(json.dumps(summary, indent=1))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"url": a.url, "summary": summary, "runs": runs}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
