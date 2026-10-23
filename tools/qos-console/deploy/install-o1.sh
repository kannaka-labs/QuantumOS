#!/usr/bin/env bash
# Install the O1 (public edge) side of the QuantumOS console. Idempotent.
#   install-o1.sh "<tunnel public key line>"
# Run from the unpacked tools/qos-console directory. Prints no secret.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"
PUB="$1"
[[ "$PUB" == ssh-ed25519\ * ]] || { echo "need the tunnel's ssh-ed25519 public key" >&2; exit 2; }

# 1. Tunnel user: may only listen on 127.0.0.1:8930, may run nothing.
id qostunnel >/dev/null 2>&1 || sudo useradd --system --create-home --home-dir /home/qostunnel \
    --shell /sbin/nologin qostunnel
sudo install -d -m 700 -o qostunnel -g qostunnel /home/qostunnel/.ssh
echo "restrict,port-forwarding,permitlisten=\"127.0.0.1:8930\",command=\"/bin/false\" $PUB" \
    | sudo tee /home/qostunnel/.ssh/authorized_keys >/dev/null
sudo chown qostunnel:qostunnel /home/qostunnel/.ssh/authorized_keys
sudo chmod 600 /home/qostunnel/.ssh/authorized_keys
command -v restorecon >/dev/null && sudo restorecon -R /home/qostunnel/.ssh || true

# 2. Credentials: plaintext in the secrets dir, hash beside nginx. Generated once.
if ! sudo test -f /etc/kannaka-secrets/qos-console.env; then
    PW="$(head -c 18 /dev/urandom | base64 | tr -d '/+=' | head -c 20)"
    printf 'QOS_CONSOLE_USER=qos\nQOS_CONSOLE_PASSWORD=%s\n' "$PW" | sudo tee /etc/kannaka-secrets/qos-console.env >/dev/null
    sudo chmod 600 /etc/kannaka-secrets/qos-console.env
    HASH="$(printf '%s' "$PW" | openssl passwd -apr1 -stdin)"
    printf 'qos:%s\n' "$HASH" | sudo tee /etc/nginx/qos-console.htpasswd >/dev/null
    unset PW HASH
fi
sudo chown root:nginx /etc/nginx/qos-console.htpasswd
sudo chmod 640 /etc/nginx/qos-console.htpasswd

# 3. Static page.
sudo install -d -m 755 /usr/share/nginx/html/qos-console
sudo install -m 644 "$SRC/web/index.html" /usr/share/nginx/html/qos-console/index.html
command -v restorecon >/dev/null && sudo restorecon -R /usr/share/nginx/html/qos-console || true

# 4. nginx: zones (http context) + locations inside the staff server block.
cat <<'ZONES' | sudo tee /etc/nginx/conf.d/qos-console-zones.conf >/dev/null
limit_req_zone  $binary_remote_addr zone=qos_page:10m rate=5r/s;
limit_req_zone  $binary_remote_addr zone=qos_boot:10m rate=6r/m;
limit_conn_zone $binary_remote_addr zone=qos_conn:10m;
ZONES
sudo install -m 644 "$SRC/deploy/nginx-qos-console.conf" /etc/nginx/qos-console.locations
STAFF=/etc/nginx/conf.d/staff.conf
if ! sudo grep -q "include /etc/nginx/qos-console.locations;" "$STAFF"; then
    sudo cp -a "$STAFF" "$STAFF.bak-qos-$(date -u +%Y%m%dT%H%M%SZ)"
    # Insert the include right after the first client_max_body_size line of the https server.
    sudo python3 - "$STAFF" <<'PY'
import sys
p = sys.argv[1]
t = open(p).read()
anchor = "client_max_body_size 64k;"
assert t.count(anchor) == 1, "anchor not unique"
t = t.replace(anchor, anchor + "\n\n    include /etc/nginx/qos-console.locations;", 1)
open(p, "w").write(t)
PY
fi
sudo nginx -t
sudo systemctl reload nginx
echo "O1 side installed"
