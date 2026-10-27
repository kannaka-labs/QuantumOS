#!/usr/bin/env python3
"""qBraid gateway backend with the SDK mocked (#262): runs in CI without a key.

`qsv_gateway._probs_qbraid` translates the wire format to OpenQASM 2, runs it on a
qBraid simulator and maps the returned counts back to the exact engine's order
(qubit 0 = least significant bit). This replaces `qbraid.runtime` with a fake whose
simulator returns counts built from the exact engine's own probabilities, written
the way qBraid writes bitstrings (qubit 0 LAST), and checks:

  1. the gateway's probabilities equal the exact engine's, for a Grover circuit
     whose target (6 = 0b110) is not a bit palindrome, so a reversed mapping fails;
  2. the gateway hands the simulator valid OpenQASM 2 that measures every qubit;
  3. a real QPU device is refused with PermissionError before anything runs.

  python3 scripts/test_qsv_qbraid_mock.py
"""
import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qpu_circuit as qc  # noqa: E402
import qsv_gateway  # noqa: E402
import qsv_mirror  # noqa: E402

# Grover-3 with one iteration has probabilities 25/32 and 1/32: whole shots at 3200.
SHOTS = 3200


class _Result:
    def __init__(self, counts):
        self.data = types.SimpleNamespace(get_counts=lambda: counts)


class _Job:
    def __init__(self, counts):
        self._counts = counts

    def result(self):
        return _Result(self._counts)


class _Device:
    def __init__(self, dev_id, simulator, counts):
        self.id = dev_id
        self.profile = types.SimpleNamespace(simulator=simulator)
        self._counts = counts
        self.runs = []

    def run(self, program, shots):
        self.runs.append((program, shots))
        return _Job(self._counts)


def _install_fake_sdk(device):
    runtime = types.ModuleType("qbraid.runtime")

    class QbraidProvider:
        def get_device(self, dev_id):
            assert dev_id == device.id, f"gateway asked for {dev_id!r}, want {device.id!r}"
            return device

    runtime.QbraidProvider = QbraidProvider
    pkg = types.ModuleType("qbraid")
    pkg.runtime = runtime
    sys.modules["qbraid"] = pkg
    sys.modules["qbraid.runtime"] = runtime


def _qbraid_counts(n, exact):
    """Counts with exactly SHOTS shots, in qBraid's bitstring order (qubit 0 last)."""
    counts = {}
    for idx, p in enumerate(exact):
        c = int(p * SHOTS)
        if c:
            counts[format(idx, f"0{n}b")] = c
    assert sum(counts.values()) == SHOTS, "pick a circuit whose probabilities are multiples of 1/SHOTS"
    return counts


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    # Grover over 3 qubits, target 6, one iteration: P(6) = 25/32 and 1/32 elsewhere.
    circuit = qc.grover(3, 6, 1)
    n, _probe, exact = qsv_mirror.run_bytes(circuit)
    if exact[6] != max(exact) or exact[3] == exact[6]:
        fail("test circuit is not asymmetric enough to catch a reversed bit order")

    dev = _Device("qbraid:qbraid:sim:qir-sv", simulator=True, counts=_qbraid_counts(n, exact))
    _install_fake_sdk(dev)
    got = qsv_gateway._probs_qbraid(circuit, "qbraid:" + dev.id, shots=SHOTS)
    want = [float(p) for p in exact]
    if len(got) != len(want) or any(abs(a - b) > 1e-12 for a, b in zip(got, want)):
        fail(f"probabilities differ from the exact engine:\n got  {got}\n want {want}")
    print(f"OK: gateway probabilities equal the exact engine (P(6)={got[6]:.5f}, qubit 0 = LSB)")

    program, shots = dev.runs[0]
    measured = "measure q -> c;" in program or program.count("measure q[") >= n
    if shots != SHOTS or "OPENQASM 2.0" not in program or not measured:
        fail(f"simulator got shots={shots} and an unexpected program:\n{program}")
    print("OK: the simulator received OpenQASM 2 measuring every qubit")

    qpu = _Device("rigetti:rigetti:qpu:cepheus-1-108q", simulator=False, counts={"0": 1})
    _install_fake_sdk(qpu)
    try:
        qsv_gateway._probs_qbraid(qc.bell(), "qbraid:" + qpu.id, shots=10)
        fail("a real QPU was accepted by the gateway")
    except PermissionError:
        pass
    if qpu.runs:
        fail("the gateway ran a job on a real QPU before refusing it")
    print("OK: a real QPU is refused before anything runs (QPU jobs go through the broker)")

    print("=== qBraid gateway mock test PASSED ===")


if __name__ == "__main__":
    main()
