#!/usr/bin/env python3
"""TCP sink for the QuantumOS send-side gate (ci-smoke-tcp-put).

Accepts ONE connection, reads it to EOF, and checks that byte k of the
stream is 0x20 + (k % 95) -- the pattern `qsh tcpput` pushes -- so a
stack that reorders, duplicates or drops a segment under a windowed send
fails on content, not just on the byte count. Prints exactly one verdict
line (grep-able by the gate) and exits 0 on success, 1 otherwise:

    SINK: <n> bytes ok in <ms> ms          host clock, first byte to EOF
    SINK: ERROR <reason>

Stdlib only: the CI runner has python3 and nothing else.
"""

import argparse
import socket
import sys
import time

PAT = bytes(0x20 + (k % 95) for k in range(95))


def expected(offset, length):
    """The pattern bytes for stream offsets [offset, offset + length)."""
    start = offset % 95
    reps = length // 95 + 2
    return (PAT[start:] + PAT * reps)[:length]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--bytes", type=int, required=True, help="expected stream length")
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the connection")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(1)
    srv.settimeout(args.timeout)
    print(f"SINK: listening on {args.bind}:{args.port} for {args.bytes} bytes", flush=True)
    try:
        conn, peer = srv.accept()
    except socket.timeout:
        print("SINK: ERROR no connection within the timeout", flush=True)
        return 1
    conn.settimeout(args.timeout)
    print(f"SINK: connection from {peer[0]}:{peer[1]}", flush=True)

    got = 0
    t_first = None
    try:
        while True:
            chunk = conn.recv(65536)
            if not chunk:
                break
            if t_first is None:
                t_first = time.monotonic()
            want = expected(got, len(chunk))
            if chunk != want:
                bad = next(i for i in range(len(chunk)) if chunk[i] != want[i])
                print(f"SINK: ERROR pattern mismatch at offset {got + bad} "
                      f"(got 0x{chunk[bad]:02x}, want 0x{want[bad]:02x})", flush=True)
                return 1
            got += len(chunk)
    except socket.timeout:
        print(f"SINK: ERROR stream stalled after {got} bytes", flush=True)
        return 1
    finally:
        conn.close()
        srv.close()
    t_end = time.monotonic()

    if got != args.bytes:
        print(f"SINK: ERROR got {got} bytes, expected {args.bytes}", flush=True)
        return 1
    ms = int((t_end - t_first) * 1000) if t_first is not None else 0
    print(f"SINK: {got} bytes ok in {ms} ms", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
