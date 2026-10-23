#!/usr/bin/env bash
# Install the QuantumOS host quantum broker on the console host (debain2).
# Run from tools/qpu-broker (it also needs ../../scripts/qpu_circuit.py). Idempotent.
# The qBraid key is copied from the operator's ~/.qbraid/qbraidrc into a root-only
# EnvironmentFile without ever being printed.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
QBRAID_VERSION=0.12.2
PYQASM_VERSION=1.0.4

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
    sudo python3 -m venv /opt/qos-qpu/venv
fi
sudo /opt/qos-qpu/venv/bin/pip install -q "qbraid==$QBRAID_VERSION" "pyqasm==$PYQASM_VERSION"

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
