/**
 * QuantumOS network layer (epic #73 phase 1: Ethernet + ARP)
 *
 * A tiny link/network layer over the RTL8139. Phase 1 proves the driver
 * end to end by ARP-resolving the QEMU user-net gateway (10.0.2.2): an
 * ARP request goes out through TX, SLIRP's reply comes back through the
 * RX interrupt, and the resolved MAC is logged. Later phases add IPv4,
 * UDP, DHCP, DNS on the same send/receive spine.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NET_H
#define NET_H

#include <kernel/types.h>

/* Capability resource id for ring-3 network access (SYS_RESOLVE). The
 * NIC's I/O base is assigned dynamically by PCI, so — unlike the console
 * or disk — the resource id can't be a port base; this synthetic id
 * (the rtl8139's device id) just needs to be unique among device caps. */
#define DEVICE_ID_NET 0x8139

/* Bring up the network stack over the NIC (no-op if no NIC). */
void net_init(void);

/* Configure a STATIC IPv4 address (epic #97), from the `ip=` boot token.
 * Called BEFORE net_init (cmdline is parsed early); net_init applies the
 * pending address after the MAC is read. Static mode skips the DHCP
 * client entirely — the enabler for two guests on a raw L2 segment with
 * no DHCP server. Idempotent; a later call overrides. */
void net_set_static_ip(const uint8_t ip[4]);

/* Max field-coupling peers (epic #139 N-way society). MUST equal ghost.h
 * GHOST_MAX_PEERS (the ring-3 ghostd receive-slot count) and be >= the
 * society's N; the two are separate compilation units (kernel vs ring-3) so
 * they cannot share a symbol — keep them in lockstep. */
#define MAX_PEERS 4

/* Field-coupling peer address(es) (epic #97; N-way #139), from the `peer=`
 * boot token (one IP, or a comma list for N). Set before net_init; read by
 * fieldsyncd via SYSINFO_PEER<index> (packed, little-endian, 0 = out of range)
 * and SYSINFO_PEER_COUNT. */
void net_add_peer_ip(const uint8_t ip[4]);
uint32_t net_get_peer_ip(void); /* legacy: peer[0] (0 = none) */
int net_get_peer_count(void);
uint32_t net_get_peer_at(int i);

/* Boot self-test: ARP-resolve the gateway, obtain a DHCP lease, ping the
 * gateway, and resolve a hostname — proving the stack end to end. Runs in
 * the net kernel thread (interrupts live), not inline in early boot. */
void net_selftest(void);

/* After the self-test, the net thread services ring-3 resolve requests
 * forever (SYS_RESOLVE posts them; a syscall can't do network I/O itself
 * because it runs with interrupts disabled). Never returns. */
void net_service_loop(void);

/* NIC up and a DHCP lease obtained (the network can send unicast IP). */
int net_ready(void);

/* Is a NIC present at all? Distinguishes "no network hardware" (a hard
 * failure) from "lease still coming up" (transient — keep polling). */
int net_nic_present(void);

/* Pure in-memory DNS-parser self-test: proves dns_parse binds an answer to the
 * query on source IP + destination port (not just a txid), so an on-link node
 * can't spoof ring-3 hostname resolution. Needs no NIC — runs on every boot as
 * a synchronous boot gate. Returns 0 on success, negative on the first failure. */
int net_dns_guard_selftest(void);

/* Post a hostname to resolve (from SYS_RESOLVE). 0 accepted, -1 if the
 * network isn't ready or a request is already in flight. */
int net_request_resolve(const char *host);

/* Poll the resolve outcome (from SYS_RESOLVE). 1 done (out_ip filled),
 * 0 pending, -1 failed. */
int net_poll_resolve(uint8_t *out_ip);

/* ---- ring-3 UDP sockets (epic #80) ----
 *
 * Syscall-facing socket operations. All run in cli'd syscall context and
 * never touch the NIC: SENDTO queues into a TX ring the net thread
 * drains, RECVFROM pops a per-socket RX ring the net thread's demux
 * fills. Payload pointers are KERNEL memory (the syscall layer bounces
 * user buffers); the net thread must never see a user pointer. */

/* Largest UDP payload: 1500 (MTU) - 20 (IP) - 8 (UDP). */
#define UDP_PAYLOAD_MAX 1472

/* net_udp_* return codes (mapped to SYSCALL_* by the syscall layer). */
#define NET_UDP_EINVAL (-1) /* bad socket id / state / port */
#define NET_UDP_EPERM (-2)  /* caller does not own the socket */
#define NET_UDP_EAGAIN (-3) /* RX ring empty / TX queue full / no slot */
#define NET_UDP_ENONET (-4) /* no NIC present (hard failure) */

/* Bind a new socket to `port` (0 = ephemeral). Returns the socket id
 * (>= 0) or NET_UDP_EINVAL (reserved/in-use port) / NET_UDP_EAGAIN (no
 * free slot). */
long net_udp_bind(uint32_t pid, uint16_t port);

/* Queue one datagram (payload is kernel memory, len <= UDP_PAYLOAD_MAX).
 * 0 queued; EAGAIN with the TX queue full or the DHCP lease still coming
 * up; ENONET with no NIC. */
long net_udp_sendto(uint32_t pid, long sock, const uint8_t *dip, uint16_t dport,
                    const uint8_t *payload, uint16_t len);

/* Pop the socket's oldest datagram into `payload` (kernel memory, up to
 * `maxlen` bytes — longer datagrams are truncated). Fills the sender's
 * ip/port. Returns copied bytes, EAGAIN when empty, ENONET with no NIC. */
long net_udp_recvfrom(uint32_t pid, long sock, uint8_t *payload, uint16_t maxlen, uint8_t *out_sip,
                      uint16_t *out_sport);

/* Release a socket (marks it CLOSING; the net thread retires it). */
long net_udp_close(uint32_t pid, long sock);

/* Release every socket `pid` owns (process teardown — the epic-#71
 * dead-PCB lesson applied to sockets). */
void net_udp_cleanup(uint32_t pid);

/* Is `dip` a sane unicast destination? Rejects 0.0.0.0, multicast/
 * reserved (>= 224.0.0.0), 255.255.255.255, and our own address. */
int net_udp_dst_ok(const uint8_t *dip);

/* ---- ring-3 TCP client (epic #82) ----
 *
 * Syscall-facing operations on the single connection. All run in cli'd
 * syscall context and never touch the NIC: the IF=1 net thread owns the
 * whole state machine, segment TX, timers, and retransmission; syscalls
 * only read/write shared TCB fields under the publish discipline and
 * post one-shot request flags the net thread services. */

/* net_tcp_* return codes (mapped to SYSCALL_* by the syscall layer). */
#define NET_TCP_EINVAL (-1)     /* not the owner / bad state / bad arg */
#define NET_TCP_EIO (-2)        /* connection refused, reset, or timed out */
#define NET_TCP_WOULDBLOCK (-3) /* handshake/close in progress, RX empty, TX busy */
#define NET_TCP_ENONET (-4)     /* no NIC present (hard failure) */

/* Active open to ip:port. WOULDBLOCK while the handshake runs (poll), 0
 * once ESTABLISHED, EIO on refuse/timeout, ENONET with no NIC, EINVAL if
 * another owner holds the connection. */
long net_tcp_connect(uint32_t pid, const uint8_t *ip, uint16_t port);

/* Passive open (epic #98; multi-listener issue #238): arm a listener on
 * `port`. WOULDBLOCK while arming or while a previous connection drains
 * — poll until 0 (armed, or already carrying a connection on that port).
 *
 * There are TCP_MAX_LISTENERS slots, so several services can listen at
 * once on distinct ports; each slot still carries ONE connection at a
 * time. EINVAL if a live foreign owner holds THAT PORT, if the caller
 * owns the client connection (one connection per pid), or if every slot
 * is taken — the last is permanent, so it is EINVAL and not WOULDBLOCK:
 * a caller polling forever on a condition that cannot clear is worse
 * than one told plainly to stop. ENONET with no NIC. */
long net_tcp_listen(uint32_t pid, uint16_t port);

/* Poll this pid's armed listener for a peer. 0 once connected (ESTABLISHED or
 * CLOSE_WAIT — the request bytes are readable even if the peer already
 * half-closed), WOULDBLOCK while listening / mid-handshake, EIO in any
 * other state (recover with CLOSE, then re-LISTEN), EINVAL if `pid` is
 * not the listener's owner. */
long net_tcp_accept(uint32_t pid);

/* Queue up to MSS bytes (kernel memory) for transmission. Returns bytes
 * accepted, WOULDBLOCK while a segment is still outstanding, EINVAL if
 * not ESTABLISHED/CLOSE_WAIT or not an owner, EIO on a broken
 * connection. */
long net_tcp_send(uint32_t pid, const uint8_t *buf, uint16_t len);

/* Copy received bytes into `buf` (kernel memory, up to maxlen). Returns
 * the byte count, 0 at EOF (peer closed and the ring is drained),
 * WOULDBLOCK when the ring is momentarily empty, EIO on reset. */
long net_tcp_recv(uint32_t pid, uint8_t *buf, uint16_t maxlen);

/* Graceful close (send FIN). Returns 0 once the connection is fully
 * closed, WOULDBLOCK while the teardown completes (poll). */
long net_tcp_close(uint32_t pid);

/* 0 ESTABLISHED, WOULDBLOCK connecting, EIO error, EINVAL if not owner. */
long net_tcp_status(uint32_t pid);

/* Abort the connection if `pid` owns it (process teardown). */
void net_tcp_cleanup(uint32_t pid);

#endif /* NET_H */
