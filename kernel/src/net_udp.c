/**
 * QuantumOS ring-3 UDP sockets (epic #80) — split out of net.c.
 *
 * The socket table, the SENDTO tx ring, the syscall-facing net_udp_* API,
 * and the net-thread rx demux / closing-slot retire / tx drain. The shared
 * wire structs, byte-order helpers and the IPv4/ARP output spine
 * (ip_fill / net_next_hop / resolve_mac) come from net_internal.h.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/net_internal.h>

/* ---- ring-3 UDP sockets (epic #80): socket table + rings ----
 *
 * Shared between cli'd syscalls (bind/sendto/recvfrom/close) and the
 * IF=1 net thread (demux/drain/retire) on one CPU. The net thread can be
 * preempted at ANY instruction, so every multi-field publish follows the
 * same discipline: plain field stores, a compiler barrier, then ONE
 * volatile index/state store that makes the entry visible. Ring indexes
 * are free-running uint32 counters (empty: head == tail; full:
 * head - tail == depth; slot: value & (depth - 1)) — the wrap-safe
 * pattern proven by rtl8139.c's rxq. */

#define UDP_SOCK_MAX 4
#define UDP_RXQ_DEPTH 4
#define UDP_TXQ_DEPTH 4

/* Ports owned by kernel consumers, never bindable: the DNS resolver's
 * fixed source port and DHCP's client port. */
#define UDP_PORT_RESOLVER 40000
#define UDP_PORT_DHCP_CLIENT 68
#define UDP_EPHEMERAL_BASE 49152

/* Slot lifecycle (the design-attack blocker fix): FREE -> ACTIVE on bind
 * (syscall side, state written LAST after full init) -> CLOSING on
 * close/teardown (syscall side, fields left intact) -> FREE only by the
 * NET THREAD, at the top of a service-loop wake — a point where it is by
 * construction not mid-copy into the slot's ring. A slot can therefore
 * never be rebound while a preempted demux still holds pointers into it. */
#define UDP_SLOT_FREE 0
#define UDP_SLOT_ACTIVE 1
#define UDP_SLOT_CLOSING 2

typedef struct {
    uint16_t len; /* payload bytes (from the validated UDP header) */
    uint16_t sport;
    uint8_t sip[4];
    uint8_t data[UDP_PAYLOAD_MAX];
} udp_dgram_t;

typedef struct {
    volatile int state;
    uint32_t owner_pid;
    uint16_t lport;
    udp_dgram_t rxq[UDP_RXQ_DEPTH];
    volatile uint32_t rx_head; /* producer: net thread demux */
    volatile uint32_t rx_tail; /* consumer: RECVFROM syscall */
    uint32_t rx_dropped;       /* ring-full drops (UDP is lossy) */
} udp_sock_t;

static udp_sock_t udp_socks[UDP_SOCK_MAX];
static uint16_t udp_ephemeral_next = UDP_EPHEMERAL_BASE;

typedef struct {
    uint8_t dip[4];
    uint16_t dport;
    uint16_t sport;
    uint16_t len;
    uint8_t data[UDP_PAYLOAD_MAX];
} udp_txent_t;

static udp_txent_t udp_txq[UDP_TXQ_DEPTH];
static volatile uint32_t udp_tx_head; /* producer: SENDTO syscall */
static volatile uint32_t udp_tx_tail; /* consumer: net thread drain */
static uint32_t udp_tx_dropped;       /* ARP-failure drops */

/* Offer one received frame to the bound sockets (net thread only).
 * Validation chain per the design attack: the payload length comes from
 * the UDP header, never from the frame length (Ethernet pads runts to
 * 60 bytes; a forged udp->len must not overread the frame into stale
 * buffer memory). Fragments are dropped — no reassembly. */
void udp_rx_demux(const uint8_t *frame, uint16_t len) {
    const uint32_t base = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t); /* 42 */
    if (len < base) {
        return;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_UDP) {
        return;
    }
    if (ntohs(ip->frag) & 0x3FFF) { /* frag offset or MF set (DF is fine) */
        return;
    }
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + sizeof(ip_hdr_t));
    uint16_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(udp_hdr_t)) {
        return;
    }
    if ((uint32_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t)) + udp_len > len) {
        return; /* header claims more bytes than the frame carries */
    }
    uint16_t plen = (uint16_t)(udp_len - sizeof(udp_hdr_t));
    if (plen > UDP_PAYLOAD_MAX) {
        return;
    }
    uint16_t dport = ntohs(udp->dport);

    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        udp_sock_t *s = &udp_socks[i];
        if (s->state != UDP_SLOT_ACTIVE || s->lport != dport) {
            continue;
        }
        uint32_t head = s->rx_head;
        if (head - s->rx_tail == UDP_RXQ_DEPTH) {
            s->rx_dropped++;
            return;
        }
        udp_dgram_t *d = &s->rxq[head & (UDP_RXQ_DEPTH - 1)];
        d->len = plen;
        d->sport = ntohs(udp->sport);
        for (int b = 0; b < 4; b++) {
            d->sip[b] = ip->src[b];
        }
        const uint8_t *payload = (const uint8_t *)udp + sizeof(udp_hdr_t);
        for (uint16_t b = 0; b < plen; b++) {
            d->data[b] = payload[b];
        }
        publish_barrier();
        s->rx_head = head + 1; /* the single publish store */
        return;
    }
}

/* ---- ring-3 UDP sockets (epic #80): syscall-facing ops ----
 *
 * These run in cli'd syscall context. They only touch the socket table
 * and rings — never the NIC. */

/* Is `port` held by any non-FREE slot? (CLOSING still owns its port
 * until the net thread retires it — rebinding early would let the
 * retiring slot's in-flight datagram land in the new one.) */
static int udp_port_in_use(uint16_t port) {
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        if (udp_socks[i].state != UDP_SLOT_FREE && udp_socks[i].lport == port) {
            return 1;
        }
    }
    return 0;
}

long net_udp_bind(uint32_t pid, uint16_t port) {
    if (!net_nic_present()) {
        return NET_UDP_ENONET;
    }
    if (port == UDP_PORT_RESOLVER || port == UDP_PORT_DHCP_CLIENT) {
        return NET_UDP_EINVAL; /* kernel-owned ports */
    }
    if (port != 0 && udp_port_in_use(port)) {
        return NET_UDP_EINVAL; /* address in use */
    }
    if (port == 0) {
        /* Ephemeral: skip ports still held (bounded — at most
         * UDP_SOCK_MAX can be in use). */
        for (int probe = 0; probe <= UDP_SOCK_MAX; probe++) {
            uint16_t cand = udp_ephemeral_next;
            udp_ephemeral_next = (uint16_t)(cand == 65535 ? UDP_EPHEMERAL_BASE : cand + 1);
            if (!udp_port_in_use(cand)) {
                port = cand;
                break;
            }
        }
        if (port == 0) {
            return NET_UDP_EAGAIN;
        }
    }
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        udp_sock_t *s = &udp_socks[i];
        if (s->state != UDP_SLOT_FREE) {
            continue;
        }
        s->owner_pid = pid;
        s->lport = port;
        s->rx_head = 0;
        s->rx_tail = 0;
        s->rx_dropped = 0;
        publish_barrier();
        s->state = UDP_SLOT_ACTIVE; /* the single publish store */
        return i;
    }
    return NET_UDP_EAGAIN; /* all slots in use */
}

/* Validate sock id BOUNDS before any array access, then state, then
 * owner — in that order (the arrayIndexThenCheck discipline). */
static udp_sock_t *udp_sock_of(uint32_t pid, long sock, long *err) {
    if (sock < 0 || sock >= UDP_SOCK_MAX) {
        *err = NET_UDP_EINVAL;
        return NULL;
    }
    udp_sock_t *s = &udp_socks[sock];
    if (s->state != UDP_SLOT_ACTIVE) {
        *err = NET_UDP_EINVAL;
        return NULL;
    }
    if (s->owner_pid != pid) {
        *err = NET_UDP_EPERM;
        return NULL;
    }
    return s;
}

long net_udp_sendto(uint32_t pid, long sock, const uint8_t *dip, uint16_t dport,
                    const uint8_t *payload, uint16_t len) {
    long err = 0;
    udp_sock_t *s = udp_sock_of(pid, sock, &err);
    if (!s) {
        return err;
    }
    if (len > UDP_PAYLOAD_MAX || dport == 0) {
        return NET_UDP_EINVAL;
    }
    if (!net_nic_present()) {
        return NET_UDP_ENONET;
    }
    if (!net_ready()) {
        return NET_UDP_EAGAIN; /* lease still coming up — poll */
    }
    uint32_t head = udp_tx_head;
    if (head - udp_tx_tail == UDP_TXQ_DEPTH) {
        return NET_UDP_EAGAIN; /* drain in progress — poll */
    }
    udp_txent_t *e = &udp_txq[head & (UDP_TXQ_DEPTH - 1)];
    for (int b = 0; b < 4; b++) {
        e->dip[b] = dip[b];
    }
    e->dport = dport;
    e->sport = s->lport;
    e->len = len;
    for (uint16_t b = 0; b < len; b++) {
        e->data[b] = payload[b];
    }
    publish_barrier();
    udp_tx_head = head + 1; /* the single publish store */
    return 0;
}

long net_udp_recvfrom(uint32_t pid, long sock, uint8_t *payload, uint16_t maxlen, uint8_t *out_sip,
                      uint16_t *out_sport) {
    long err = 0;
    udp_sock_t *s = udp_sock_of(pid, sock, &err);
    if (!s) {
        return err;
    }
    uint32_t tail = s->rx_tail;
    if (tail == s->rx_head) {
        return net_nic_present() ? NET_UDP_EAGAIN : NET_UDP_ENONET;
    }
    const udp_dgram_t *d = &s->rxq[tail & (UDP_RXQ_DEPTH - 1)];
    uint16_t n = d->len;
    if (n > maxlen) {
        n = maxlen; /* truncate — standard UDP recvfrom semantics */
    }
    for (uint16_t b = 0; b < n; b++) {
        payload[b] = d->data[b];
    }
    for (int b = 0; b < 4; b++) {
        out_sip[b] = d->sip[b];
    }
    *out_sport = d->sport;
    publish_barrier();
    s->rx_tail = tail + 1; /* free the ring entry */
    return n;
}

long net_udp_close(uint32_t pid, long sock) {
    long err = 0;
    udp_sock_t *s = udp_sock_of(pid, sock, &err);
    if (!s) {
        return err;
    }
    s->state = UDP_SLOT_CLOSING; /* the net thread retires it */
    return 0;
}

void net_udp_cleanup(uint32_t pid) {
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        if (udp_socks[i].state == UDP_SLOT_ACTIVE && udp_socks[i].owner_pid == pid) {
            udp_socks[i].state = UDP_SLOT_CLOSING;
        }
    }
}

int net_udp_dst_ok(const uint8_t *dip) {
    if (ip_eq(dip, IP_ZERO) || ip_eq(dip, IP_BCAST)) {
        return 0;
    }
    if (dip[0] >= 224) { /* multicast + reserved */
        return 0;
    }
    if (net_has_addr() && ip_eq(dip, my_ip)) {
        return 0; /* no loopback — the NIC doesn't echo its own TX */
    }
    if (net_has_addr() && dip[0] == my_ip[0] && dip[1] == my_ip[1] && dip[2] == my_ip[2] &&
        dip[3] == 255) {
        return 0; /* on-link directed broadcast — unARPable, would stall
                   * the TX drain ~2s in a doomed arp_resolve */
    }
    /* Static mode has no gateway: an off-link unicast is unroutable and
     * would ARP the void — reject it at the syscall boundary (EINVAL). */
    if (static_link && !dhcp_have_lease &&
        !(dip[0] == my_ip[0] && dip[1] == my_ip[1] && dip[2] == my_ip[2])) {
        return 0;
    }
    return 1;
}

/* ---- ring-3 UDP sockets (epic #80): net-thread side ---- */

/* Retire CLOSING slots (net thread only, called at the top of a wake —
 * never mid-copy, so a preempted demux can't be writing into a slot
 * being reset; see the slot-lifecycle comment at the table). */
void udp_retire_closing(void) {
    for (int i = 0; i < UDP_SOCK_MAX; i++) {
        udp_sock_t *s = &udp_socks[i];
        if (s->state != UDP_SLOT_CLOSING) {
            continue;
        }
        s->owner_pid = 0;
        s->lport = 0;
        s->rx_head = 0;
        s->rx_tail = 0;
        publish_barrier();
        s->state = UDP_SLOT_FREE;
    }
}

/* Drain queued datagrams (net thread only). The entry is consumed
 * COMPLETELY before the tail advances — SENDTO's full test then counts
 * an in-drain entry as still occupied, so a preempting syscall can never
 * overwrite what this loop is still reading. */
void udp_tx_drain(void) {
    while (udp_tx_tail != udp_tx_head) {
        const udp_txent_t *e = &udp_txq[udp_tx_tail & (UDP_TXQ_DEPTH - 1)];
        uint8_t mac[ETH_ADDR_LEN];
        if (resolve_mac(net_next_hop(e->dip), mac)) {
            static uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) +
                               UDP_PAYLOAD_MAX]; /* net thread only — never reentered */
            eth_hdr_t *eth = (eth_hdr_t *)out;
            ip_hdr_t *ip = (ip_hdr_t *)(out + sizeof(eth_hdr_t));
            udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
            uint8_t *body = (uint8_t *)udp + sizeof(udp_hdr_t);

            for (int i = 0; i < ETH_ADDR_LEN; i++) {
                eth->dst[i] = mac[i];
                eth->src[i] = self_mac[i];
            }
            eth->type = htons(ETH_TYPE_IP);

            uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + e->len);
            udp->sport = htons(e->sport);
            udp->dport = htons(e->dport);
            udp->len = htons(udp_len);
            udp->checksum = 0; /* legal for IPv4 */
            for (uint16_t b = 0; b < e->len; b++) {
                body[b] = e->data[b];
            }
            ip_fill(ip, e->dip, IP_PROTO_UDP, udp_len, (uint16_t)(0x5000 + udp_tx_tail));
            netdev_transmit(out, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + udp_len));
        } else {
            udp_tx_dropped++; /* bounded ARP failed — UDP is lossy */
        }
        publish_barrier();
        udp_tx_tail = udp_tx_tail + 1; /* only after full consumption */
    }
}
