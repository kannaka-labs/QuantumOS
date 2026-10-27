#!/bin/bash
# The kernel's network self-test under Firecracker, over virtio-net on virtio-mmio (#235).
#
# QEMU's user-net gives the guest a gateway, DHCP and DNS for free; a Firecracker tap gives
# nothing. This stands up the same three on a tap with dnsmasq (gateway 10.0.2.2, a lease of
# 10.0.2.15, and a DNS answer for example.com), boots the kernel, runs `nslookup example.com`
# in qsh, and checks the lines the QEMU gate (make ci-smoke-net) checks: virtio-net up over
# mmio, ARP of the gateway, the DHCP lease, the ping reply, and the DNS answer.
#
# Needs: root (sudo) for the tap, dnsmasq, firecracker, python3.
# usage: scripts/firecracker_net_selftest.sh <firecracker> <kernel.elf>
set -euo pipefail
FC=$(readlink -f "$1")
KERNEL=$(readlink -f "$2")
TAP=qosnet$$
D=$(mktemp -d)
chmod 755 "$D"          # dnsmasq drops to an unprivileged user and must write its log here
LOG=$D/console.log
cleanup() {
  [ -n "${VM:-}" ] && kill "$VM" 2>/dev/null || true
  [ -f "$D/dnsmasq.pid" ] && sudo kill "$(cat "$D/dnsmasq.pid")" 2>/dev/null || true
  sudo ip link del "$TAP" 2>/dev/null || true
  rm -rf "$D"
}
trap cleanup EXIT

sudo ip tuntap add "$TAP" mode tap user "$(id -un)"
sudo ip addr add 10.0.2.2/24 dev "$TAP"
sudo ip link set "$TAP" up
touch "$D/dnsmasq.log" && chmod 666 "$D/dnsmasq.log"
sudo dnsmasq --interface="$TAP" --bind-interfaces --except-interface=lo \
  --dhcp-range=10.0.2.15,10.0.2.15,255.255.255.0,1h --dhcp-option=3,10.0.2.2 --dhcp-option=6,10.0.2.2 \
  --address=/example.com/10.0.2.2 --no-resolv --no-hosts --pid-file="$D/dnsmasq.pid" \
  --log-facility="$D/dnsmasq.log" --log-dhcp --log-queries

cp "$KERNEL" "$D/kernel.elf"
cat > "$D/vm.json" <<EOF
{"boot-source": {"kernel_image_path": "$D/kernel.elf", "boot_args": ""},
 "drives": [],
 "machine-config": {"vcpu_count": 1, "mem_size_mib": 128},
 "network-interfaces": [{"iface_id": "eth0", "guest_mac": "02:fc:00:00:00:15", "host_dev_name": "$TAP"}]}
EOF
: > "$D/fc.log"
( sleep 12; printf 'nslookup example.com\n'; sleep 14 ) | \
  "$FC" --no-api --config-file "$D/vm.json" --log-path "$D/fc.log" --level Error > "$LOG" 2>&1 &
VM=$!
sleep 30
kill "$VM" 2>/dev/null || true
VM=

fail=0
check() {
  if grep -qF "$1" "$LOG"; then echo "OK   $1"; else echo "MISS $1"; fail=1; fi
}
check "NET: virtio-net up (mmio)"
check "NET: ARP 10.0.2.2 is at MAC"
check "NET: DHCP lease 10.0.2.15"
check "NET: ping 10.0.2.2 reply received"
check "qsh: example.com -> 10.0.2.2"   # the DNS answer dnsmasq gives, as qsh prints it
grep -aE "nslookup|qsh: example|NET: (DHCP|ARP|ping)" "$LOG" | head -12
grep -E "query|reply|DHCPACK" "$D/dnsmasq.log" 2>/dev/null | tail -6
if [ $fail -ne 0 ]; then
  echo "--- console tail"; tail -40 "$LOG"
  echo "--- dnsmasq"; tail -20 "$D/dnsmasq.log" 2>/dev/null || true
  echo "FAIL"; exit 1
fi
echo "PASS: net self-test under Firecracker over a tap (ARP, DHCP, ping, DNS)"
