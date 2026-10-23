#!/usr/bin/env bash
# Install the QuantumOS host quantum broker on the console host (debain2).
# Run from tools/qpu-broker (it also needs ../../scripts/qpu_circuit.py). Idempotent.
# The qBraid key is copied from the operator's ~/.qbraid/qbraidrc into a root-only
# EnvironmentFile without ever being printed.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
# Rigetti's direct device takes a pyquil Program, and pyquil's quil wheels need Python
# 3.12 (no 3.13 wheel), so the venv uses a uv-managed CPython 3.12 under /opt/qos-qpu.
PINS="qbraid==0.12.2 pyqasm==1.0.4 pyquil==5.0.0rc3 qiskit==2.5.2"
UV="${UV:-$(command -v uv || echo "$HOME/.local/bin/uv")}"

getent group qosqpu >/dev/null || sudo groupadd --system qosqpu
id qosqpu >/dev/null 2>&1 || sudo useradd --system --gid qosqpu --home-dir /nonexistent \
    --no-create-home --shell /usr/sbin/nologin qosqpu
# The console launcher reaches the broker's socket through this group.
id qosconsole >/dev/null 2>&1 && sudo usermod -aG qosqpu qosconsole

sudo install -d -m 755 -o root -g root /opt/qos-qpu
sudo install -m 644 -o root -g root "$SRC/qpu_broker.py" /opt/qos-qpu/qpu_broker.py
sudo install -m 644 -o root -g root "$SRC/../../scripts/qpu_circuit.py" /opt/qos-qpu/qpu_circuit.py
sudo install -m 644 -o root -g root "$SRC/README.md" /opt/qos-qpu/README.md
if [ ! -x /opt/qos-qpu/venv/bin/python ]; then
    sudo env UV_PYTHON_INSTALL_DIR=/opt/qos-qpu/python "$UV" python install 3.12
    PY="$(sudo env UV_PYTHON_INSTALL_DIR=/opt/qos-qpu/python "$UV" python find --managed-python 3.12)"
    sudo "$UV" venv -q --python "$PY" /opt/qos-qpu/venv
fi
# shellcheck disable=SC2086
sudo "$UV" pip install -q --python /opt/qos-qpu/venv/bin/python $PINS
sudo chmod -R a+rX /opt/qos-qpu

sudo install -d -m 700 -o root -g root /etc/qos-qpu
if ! sudo test -s /etc/qos-qpu/broker.env; then
    rc="${QBRAIDRC:-$HOME/.qbraid/qbraidrc}"
    key="$(sed -n 's/^api-key *= *//p' "$rc" | head -1)"
    [ -n "$key" ] || { echo "no api-key in $rc" >&2; exit 1; }
    printf 'QBRAID_API_KEY=%s\n' "$key" | sudo tee /etc/qos-qpu/broker.env >/dev/null
    unset key
fi
sudo chown root:root /etc/qos-qpu/broker.env
sudo chmod 600 /etc/qos-qpu/broker.env

sudo install -m 644 -o root -g root "$SRC/deploy/qos-qpu-broker.service" /etc/systemd/system/qos-qpu-broker.service
sudo systemctl daemon-reload
sudo systemctl enable --now qos-qpu-broker.service
sudo systemctl restart qos-qpu-broker.service
sleep 2
systemctl is-active qos-qpu-broker.service
ls -l /run/qos-qpu/broker.sock
