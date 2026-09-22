# Networking: PCI, the RTL8139 NIC, and the link layer

Epic #73, phase 1. QuantumOS booted, ran a shell, kept a persistent
filesystem — but could not speak to a network. This phase adds the
foundation: PCI device discovery, a real NIC driver, and enough of the
link layer to prove packets flow both directions against QEMU's built-in
user-mode network (SLIRP).

## Why prove it with ARP against SLIRP

QEMU's `-netdev user` gives the guest a private 10.0.2.0/24 network whose
gateway (10.0.2.2), DHCP server, and DNS (10.0.2.3) always respond — no
host privileges, no external network, fully deterministic. That makes it
perfect for a headless CI gate. Phase 1's proof is an **ARP request for
the gateway**: if SLIRP's reply comes back and we parse it, then PCI
enumeration, the driver's transmit path, the receive interrupt, and ARP
parsing all work — the entire link layer, both directions, through the
real driver.

## PCI enumeration (`kernel/src/pci.c`)

Just enough PCI to find one device: the classic `0xCF8`/`0xCFC`
configuration port pair, a scan of bus 0 for a vendor:device match
(`10EC:8139`), and reads of BAR0 (the I/O base), the interrupt line
(config offset `0x3C`), and the command register (to set the
bus-master + I/O-space enable bits so the NIC can DMA and answer port
I/O). This is the OS's first device-discovery mechanism; every future
PCI device rides the same three functions.

## The RTL8139 driver (`kernel/src/rtl8139.c`)

QEMU's `-device rtl8139` — the classic hobby-OS NIC because its receive
path is a single **linear ring buffer** rather than a descriptor ring.

- **Bring-up**: power on (`CONFIG_1` = 0), soft-reset and poll (bounded)
  until the reset bit clears, read the 6-byte MAC, program the RX buffer
  base, set the RX config (accept broadcast + physical-match + multicast,
  `WRAP`, 8 KiB buffer), enable the ROK/TOK interrupts, and enable the
  receiver + transmitter.
- **Receive** is interrupt-driven. The chip DMAs each frame — prefixed by
  a 4-byte `{status, length}` header — into the ring and advances its
  write pointer; the IRQ handler walks from our read cursor, strips the
  header and the trailing 4-byte CRC, and pushes each frame into a small
  kernel queue, then advances `CAPR` (with the chip's mandatory
  `CAPR = offset − 16` quirk). The `WRAP` bit means the chip never splits
  a frame at the buffer's end (it may write up to a frame past it), so
  the buffer is oversized and no manual wrap-stitching is needed.
- **Transmit** uses the 4 legacy TX descriptors round-robin: copy the
  frame to a per-descriptor bounce buffer (in the low identity map, so
  its virtual address *is* its DMA address), program the descriptor's
  address and length, and wait (bounded) for the `TOK` bit. Runt frames
  are zero-padded to the 60-byte Ethernet minimum; the chip appends the
  CRC.
- Every hardware wait is **bounded** (the `console_write`/ATA lesson).

The received-frame queue is the `console.c` ring pattern: an IRQ producer
(IF=0) and a consumer that takes an interrupt-save guard, so the network
layer drains frames without racing the handler.

## The link layer (`kernel/src/net.c`)

Ethernet + ARP, with `htons`/`ntohs` on every field wider than a byte
(network order is big-endian, x86 is little-endian). The boot self-test
sends a broadcast ARP request for 10.0.2.2 and waits (bounded) for the
reply, matching it by opcode and sender IP before logging the resolved
MAC.

**Where it runs matters.** The NIC's IRQ only comes alive after
`interrupt_enable_all()` at the very end of `kernel_init`, so the
self-test cannot run inline in early boot. It runs in a dedicated
**kernel thread** (`net`) created at scheduler init: with interrupts
enabled, it sends the ARP and then `hlt`s between polls — sleeping until
the next interrupt (the timer, or the point: the NIC's RX IRQ). A tight
IF-blind spin there would deadlock, since the RX IRQ could never preempt
it to fill the queue.

## The dynamic IRQ

Unlike the timer/keyboard/COM1 (fixed IRQs 0/1/4), the NIC's line is
assigned by PCI at runtime (read from config space). So it can't be a
compile-time `case` in the IRQ dispatch: `irq_handler`'s default case
routes the interrupt to the driver when the number matches
`rtl8139_irq_line()`, and `kernel_init` unmasks that line only when a NIC
is present.

## CI gate: `make ci-smoke-net` (new required `networking` job)

Boots with `-netdev user -device rtl8139` and asserts:
1. `NET: rtl8139 up` — the NIC was found via PCI and brought up.
2. `NET: ARP 10.0.2.2 is at MAC:` — an ARP request reached SLIRP and its
   reply came back through the RX interrupt and parsed.

Live proof: the resolved gateway MAC is `52:55:0A:00:02:02` — exactly the
address SLIRP derives for 10.0.2.2, so the reply is genuine.

The default `-kernel` boot attaches no rtl8139; the driver logs
`NET: no rtl8139 NIC found — networking disabled` and continues
unchanged. The default `make ci-smoke` double-side-gates that honest
degrade (no-NIC line present, `rtl8139 up` absent).

## Phase 2 — IPv4, UDP, and DHCP (`kernel/src/net.c`)

On the same send/receive spine, phase 2 adds:

- **IPv4** with the 20-byte header and its one's-complement header
  checksum (`checksum16`).
- **UDP** (checksum left 0 — "not computed", which RFC 768 permits for
  IPv4 and SLIRP accepts).
- A **DHCP client**: a full DISCOVER → OFFER → REQUEST → ACK exchange.
  All four messages are broadcast (destination MAC `ff:…`, source IP
  `0.0.0.0`, destination `255.255.255.255`) with the BOOTP broadcast flag
  set, so no address or ARP resolution is needed first. The client tracks
  the transaction by a fixed `xid` and the DHCP magic cookie, parses the
  `msgtype`/`server-id` options, and on ACK records `yiaddr` as its lease.

Obtaining the lease is the phase-2 proof, because a successful ACK means
Ethernet TX *and* RX, the IPv4 header + checksum, UDP, and broadcast all
worked, both directions. The gate is `NET: DHCP lease 10.0.2.15` — the
exact address SLIRP's DHCP server hands out.

## CI gate additions

`make ci-smoke-net` now also asserts (with the timeout line as a hard
failure):

3. `NET: DHCP lease 10.0.2.15` — the DHCP exchange completed and the
   IPv4/UDP stack works end to end.

## Phase 3 — ICMP echo + DNS (`kernel/src/net.c`)

Phase 3 sends **unicast** IP (phases 1-2 were broadcast or gateway-only),
using the DHCP lease as the source address and an ARP-resolved
destination MAC:

- **ICMP echo** — an echo request (type 8) to the gateway 10.0.2.2 with
  an id/seq and a 32-byte payload, its ICMP checksum computed, sent to
  the gateway's resolved MAC. SLIRP answers ping to 10.0.2.2 internally,
  so this is a **fully self-contained** round trip that exercises unicast
  IPv4, the header checksum, and ICMP both directions. Gate:
  `NET: ping 10.0.2.2 reply received`.
- **DNS** — the capstone. ARP-resolve SLIRP's DNS proxy (10.0.2.3), send
  a DNS A-query for `example.com` (length-prefixed QNAME, recursion
  desired), and parse the response — skipping the question section,
  following name-compression pointers, and returning the first A record.
  SLIRP forwards the query to the runner's resolver, so a well-formed A
  record coming back proves the whole path: ARP → IPv4 → UDP → DNS, both
  directions. Gate: `NET: DNS example.com -> <a.b.c.d>` (any A record;
  the timeout line is a hard failure).

**A hobby OS that seeds its dice from quantum entropy can now resolve a
hostname over a network stack it built from the NIC driver up.**

The DNS gate is the one non-hermetic check in the suite: it depends on
the CI runner's external resolver (GitHub Actions runners reliably
resolve `example.com`). The ICMP gate is fully hermetic and proves the
unicast IP path on its own.

### DNS answer binding (anti-spoofing)

`SYS_RESOLVE`'s result is trusted by ring 3, so a DNS reply must be bound
to the query the kernel actually issued — otherwise any node on the L2
segment (the epic #97 peer deployments put untrusted guests on a shared
link) could inject a forged A record and poison every hostname lookup.
`dns_parse` therefore accepts a response only when **all four** hold: it
is from the resolver (`ip->src == IP_DNS`), from the DNS port
(`udp->sport == 53`), addressed back to our ephemeral query port
(`udp->dport == sport`), and carries our transaction id. The transaction
id **and** the ephemeral source port are drawn per-query from the
qseed-mixed kernel PRNG (`quantum_kernel_rand`), replacing the old
constant txid `0x2000` / port `40000` that made a blind spoof trivial —
standard DNS source-port + txid randomization. (A *sniffing* on-link
attacker is out of scope for plain DNS-over-UDP; that is DNSSEC's remit.)

A synchronous boot self-test, `net_dns_guard_selftest`, forges three
in-memory replies — a legitimate one (accepted), a wrong-source one
(rejected), and a wrong-dest-port one (rejected) — and the default boot
gates on its `DNSGUARD: spoofed DNS answer rejected (src+port bound)`
line. It needs no NIC, so it runs in every `ci-smoke`, and without the
binding checks the spoofed replies are accepted and the boot panics
before printing the line.

## Full CI gate (`make ci-smoke-net`)

1. `NET: rtl8139 up` — NIC found via PCI and brought up
2. `NET: ARP 10.0.2.2 is at MAC:` — link layer, both directions
3. `NET: DHCP lease 10.0.2.15` — IPv4 + UDP + broadcast
4. `NET: ping 10.0.2.2 reply received` — unicast IPv4 + ICMP
5. `NET: DNS example.com -> <ip>` — the full stack, a real hostname lookup

## Ring-3 access: `SYS_RESOLVE` + the shell's `nslookup`

The stack started as a kernel boot self-test; `SYS_RESOLVE` (#26) exposes
it to user programs. The design respects the one hard constraint: **a
syscall runs with interrupts disabled**, so it can never pump the NIC's
RX interrupt — it cannot do network I/O itself. Instead:

- The resident **`net` kernel thread** (IF=1, which already ran the
  self-test) owns all network I/O. After the self-test it enters
  `net_service_loop`, waking on each timer tick to service requests.
- `SYS_RESOLVE` is **non-blocking request/poll**: the first call posts a
  hostname into a shared slot (`resolve_state`, a volatile written last as
  the handshake flag) and returns `WOULD_BLOCK`; the net thread does the
  DNS lookup and fills the result; later calls poll — `WOULD_BLOCK` while
  pending, then the address, or EIO on failure. The shell loops with
  `yield` (heartbeating so a slow lookup can't get it watchdog-killed).
- It is **capability-gated**: `CAP_RESOURCE_DEVICE` over `DEVICE_ID_NET`
  (a synthetic id — the NIC's I/O base is dynamic), granted to `qsh`
  alone. The capless `ghost_test` proves the denial by attack every boot
  (`NETC: capless caller denied (EPERM)`).

The shell gains `nslookup <host>`. CI pipes `nslookup example.com` into
the shell and gates `qsh: example.com -> <a.b.c.d>` — a hostname resolved
**from ring 3** through the syscall, distinct from the boot self-test's
`NET:` line. A NIC-less boot reports no network honestly.

## Ring-3 UDP sockets: `SYS_UDP` (epic #80, `kernel/src/net_udp.c`)

`SYS_RESOLVE` asks the kernel to do one specific thing; `SYS_UDP` (#27)
generalizes the same worker spine into real sockets — user programs send
and receive **arbitrary datagrams**. It is op-multiplexed over a request
struct (`udp_req_t` in `user/usys.h`; the usys wrappers cap out at three
registers — the classic socketcall shape): `UDP_BIND` (port 0 =
ephemeral, from 49152 up), `UDP_SENDTO`, `UDP_RECVFROM`, `UDP_CLOSE`.
Everything is non-blocking (`WOULD_BLOCK` = poll again), and every op is
gated on the same `DEVICE_ID_NET` capability (held by the citizens granted
`grant_net` — `qsh`, `httpd`, and `fieldsyncd`) — the
capless `ghost_test` proves the bind denial by attack every boot, NIC or
no NIC (`NETC: capless UDP bind denied (EPERM)`).

The concurrency design was adversarially attacked before implementation
(epic #80 records the findings); the load-bearing rules:

- **The net thread owns ALL NIC I/O.** `UDP_SENDTO` copies the payload
  into a kernel TX ring (the net thread runs under its own CR3 — it must
  never see a user pointer); the net thread drains it, ARP-resolving the
  next hop through a small **ARP cache** (gateway and DNS proxy warm
  after the self-test). The drain consumes an entry *completely* before
  advancing the ring's tail, so a syscall can never overwrite an entry
  mid-transmit.
- **Every received frame is offered to the socket demux first** via a
  single `net_rx()` wrapper — the only caller of `rtl8139_receive` — so
  no kernel wait loop (ARP/DHCP/ICMP/DNS) can destroy a user datagram.
  The demux validates ruthlessly: payload length comes from the UDP
  header (never the padded frame length), fragments are dropped, forged
  lengths can't overread the frame.
- **Socket slots have a three-state lifecycle** (`FREE → ACTIVE →
  CLOSING → FREE`): close and process-teardown only *mark* a slot; the
  net thread alone retires `CLOSING → FREE`, at the top of a wake, where
  it is by construction never mid-copy. A slot can't be rebound under a
  preempted demux. Per-socket RX rings are SPSC with free-running
  uint32 indexes; every multi-field publish ends with a compiler barrier
  and one volatile store.

The shell gains `udping <host>` — a **userspace DNS client**: it builds
the A-query in ring 3, `UDP_SENDTO`s it to SLIRP's proxy `10.0.2.3:53`,
`UDP_RECVFROM`s the raw reply (validating sender and txid), and parses
the answer itself. CI gates both `qsh: udp <N> bytes from 10.0.2.3:53`
(raw datagrams, both directions, through the socket API) and
`qsh: udpdns example.com -> <a.b.c.d>` (the ring-3 parse). SLIRP only
*forwards* DNS to the runner's resolver, so these gates share the
existing DNS gates' (reliable) runner-resolver dependency — no new risk
class.

## A TCP client: `SYS_TCP` + the shell's `http` (epic #82, `kernel/src/net_tcp.c`)

The capstone. `SYS_TCP` (#28) is a real, if minimal, TCP **client** — one
active-open connection at a time — built on the same net-thread spine.
The IF=1 net thread owns the entire state machine (CLOSED → SYN_SENT →
ESTABLISHED → the FIN dance → TIME_WAIT → CLOSED, plus an ERROR sink for
RST/timeout); syscalls only read/write the shared TCB under the publish
discipline and post one-shot request flags (`connect_req` / `close_req` /
`abort_req` — independent flags so a reaper's abort can never be clobbered
by a concurrent close). The op-multiplexed syscall exposes CONNECT / SEND
/ RECV (0 = EOF) / CLOSE / STATUS, all non-blocking WOULD_BLOCK polls,
capability-gated exactly like `SYS_UDP`.

The design was adversarially attacked before implementation (epic #82
records the findings). The load-bearing pieces the attack forced:

- **One-pass input.** A single segment carrying ACK + payload + FIN (both
  real servers piggyback the FIN on the last data segment) is processed
  as an ordered pipeline — validate/checksum/RST, then the ACK field,
  then in-order payload, then the FIN — with exactly one ACK reflecting
  the final `rcv_nxt`. Mutually-exclusive branches would drop the FIN and
  hang forever waiting for an EOF that already arrived.
- **Mandatory checksum, done right.** The segment length comes from the
  IP header's `total_len`, never the Ethernet frame length (a 54-byte
  control segment is padded to the 60-byte minimum; folding the pad into
  the checksum drops every ACK). The checksum runs once over a single
  running sum of pseudo-header + header + payload.
- **A window-update path.** The advertised window is the free receive-ring
  space; after the app drains the ring the net thread proactively
  re-advertises, instead of deadlocking on a peer that is waiting for a
  window it was last told was zero.
- **A terminal ERROR + net-thread-only retire.** A syscall never resets a
  net-thread-owned field; ERROR is sticky until an abort retires the TCB
  to a pristine CLOSED (the UDP three-state lesson), and the ephemeral
  port advances every open so a stale in-flight segment can't 4-tuple
  match a fresh connection.

The shell gains `http <host> [port]`: it resolves the name (`SYS_RESOLVE`
— two syscalls compose), connects, sends one `GET / HTTP/1.0` with
`Host:` and `Connection: close`, reads the response to EOF, and prints the
status line and byte count. **CI proves it two ways.** A hermetic gate
runs a loopback `python3 -m http.server` on the runner: SLIRP forwards a
guest connection to `10.0.2.2:PORT` to the host's `127.0.0.1:PORT`, so
`http 10.0.2.2 18080` drives a full three-way handshake, bidirectional
data, and FIN teardown against a real TCP peer with **zero external
network** — gated on `qsh: http 10.0.2.2 -> HTTP/1.[01] 200` and
`qsh: http 10.0.2.2: <N> bytes received`. Then `http example.com` is the
real-world capstone (`qsh: http example.com -> HTTP/1.[01] 200`), the same
non-hermetic runner-egress class as the `example.com` DNS gate.

## A TCP server: listen/accept + httpd (epic #98)

`SYS_TCP` grows two ops, and the OS grows its first inbound service. A
second static TCB (`tcb_srv`) carries the single **passive-open**
connection alongside the client's — the two are disjoint by construction
(the client's local port is ephemeral ≥ 49152, the server's is its listen
port), so `qsh`'s outbound `http` and an inbound request coexist in one
boot. The state machine gains `LISTEN` and `SYN_RCVD`; the demux, service
loop and reset primitives are per-connection (`tcp_rx_one` /
`tcp_service_one`), so a client parked in ERROR can never deafen the
server.

- **`TCP_OP_LISTEN` (5)** — arm the listener on `req.port`. Re-postable:
  the caller polls until 0 (armed), so a lost arming store simply
  re-posts. WOULD_BLOCK also covers a previous connection still draining
  (including a dead owner's — process cleanup posts the abort, the next
  poll claims the freed TCB). One connection per pid: a listener may not
  CONNECT and vice versa.
- **`TCP_OP_ACCEPT` (6)** — poll for a peer: 0 once ESTABLISHED **or
  CLOSE_WAIT** (a client that sends `GET`+FIN in the completing ACK's
  segment has already half-closed by the first accept poll — its request
  bytes are in the ring), WOULD_BLOCK while listening or mid-handshake,
  EIO anywhere else (recover with CLOSE, then re-LISTEN). SEND is legal
  in CLOSE_WAIT for the same reason: a server must be able to answer a
  peer that half-closed behind its request.

The passive open runs entirely in the net-thread demux: a clean SYN
(`SYN && !ACK && !RST`, checked **after** the checksum so a corrupt SYN
can't burn the slot) captures the peer's address — and the arriving
frame's **source MAC** as the reply nexthop, so demux never touches the
blocking ARP pump — then answers SYN|ACK and waits in SYN_RCVD. A lost
SYN|ACK is retransmitted; exhaustion **re-arms LISTEN** (never ERROR — a
dedicated `tcp_rearm_listen` preserves owner and port), as does an RST
during the handshake, so at most one half-open exists and it always
self-heals. Both reset primitives store `state` **last** behind the
publish barrier: a syscall preempting the zero-loop still sees the old
busy state and keeps polling instead of posting into a half-zeroed TCB.

**httpd** (`user/httpd.c`) is the ring-3 consumer: a monitored `grant_net`
service that loops listen → accept → read the request under a **total**
deadline (captured once, never reset on progress — a byte-dribbling
slow-loris is cut off, not refreshed) → serve one HTTP/1.0 response →
close → re-listen. Every body value is computed at request time (uptime
from `SYS_TICKS`, memory from `SYS_SYSINFO`, a rising `served=` counter).
Its `grant_net` is honestly coarser than the job (it also gates UDP/DNS/
outbound connects), so the program deliberately contains no outbound
operation and discards the request bytes unparsed. Without a NIC it logs
once and idles — the default boot is unchanged.

**CI (`ci-smoke-httpd`)** boots with SLIRP
`hostfwd=tcp:127.0.0.1:18081-:8080` and fetches the page **twice from the
host**. Both curls run wall-clock-deadline retry loops (SLIRP accepts the
host side immediately even pre-listen, so a failed attempt burns its full
`--max-time`; and each serve is followed by ~1s of TIME_WAIT before the
re-listen). The gate is non-vacuous three ways: the body sentinel, a
`served=` counter that must **strictly rise** across the fetches (no
static page or stranger's server can satisfy it), and `uptime=[1-9]` on
the second body.

## Source layout (after the transport split)

The stack started life as a single `kernel/src/net.c`. As the ring-3
transports landed it grew past 1900 lines, so it is now split by transport —
same code, three translation units plus a shared internal header:

- **`kernel/src/net.c`** — the control plane: Ethernet, ARP, IPv4, ICMP,
  DHCP, DNS, the `SYS_RESOLVE` state machine, and the boot self-test.
- **`kernel/src/net_udp.c`** — the ring-3 UDP sockets (epic #80): the
  socket table, the SENDTO tx ring, the `net_udp_*` syscall API, and the
  net-thread rx demux / closing-slot retire / tx drain.
- **`kernel/src/net_tcp.c`** — the ring-3 TCP client (epic #82) and
  server (epic #98): the client and server TCBs and their net-thread
  state machine, plus the `net_tcp_*` API.
- **`kernel/include/kernel/net_internal.h`** — the shared internal
  surface: the `eth`/`ip`/`udp` wire structs (with `_Static_assert` size
  guards), the `htons`/`htonl`/`ip_eq` helpers, the netif globals, and the
  handful of prototypes one net TU calls in another. Not a public API;
  user-facing declarations still live in `kernel/include/kernel/net.h`.
- **`kernel/src/netdev.c`** — not a transport: the seam between the stack
  and whichever NIC is present. Holds the probe list, the bound-device
  pointer, and the `netdev_*` forwarders.
- **`kernel/src/virtio_net.c`** — the virtio-mmio transport and the
  virtio-net device, behind the netdev contract. The NIC Firecracker has.

The transports keep all their state `static` in their own file, so the
concurrency invariants (the volatile publish discipline on the UDP tx ring
and the TCP TCB) are unchanged — the split is a pure code move.

## Guest-to-guest networking: static IP + an ARP responder (epic #97)

Everything above rides QEMU's user-mode network (SLIRP), which hands the
guest a DHCP lease, a gateway, DNS — and crucially **answers ARP on the
guest's behalf**. Two QuantumOS instances on a raw L2 segment
(`-netdev socket`) have none of that: no DHCP server, no gateway, and no
ARP proxy. Two gaps had to close before they could exchange a packet:

- **Static addressing.** The boot cmdline gains a token-anchored `ip=A.B.C.D`
  (parsed like `qseed=`/`quiet`): it sets `my_ip`, raises a `static_link`
  flag, and **skips the DHCP client entirely**. The five places that used
  to hard-check `dhcp_have_lease` — routing (`net_next_hop`), readiness
  (`net_ready`), DNS (`dns_resolve`), destination validation
  (`net_udp_dst_ok`), and the boot self-test — now consult
  `net_has_addr()` (`dhcp_have_lease || static_link`), so a static address
  behaves like a leased one. Static mode has no gateway, so `net_next_hop`
  returns *unroutable* (NULL) for an off-link destination and the caller
  drops it rather than ARPing a nonexistent `10.0.2.2` forever. The
  self-test prints `NET: static ip A.B.C.D` and skips its SLIRP-oriented
  ARP/DHCP/ping/DNS phases (each would otherwise burn a doomed multi-second
  timeout).
- **An ARP responder.** `net_rx` now answers an inbound ARP *request* for
  our own address with a reply (and learns the requester's MAC for free).
  SLIRP never sends us requests, so this is inert on every existing gate —
  but it is exactly what lets a peer resolve our link address.

The shell gains `net2 <peer-ip>`: bind UDP `:9999`, send a probe to the
peer, and poll for the peer's probe. `make ci-smoke-2net` boots **two**
guests joined by a `-netdev socket` (distinct static IPs, distinct MACs),
runs `net2` on each, and asserts **both** logs show `NET2: probe from
<peer>`. Because each guest's source IP differs from its own, a received
datagram can only mean our ARP responder answered and the on-link route
delivered — it is impossible to fake by loopback. This is the wire that
epic #97 (two kernels coupling their oscillator fields) will run over.

## Two kernels, one field: UDP coupling (epic #97)

With the wire in place, two QuantumOS instances **couple their `ghostd`
oscillator fields over UDP** — distributed phase synchronization between
two running kernels. A new ring-3 service, **`fieldsyncd`**, holds the
network capability and an IPC send-cap to its local `ghostd` (the
`paradoxd`↔`ghostd` precedent, now over the wire). Once a second, it asks
`ghostd` for a phase snapshot (`GHOST_SNAPSHOT`), sends the 256 phases to
the peer named by the `peer=` boot token, and forwards every datagram it
receives to `ghostd` as a `GHOST_COUPLE` message. `ghostd` folds the
remote phases into its field with a Kuramoto nudge (a fresh target per
frame, so it never overshoots a stale one) and measures the **cross-node
order parameter R_x**. Phases travel as one byte each — the top byte of
`ghostd`'s uint32 "turns" — so a phase difference is exact modular u8
arithmetic with no sign-extension trap.

The gate that proves it, `make ci-smoke-fieldsync`, is deliberately
**non-vacuous** (the design review's central demand: two identical frozen
fields would read R_x = 1.0 from t=0 and prove nothing). Each guest boots
with a **different `qseed`**, so when coupling engages `ghostd` seeds its
field divergently — R_x therefore *starts low* (the fields are genuinely
uncorrelated) and can only rise because `fieldsyncd` is carrying the
peer's phases. The gate asserts, on **both** nodes: a `frame from <peer>`
(real reception), a sub-0.50 R_x sample (divergent start), a
`SYNCHRONIZED (R_x>=0.80)` line (convergence), **and** that the node's own
`GHOSTD: 3/3 RECALL OK` still passes (coupling is additive — it does not
break local associative memory). Observed: R_x climbs 0.11 → 0.03 → 0.99
within a few exchanges; both fields lock.

### N-way society: a mean field over N kernels (epic #139)

The coupling generalizes from two kernels to **N** (a "society"). The
`peer=` boot token accepts a **comma-separated list** —
`peer=10.0.0.2,10.0.0.3` — parsed into a bounded peer array
(`net_add_peer_ip`, `MAX_PEERS = 4`); a guest reads the count via
`SYSINFO_PEER_COUNT` and each entry via `SYSINFO_PEER<index>`. `fieldsyncd`
unicasts its snapshot to **each** peer over a **shared multicast L2**
(QEMU `-netdev socket,mcast=<group>:<port>` — the only single-NIC shared-L2
primitive; unicast-to-each avoids any multicast-send path). Each frame is
tagged with its source IP, and `fieldsyncd` drops any frame whose source is
not a configured peer (closing self-coupling and forged-source slot
exhaustion). `ghostd` keys a **per-peer slot** by source IP and folds the
**circular MEAN field** — the Kuramoto coupling is `(K/P)·Σ_p sin(θ_p − θ_i)`
over the live peers `P`, a sum of sin-of-*differences* (wrap-safe; at one
peer it is byte-identical to the two-kernel fold). The sync verdict uses the
**minimum** pairwise R_x over live peers, so a partial 2-of-3 lock cannot
pass. A stale peer is dropped from the mean, but the live set is floored at
one (never a divide-by-zero, and the 2-VM "freeze on the last frame"
behavior is preserved). `make ci-smoke-society3` boots **three** kernels
with three distinct qseeds and asserts all three reach min-pairwise
R_x ≥ 0.80 from a sub-0.50 divergent start — mean-field convergence that a
two-kernel society (which can only pairwise-lock) structurally cannot fake.
Trust is unchanged from the two-kernel case: a synchronized society proves N
fields coupled on the shared loopback L2, not that N attested identities
coupled — a forged source can *skew* the mean (never *starve* it), and trust
reduces to control of the host L2.

Honest scope: `fieldsyncd` does **not** verify the peer's identity
in-guest — frames carry a boot-identity commitment the *host* attestation
verifier checks; full in-guest mutual Lamport verification is a one-time
signature and out of scope. Peer IPC caps are not re-minted on a watchdog
restart (a known `service.c` limitation), so a reborn `fieldsyncd` logs
that its `ghostd` wiring was lost rather than spinning silently. This
couples `ghostd`'s *living* attractor field; the kernel holographic field
(epic #95) has no continuous dynamics and is not part of this.

## Known limits / follow-ups (the honest boundary)

- **TCP is one client + one server connection.** One active-open and one
  passive-open at a time (epic #98), one connection per pid; the server
  accepts serially (a SYN while busy is dropped and the peer's retry
  lands after the re-listen). Stop-and-wait send (one outstanding
  segment ≤ MSS), in-order receive only (out-of-order segments are
  dropped; the peer retransmits), no congestion control, a shortened
  ~1 s TIME_WAIT (not 2 MSL), and no TLS. A complete, honest
  fetch-a-page client and serve-a-page server — not a general stack.
- UDP: 4 sockets, 4-deep rings, 1472-byte datagrams (no IP
  fragmentation), no UDP TX checksum (0 is legal for IPv4), no
  broadcast/multicast send.
- **Two NIC drivers (virtio-net, rtl8139), IPv4 only.** The probe binds the
  first one present, so the same kernel image comes up on Firecracker and on
  QEMU. **The virtio-net driver has not yet been run** — see its section
  below for what that means. The network capability is held by `qsh`,
  `httpd`, and `fieldsyncd` today; one kernel resolve and one TCP connection
  in flight at a time.
- Static mode is a single flat /24 with no gateway (no off-link routing)
  and no DNS server on the peer segment; it is the two-guest enabler, not
  a general static-networking configuration.

## Robustness hardening (adversarial bug-hunt)

An adversarial sweep of the stack fixed four defects, all against untrusted
wire input:

- **TCP data loss on a racing ACK.** An incoming ACK cleared `tx_pending`
  whenever `snd_una == snd_nxt` — but the app can STAGE a response
  (`tx_pending=1`) before the net thread transmits, at which point
  `snd_una == snd_nxt` still holds. A second ACK-bearing segment (e.g. a
  half-closing client's FIN|ACK) landing in that window silently dropped the
  staged response. The clear is now gated on the ACK actually *advancing*
  `snd_una`.
- **Connection leak to a dead pid.** `net_tcp_cleanup`/`net_tcp_close`
  ignored a pending client `connect_req` (the TCB is still `CLOSED` until the
  net thread sends the SYN), so a client that died before its SYN went out
  would have the connect serviced *after* death — a connection owned by a
  dead/recycled pid. Now retired like the server's pending `listen_req`.
- **Blind RST injection.** A RST matching only the 4-tuple tore the
  connection down with no sequence check (RFC 5961). A RST is now accepted
  only if it ACKs our SYN (SYN_SENT) or sits exactly at `rcv_nxt`
  (synchronized); anything else is dropped.
- **DNS answer compression pointer.** The answer name-walk mishandled a NAME
  ending in a mid-name compression pointer (legal per RFC 1035 §4.1.4),
  overshooting the record and missing a valid A answer. It now mirrors the
  question parser's pointer handling.

## The netdev seam: one ops table, late binding (stage 1 of #235)

Everything above was written against the RTL8139 and called it **by symbol
name** — `rtl8139_transmit`, `rtl8139_present`, `rtl8139_irq_line` — from 16
call sites across `net.c`, `net_udp.c`, `net_tcp.c`, `interrupts.c` and
`main.c`. A second NIC therefore could not exist: adding one meant shadowing
those symbols, which is not something a capability kernel should permit.

`kernel/include/kernel/netdev.h` is the seam — one small ops table, filled in
by a driver, bound once at boot:

| member | contract |
|---|---|
| `name` | driver name, for the boot log |
| `init` | the probe. Returns 1 **only** if a device was found *and* brought up, with no side effects a later driver would trip over. Called at most once per boot. |
| `present` | 1 once a device is bound and up |
| `get_mac` | copies `ETH_ADDR_LEN` bytes out |
| `transmit` | one frame out; 1 on success |
| `irq` | the device's interrupt handler |
| `irq_line` | which line the device landed on (see *The dynamic IRQ*, above) |
| `receive` | drain one frame into `buf`; returns its length, or 0 |

`netdev_init()` walks the registered drivers in order and keeps the first that
reports itself present. `netdev_name()` reports which one for the boot log and
never returns NULL — it answers `"none"` rather than making the caller check.

**The contract is unchanged.** Every member mirrors the `rtl8139_` entry point
of the same name; only the binding is late. That is deliberate: it keeps this
a pure refactor, verifiable by the `net_selftest()` that already exists
(ARP → DHCP → ping → DNS) rather than by new tests written to match it.

The link-layer constants `ETH_ADDR_LEN`, `NET_MTU` and `NET_FRAME_MAX` moved
here from the driver header, because the stack needs them without knowing
which NIC it has.

### The no-NIC path is the same no-NIC path

With nothing bound, the bound-device pointer stays NULL and every accessor
returns the no-NIC answer — 0, or a no-op — rather than faulting. This is not
new tolerance: it is exactly what the `rtl8139_present() == 0` path already
did, and what the stack above already relies on. Boot continues without a
network, as before.

### Why the seam was worth cutting

Firecracker exposes **no PCI at all** (`docs/FIRECRACKER.md`), so the RTL8139
can never be found there and the whole stack above it is dead on that
hypervisor — while ARP, IPv4, ICMP, DHCP, DNS, ring-3 UDP and TCP with
`listen`/`accept` are all already present and working. The NIC was the only
thing missing. This seam is what lets the NIC Firecracker *does* expose be
written without touching any of the stack above it.

## virtio-net over virtio-mmio: the NIC Firecracker has (stage 2 of #235)

The seam above made a second driver possible; this is the driver. A modern
(virtio 1.x) MMIO transport and a virtio-net device, filling in the same
`netdev_ops_t` the rtl8139 fills in.

### There is no bus to walk

The RTL8139 is found by enumerating PCI. Firecracker has no PCI, and
virtio-mmio devices are not discoverable at all — the hypervisor tells the
guest where they are, on the kernel command line, one token per device:

```
virtio_mmio.device=4K@0xd0000000:5
                   size@base:irq
```

`cmdline_get_virtio_mmio()` in `main.c` decodes every such token, matched
**only at a whitespace token boundary** — the same discipline `ip=` and
`peer=` already use, so it can never trip on a substring. Size takes an
optional `K`/`M` suffix, base is hex or decimal, irq is decimal.

Each token is handed to `virtio_mmio_add_device(base, size, irq)`, which
stores it (up to `VIRTIO_MMIO_MAX_DEVICES`, 8 — Firecracker's default is
well under that, and a static array means the probe needs no allocator).
It is idempotent per base, and it **refuses** `irq >= 16` rather than
storing it: that is the rtl8139 lesson written down, because a line that
cannot deliver RX is better refused at parse time than unmasked later as a
bogus vector. Malformed tokens are skipped, not guessed at — a wrong MMIO
base is a fault, not a degraded NIC.

The parser must run **before** `netdev_init()`, since the probe reads the
recorded slots, checking each for the virtio magic and a network device id.
`netdev_init()` tries virtio-net **before** the PCI bus scan, so a guest
with both takes the paravirtual NIC.

### Status: written, not yet run

**This driver has never been compiled or executed.** There is no
cross-toolchain, KVM or QEMU on the machine it was written on, so nothing
here has been past a compiler. It is documented because the seam and the
cmdline contract are worth reviewing on their merits — not because it is
known to work.

The acceptance test is the one that already exists and has never been able
to run: boot under Firecracker with a tap device and require
`net_selftest()` (ARP → DHCP → ping → DNS) to pass. Until that goes green,
treat this section as a description of intent.

Where it is likeliest wrong, for whoever runs it first: the avail/used ring
index wrap arithmetic; whether Firecracker wants `QueueNum` written before
or after the address registers; and whether refusing a `QueueNumMax < 64`
outright should instead shrink the ring.
