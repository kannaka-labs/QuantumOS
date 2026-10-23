#!/usr/bin/env bash
# Install the QuantumOS console launcher + tunnel on the KVM host (debain2).
# Run from a directory holding qos_console.py, deploy/*.service and README.md.
# Idempotent. Prints the tunnel public key for O1's authorized_keys at the end.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
O1=170.9.238.136

sudo apt-get install -y -q python3-websockets >/dev/null
getent group qosconsole >/dev/null || sudo groupadd --system qosconsole
id qosconsole >/dev/null 2>&1 || sudo useradd --system --gid qosconsole --groups kvm \
    --home-dir /nonexistent --no-create-home --shell /usr/sbin/nologin qosconsole
sudo usermod -aG kvm qosconsole
# The host quantum broker (tools/qpu-broker) owns this group; its installer adds us too.
getent group qosqpu >/dev/null || sudo groupadd --system qosqpu
sudo usermod -aG qosqpu qosconsole

sudo install -d -m 755 -o root -g root /opt/qos-console
sudo install -m 644 -o root -g root "$SRC/qos_console.py" /opt/qos-console/qos_console.py
sudo install -m 644 -o root -g root "$SRC/qpu_relay.py" /opt/qos-console/qpu_relay.py
sudo install -m 644 -o root -g root "$SRC/README.md" /opt/qos-console/README.md
# The kernel the production microVMs already run (docs/FIRECRACKER.md: ELF64, PVH entry).
sudo install -m 644 -o root -g root /srv/qos/kernel.elf /opt/qos-console/kernel.elf

sudo install -d -m 750 -o qosconsole -g qosconsole /etc/qos-console
if ! sudo test -f /etc/qos-console/tunnel_ed25519; then
    sudo -u qosconsole ssh-keygen -q -t ed25519 -N "" -C "qos-console-tunnel@debain2" -f /etc/qos-console/tunnel_ed25519
fi
# Pin O1's host keys (fetched over the network, then checked by the caller against
# the fingerprints O1 reports locally before the tunnel is trusted).
ssh-keyscan -t ed25519,ecdsa,rsa "$O1" 2>/dev/null | sudo tee /etc/qos-console/known_hosts.new >/dev/null
sudo chown qosconsole:qosconsole /etc/qos-console/known_hosts.new
sudo chmod 644 /etc/qos-console/known_hosts.new

sudo install -m 644 -o root -g root "$SRC/deploy/qos-console.service" /etc/systemd/system/qos-console.service
sudo install -m 644 -o root -g root "$SRC/deploy/qos-console-tunnel.service" /etc/systemd/system/qos-console-tunnel.service
sudo systemctl daemon-reload
echo "PUBKEY: $(sudo cat /etc/qos-console/tunnel_ed25519.pub)"
echo "KEYSCAN fingerprints:"; ssh-keygen -lf /etc/qos-console/known_hosts.new
