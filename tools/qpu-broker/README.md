# QuantumOS host quantum broker (qBraid)

This broker lets a circuit typed inside a QuantumOS guest run on a qBraid simulator or on a
real QPU (#262). The broker runs on the host and is the only process that holds the qBraid
API key. Guests send it requests and get results back. They never see the key, and they
never have a network path to qBraid.

```
qsh> qpu run bell                         (guest, ring 3)
  │  COM1: ESC ] qpu ; S ; <tag> ; s ; <shots> ; <circuit hex> BEL
  ▼
qos-console launcher (tools/qos-console/qpu_relay.py)
  │  strips the escape from the page, checks the grammar, rate-limits the session,
  │  and adds the session id and a salted client hash (the guest cannot set them)
  ▼  Unix socket /run/qos-qpu/broker.sock (group qosqpu)
qos-qpu-broker.service (this directory)
  │  validates the circuit, applies the money rules, and picks the device at run time
  ▼  qBraid SDK (QBRAID_API_KEY from /etc/qos-qpu/broker.env, root 0600)
qBraid simulator, or a real QPU
  ▲
  └─ answer on COM1: ESC ] qres ; <tag> ; <state> ; <job> ; <text> BEL
```

The console VMs have no NIC, and their only drive is the 4 MiB save disk. COM1 is their only channel, and this is a
second use of it, alongside the session reseed (ADR-0025). On a machine with no broker
(QEMU, a laptop), nothing answers `qpu` and the command says so.

## In the shell

| Command | What it does |
|---|---|
| `qpu` | Shows what the host offers and how much of today's QPU budget is left |
| `qpu run <bell\|ghz3\|grover3> [shots]` | Runs the circuit on a free qBraid simulator and waits for the counts (about 20 s) |
| `qpu real <bell\|ghz3\|grover3> [shots]` | Queues the circuit on a real QPU and returns at once with `queued on <device>, job <id>` |
| `qpu job <id>` | Status, counts, cost and provider job id. Works from any later session |

Every answer names its source, as `qBraid simulator <device id>` or `QPU <device id>`, and
gives the broker job id, the provider's job id, the shot count, the time and the cost. The
counts are bitstrings as qBraid returns them, with qubit 0 as the rightmost character.

## Money rules (enforced here, not in the UI)

| Rule | Value on debain2 |
|---|---|
| Default device | a free simulator: online, every price field 0, enough qubits |
| Real QPUs | on (`--real`) |
| Daily budget, all visitors together (UTC day) | 10 qBraid credits (`--budget-per-day`) |
| Per-job cap, estimated before submit | 5 credits (`--max-job-credits`) |
| Real jobs per session | 1 |
| Real jobs per visitor per day | 1, keyed by the launcher's salted client hash |
| Simulator jobs | 10 per session, 500 per day |
| Qubits / ops / shots | sim up to 8 / 40 / 1000; QPU up to 5 / 40 / 100 (default 10 shots) |

- **Device choice happens at run time.** The broker reads qBraid's device list and live
  prices through the SDK, sorts the QPUs with enough qubits by estimated cost, and takes
  the cheapest one that reports `ONLINE` right then. A QPU with no price, or with every
  price at 0, is skipped. A zero price means the device is billed outside qBraid credits
  (for example `openquantum:*`), so the broker cannot bound what it costs.
- **The cost estimate** is `perTask + perShot × shots + perMinute × time`. qBraid does not
  quote execution time before a job runs, so per-minute devices use
  `time = (shots × 0.19 ms + 17 ms) × 1.2`. That model is fitted to two billed Bell
  jobs on Rigetti Cepheus-1-108Q: 100 shots billed 7.13 credits (2026-09-07), and 20
  shots billed 4.1724 credits (2026-10-24, the first console job). The first model,
  `(shots × 0.4 ms + 5 ms) × 1.5`, estimated 3.90 credits for that 20-shot job, which
  was 7% under the bill. The estimate is logged before submit. The billed cost from the finished job then replaces it in the
  day's spend.
- A job whose submit fails keeps its estimate counted against the day. Spend is never
  under-counted.
- **A refusal says what still works.** When a visitor's real job is refused (the session or
  visitor/day job is used, or today's budget is spent), the answer says the simulator is
  still free (`qpu run bell`) and, for the daily limits, how long until 00:00 UTC. A spent
  budget is refused before qBraid is asked for devices, so it never waits on the network.
- QPU jobs are asynchronous, because queues can take minutes to hours. The broker polls
  open jobs every 15 s and keeps every job in SQLite (`/var/lib/qos-qpu/broker.db`).

1 qBraid credit is $0.01, so the daily budget is $0.10.

## Daily calibration circuit (#275)

The idea came from **mira_muse** (1F916, comment c119164 on post #9955). A visitor's 20
real shots are a single reading, with nothing to compare them against. A fixed circuit
run on a schedule gives every other job a baseline.

- **What runs:** a Bell circuit (H q0, CX q0,q1, measure both), 20 shots
  (`--calib-shots`), once per UTC day at 00:05 (`qos-qpu-calibrate.timer`, a systemd
  timer, not cron). It goes through the broker's `calibrate` op. Only the host sends that
  op; the console relay's grammar has no way to send it.
- **Same rules as a visitor's job:** the same device choice (the cheapest priced QPU
  online at that moment), the same 5-credit per-job cap, and the same 10-credit daily
  budget. A job estimated over the cap is not submitted. The day is then recorded as
  `refused`, with the reason.
- **One job a day is reserved.** Until the day's calibration has been tried, visitor jobs
  may spend only the budget minus `--calib-reserve` (5 credits on debain2). At about 4.2
  credits a job, visitors get about one real job a day. A refused visitor sees
  "1 job/day is for calibration." `--calib-from` sets the first day of the series. Before
  that day nothing is held back and `calibrate` refuses.
- **At most once a day.** A second `calibrate` the same day returns that day's job, or
  repeats its refusal. It never submits again.
- **The series** (`qpu_calibrate.py`): one row per day is appended to
  `/var/lib/qos-qpu/calibration.jsonl`, which is never rewritten. The row carries date,
  state, device, series number, broker and provider job ids, shots, raw counts,
  `fraction_00_11`, and billed and estimated credits. `calibration.json` is regenerated
  from it, and the console serves it at https://qos.ninja-portal.com/calibration.json and
  as a table at /calibration.
- **A bad day is never a silent gap.** The states are:
  - `done`
  - `failed`: the provider failed or cancelled the job
  - `refused`: the cap, the budget, or no device online
  - `timeout`: still queued after 20 h; `qpu job <id>` fetches it later
  - `error`: the broker could not be reached

  Every state except `done` exits non-zero, so systemd shows the unit as failed. A day
  with no row at all, for example when the host was down, shows as `MISSING` in the
  console's table.
- **Device changes:** when the device differs from the previous row's, the row starts a
  new `series` number with `device_changed: true`. Counts from two devices are not
  comparable.
- **Honest limits:** 20 shots a day shows gross drift or a broken device, not small
  fidelity changes.
- Testing the path on the free simulator, without touching the series:
  `qpu_calibrate.py --socket /run/qos-qpu/broker.sock --series /tmp/c.jsonl --json /tmp/c.json --sim`.

## Install (debain2)

```
cd tools/qpu-broker && ./deploy/install-debain2.sh
```

The install script creates the `qosqpu` user and puts the broker in `/opt/qos-qpu`, with
a venv on a uv-managed CPython 3.12 that pins `qbraid==0.12.2` and `pyquil==5.0.0rc3` (Rigetti's
direct device takes a pyquil Program, and pyquil's wheels need Python 3.12). It writes `/etc/qos-qpu/broker.env` from the
operator's `~/.qbraid/qbraidrc` without printing the key, then enables
`qos-qpu-broker.service`. The console unit joins the `qosqpu` group and passes
`--qpu-broker /run/qos-qpu/broker.sock`.

## Tests

`python3 -m pytest -q tools/qpu-broker tools/qos-console`. qBraid is replaced by a fake
backend, so the tests need no credentials. A small exact state-vector check confirms that
the OpenQASM translation of grover3 gives p(|101>) = 121/128, the same answer as the
in-OS exact engine.
