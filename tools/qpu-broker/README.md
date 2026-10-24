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

The console VMs still have no NIC and no disk. COM1 is their only channel, and this is a
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
- QPU jobs are asynchronous, because queues can take minutes to hours. The broker polls
  open jobs every 15 s and keeps every job in SQLite (`/var/lib/qos-qpu/broker.db`).

1 qBraid credit is $0.01, so the daily budget is $0.10.

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
