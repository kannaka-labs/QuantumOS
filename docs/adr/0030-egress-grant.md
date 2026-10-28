# 30. Networking Is an Egress Grant, Not a Side Effect of a NIC

Date: 2026-10-28
Status: Accepted (2026-10-28, Nick: "accept the ADR and build PR A"; principle accepted 2026-10-24 from nak_nanaz's proposal on 1F916; #252)

## Context

Today a bound NIC *is* networking. `netdev_init` binds the first present driver, `net_init`
then ARPs the gateway, takes a DHCP lease and resolves names, and every citizen holding
`CAP_RESOURCE_DEVICE` over `DEVICE_ID_NET` can open UDP and TCP sockets to anywhere. The
only reason the public console has no egress is that its microVMs have no NIC.

#252 asks for an e1000 driver so QuantumOS has a network on hypervisors and clouds that
emulate one (OCI's emulated mode, VirtualBox, QEMU's default `-nic`). With the current
design, shipping that driver would change QuantumOS's claim from "isolated compute" to
"arbitrary egress wherever a NIC exists". nak_nanaz's proposal, which Nick accepted in
principle: keep the default offline, make networking an explicit, short-lived, bounded,
receipted grant, and benchmark drivers separately from policy. #256 made the same move for
disks: disposable by default, named persistence as a separate grant.

## Decision

**A NIC without a grant is a NIC with nothing to do.** The driver binds (so probing is
testable and the boot log names the hardware), but the stack stays down: no ARP, no DHCP,
no DNS, no frame leaves the machine. `NET: e1000 present, no egress grant: networking off`.
This is the default on every path, including static `ip=` mode and every CI gate.

### The grant

One record, from one of two sources, with one grammar:

```
egress=ttl=600;bytes=4194304;allow=10.0.2.2:53/udp,example.com:80/tcp,93.184.216.34:443/tcp;listen=
```

- **Boot command line** `egress=<spec>`: cold boots, CI, bare metal.
- **COM1 escape** `ESC ] egress ; <spec> BEL`: a host granting a restored VM (ADR-0025's
  reseed path). The kernel consumes it before the shell sees it, as it does the reseed.

Fields:
- `ttl`: seconds from activation, from the timer. After it, the stack quiesces.
- `bytes`: total frame bytes, transmitted plus received, including infrastructure traffic.
  After it, the stack quiesces.
- `allow`: destinations a citizen may send to, `ip|host:port/proto`. A host entry is matched
  against the kernel's own DNS answer for that name, so a citizen cannot reach an IP by
  resolving a different name.
- `listen`: local ports inbound connections may be accepted on; empty by default. `httpd`
  is a listener and gets nothing without it.

A second grant replaces the first and is recorded as a new grant. Neither source can widen a
grant a citizen already holds: grants come from the host, never from ring 3.

### Infrastructure traffic

While a grant is active, the stack may ARP the gateway, take a DHCP lease, answer ICMP echo
and query the resolver DHCP named (or SLIRP's) on 53/udp. That traffic is not in the
allowlist because the stack cannot work without it; it is counted against `bytes` and
recorded in the receipt under `infra`. Outside a grant none of it happens.

### Enforcement

One function, `egress_check(pid, dst_ip, dst_port, proto, len)`, at the three places ring-3
traffic reaches the wire: the UDP send path (`net_udp.c`), the TCP connect and segment path
(`net_tcp.c`), and `dns_resolve` for host allowlist entries. It returns allow or a reason
(`no grant`, `expired`, `budget`, `destination`, `port`), and the syscall surfaces the reason
as the error. Inbound: `net_tcp.c` accepts only on `listen` ports; everything else is dropped
and counted. Budgets are decremented in the driver's transmit and receive paths, so they see
every byte, not only the ones policy looked at.

### The receipt

A new audit kind, `AUDIT_EGRESS`, in the kernel's existing ledger: one row per
`(pid, destination, port, proto, verdict)` with a count and a byte total, plus one row per
grant (`granted`, `expired`, `budget`). The kernel writes it, so a citizen cannot forge or
suppress its own row, the property the ledger already has for capabilities. `qsh egress`
prints attempted against allowed; the existing `audit` command and `verify_attestation.py`
path carry it off the machine. The ledger holds 256 entries, so rows aggregate by key; a
flood is a count, not an overflow.

### Drivers are separate from policy

A driver PR (#252's e1000, #253's virtio-net offloads) touches `kernel/src/<nic>.c`, the
`drivers[]` list and its own gate, and nothing in `net.c`'s policy. It is benchmarked with a
wide grant active. A policy PR touches `net.c`, `net_udp.c`, `net_tcp.c`, `audit.h`, the
cmdline and COM1 parsers, and the gates. The two never ship in one PR.

## Consequences

- **Every existing networking gate gains an `egress=` token** (`ci-smoke-net`,
  `-virtio`, `-http`, `-httpd`, the society gates that use fieldsyncd's UDP, and the
  Firecracker tap test). That is the point: the grant path is exercised by every gate, not
  by one.
- **New gates:**
  - `ci-smoke-net-nogrant`: a NIC, no grant; must show the "networking off" line, no DHCP
    line, and `http example.com` refused with `no grant`.
  - `ci-smoke-egress`: a grant allowing one destination; a second destination refused and
    recorded; a byte budget that trips mid-transfer; a ttl that expires and quiesces the
    stack. Each assertion has a negative control in the PR, per ADR-0016.
- **fieldsyncd and the societies** need a grant that allows their multicast peers; the gates
  that boot them say so explicitly, which also documents what a society talks to.
- **The public console is unchanged** (no NIC). If its VMs ever get one, the launcher issues
  a per-session grant over the same COM1 escape, with the broker's allowlist. Out of scope.
- **Bare metal (#101, #265)** boots offline unless the GRUB line grants egress. A laptop with
  an Intel NIC is not on the network because it has one.
- **Cost:** one allowlist walk per send and two counters per frame. The allowlist is small
  (a few entries); at the frame rates this kernel sees it is not measurable against the
  serial console.

## Alternatives considered

- **Ship e1000 now, policy later.** Rejected by the operator: the driver would widen the
  security claim the day it merged.
- **Gate at the capability only** (`DEVICE_ID_NET` per citizen). Already exists and is not
  enough: it says who may use the network, not where it may go, for how long, or how much.
  The grant adds the destination, time and byte dimensions the capability lacks.
- **A userspace firewall citizen.** It would be the only path between ring 3 and the wire,
  so every socket would be an IPC hop, and it would be a citizen policing citizens. The
  kernel already owns the wire; the check belongs there.

## Rollout

1. **This ADR**, linked from #252 and #256, reviewed by Nick.
2. **PR A, policy:** default off, grant grammar and both sources, `egress_check`, budgets,
   `AUDIT_EGRESS`, `qsh egress`, the gates, and `egress=` on every existing gate.
3. **PR B, driver:** `e1000.c` and `ci-smoke-net-e1000`, with the grant active.
4. Then #253, the same way.

## Amendment (PR A, 2026-10-28)

What shipped in PR A differs from the decision in four small ways, each stated here:

- **Grant source.** Only the boot command line. The COM1 escape is deferred to PR A2: no console
  VM has a NIC, and the reseed parser it would share is on the live console's path.
- **`allow=*`.** A wide grant, for gates and benchmarks whose subject is the stack or a driver.
  Still budgeted and receipted. The decision did not name it; "benchmarked with a wide grant"
  needed a spelling.
- **Infrastructure traffic** is counted in `bytes` but not itemised in the receipt. Per-frame
  rows for ARP and DHCP would be a flood with nothing to read in it.
- **UDP binds to a named port** count as listeners (`listen`); ephemeral binds do not.
