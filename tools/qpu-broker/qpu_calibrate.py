#!/usr/bin/env python3
"""Daily QPU calibration run for the QuantumOS console's quantum broker (#275).

Idea from mira_muse (1F916, c119164 on post #9955): a visitor's 20 real shots are a single
reading with nothing to compare against. One fixed circuit, run on a schedule with the
same shots and its counts published, gives every other job a baseline.

Once a day (qos-qpu-calibrate.timer, 00:05 UTC) this asks the broker to run its fixed
calibration circuit (`{"op": "calibrate"}`: Bell, --calib-shots on the broker), waits for
the result, and appends exactly one row for the day to an append-only series:

  --series  calibration.jsonl  one JSON row per line, only ever appended to
  --json    calibration.json   the whole series as one document, rewritten from the
                               JSONL after each run (the console serves this file)

The job goes through the broker's normal device choice (cheapest priced QPU online now),
per-job cap and daily budget, so it can be refused. Every outcome writes a row, so a day
is never a silent gap:

  done       counts, billed credits and fraction of 00+11 recorded
  failed     the provider failed or cancelled the job
  refused    the broker would not submit it (over the per-job cap, no device online,
             budget spent); the reason is in the row and nothing was billed
  timeout    still queued at the deadline; the job ids are recorded, and `qpu job <id>`
             fetches the result later
  error      the broker could not be reached or answered nonsense

Exit status is 0 only for `done`, so a bad day also shows as a failed unit in systemd.
The day's device is recorded in each row; when it differs from the previous row's
device, the row starts a new series number (device_changed: true), because counts from
two devices are not comparable.

  qpu_calibrate.py --socket /run/qos-qpu/broker.sock \
      --series /var/lib/qos-qpu/calibration.jsonl --json /var/lib/qos-qpu/calibration.json
  qpu_calibrate.py ... --sim     # the same path on the free simulator, for testing
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path

OPEN = ("submitting", "queued", "running")
ABOUT = {
    "what": "Daily calibration of the QPU behind the QuantumOS public console (qos.ninja-portal.com)",
    "circuit": "Bell: H q0; CX q0,q1; measure both. Ideal counts are only 00 and 11.",
    "schedule": "once per UTC day at 00:05, through the console's quantum broker and its "
                "10-credit daily budget (one job a day is reserved for this)",
    "fraction_00_11": "(count of 00 + count of 11) / shots; 1.0 is ideal, 0.5 is noise",
    "series": "a new series number starts when the device changes; compare rows within a series",
    "limits": "20 shots a day shows gross drift or a broken device, not small fidelity changes",
    "idea": "mira_muse (1F916 citizen), comment c119164 on post #9955",
    "issue": "https://github.com/kannaka-labs/QuantumOS/issues/275",
    "bit_order": "qBraid bitstrings, qubit 0 rightmost",
}


def utc_day(t: float | None = None) -> str:
    return time.strftime("%Y-%m-%d", time.gmtime(t if t is not None else time.time()))


def ask(sock_path: str, req: dict, timeout: float = 300.0) -> dict:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(sock_path)
        s.sendall(json.dumps(req).encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    ans = json.loads(buf)
    if not isinstance(ans, dict):
        raise ValueError("broker answer is not an object")
    return ans


def load(series: Path) -> list[dict]:
    if not series.exists():
        return []
    rows = []
    for line in series.read_text(encoding="utf-8").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def fraction(counts: dict | None) -> float | None:
    if not counts:
        return None
    total = sum(counts.values())
    return round((counts.get("00", 0) + counts.get("11", 0)) / total, 4) if total else None


def make_row(day: str, state: str, job: dict | None, prev: list[dict], reason: str | None = None) -> dict:
    job = job or {}
    device = job.get("device")
    last = next((r for r in reversed(prev) if r.get("device")), None)
    series_no = last["series"] if last else 0
    changed = False
    if device and (last is None or last["device"] != device):
        series_no += 1
        changed = last is not None
    counts = job.get("counts")
    return {
        "date": day, "state": state, "device": device,
        "series": series_no if device else (last["series"] if last else None),
        "device_changed": changed,
        "broker_job": job.get("job"), "provider_job": job.get("provider_job"),
        "shots": job.get("shots"), "counts": counts, "fraction_00_11": fraction(counts),
        "credits_billed": job.get("cost") if job.get("cost_source") == "billed" else None,
        "credits_estimated": job.get("est_cost"),
        "submitted": job.get("submitted"), "finished": job.get("finished"),
        "reason": reason if reason is not None else job.get("detail"),
        "recorded": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }


def append(series: Path, row: dict) -> None:
    """Append one line and fsync it: the JSONL is the record, and is never rewritten."""
    series.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(series, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o640)
    try:
        os.write(fd, (json.dumps(row, sort_keys=True) + "\n").encode())
        os.fsync(fd)
    finally:
        os.close(fd)


def publish(series: Path, out: Path | None) -> None:
    if out is None:
        return
    doc = dict(ABOUT, rows=load(series))
    tmp = out.with_name(out.name + ".tmp")
    tmp.write_text(json.dumps(doc, indent=1, sort_keys=False) + "\n", encoding="utf-8")
    os.chmod(tmp, 0o640)
    os.replace(tmp, out)


def run(a: argparse.Namespace, sleep=time.sleep, clock=time.time) -> int:
    series = Path(a.series)
    out = Path(a.json) if a.json else None
    rows = load(series)
    day = utc_day(clock())
    if not a.sim and any(r.get("date") == day for r in rows):
        print(f"calibration {day}: already recorded, nothing to do")
        publish(series, out)
        return 0 if next(r for r in rows if r.get("date") == day)["state"] == "done" else 1

    def finish(state: str, job: dict | None, reason: str | None = None) -> int:
        row = make_row(day, state, job, rows, reason)
        append(series, row)
        publish(series, out)
        print(f"calibration {day}: {json.dumps(row, sort_keys=True)}", flush=True)
        return 0 if state == "done" else 1

    try:
        ans = ask(a.socket, {"op": "calibrate", "sim": bool(a.sim)})
    except Exception as e:  # noqa: BLE001 - recorded, and the unit fails
        return finish("error", None, f"broker unreachable: {type(e).__name__}: {str(e)[:200]}")
    if not ans.get("ok"):
        return finish("refused", None, str(ans.get("error", "refused"))[:300])
    job = ans
    deadline = clock() + a.deadline_hours * 3600
    while job.get("state") in OPEN:
        if clock() >= deadline:
            return finish("timeout", job, f"still {job.get('state')} after {a.deadline_hours:g} h; "
                                          f"`qpu job {job.get('job')}` fetches it later")
        sleep(a.poll_seconds)
        try:
            nxt = ask(a.socket, {"op": "job", "id": job["job"]})
        except Exception as e:  # noqa: BLE001 - keep waiting; the deadline bounds this
            print(f"poll failed: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
            continue
        if nxt.get("ok"):
            job = nxt
    if job.get("state") == "done":
        return finish("done", job)
    return finish(job.get("state") or "error", job)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", required=True)
    ap.add_argument("--series", required=True, help="append-only JSONL record")
    ap.add_argument("--json", help="the whole series as one JSON document (what the console serves)")
    ap.add_argument("--sim", action="store_true", help="run on the free simulator (testing the path)")
    ap.add_argument("--poll-seconds", type=float, default=30.0)
    ap.add_argument("--deadline-hours", type=float, default=20.0)
    return ap


def main(argv=None) -> int:
    return run(build_parser().parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
