/**
 * QuantumOS network layer implementation (epic #73 phases 1-2).
 *
 * Ethernet + ARP (phase 1) and IPv4 + UDP + a DHCP client (phase 2) over
 * the RTL8139. All multi-byte network fields are big-endian ("network
 * order"); x86 is little-endian, so htons/htonl appear on every field
 * wider than a byte.
 *
 * The boot self-test proves the stack end to end against QEMU user-net
 * (SLIRP): it ARP-resolves the gateway, then runs a full DHCP exchange
 * to obtain the 10.0.2.15 lease — which exercises Ethernet TX/RX, IPv4,
 * UDP, checksums, and broadcast, both directions.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/net.h>
#include <kernel/netdev.h>
#include <kernel/boot.h>
#include <kernel/net_internal.h>
#include <kernel/quantum.h> /* quantum_kernel_rand — unpredictable DNS txid/sport */

/* QEMU user-net (SLIRP): gateway 10.0.2.2, DHCP hands out 10.0.2.15.
 * IP_ZERO/IP_BCAST are also read by the transport TUs, so they have
 * external linkage (declared extern in net_internal.h). */
static const uint8_t IP_GATEWAY[4] = {10, 0, 2, 2};
const uint8_t IP_ZERO[4] = {0, 0, 0, 0};
const uint8_t IP_BCAST[4] = {255, 255, 255, 255};
static const uint8_t MAC_BCAST[ETH_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define ETH_TYPE_ARP 0x0806
#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define DHCP_SPORT 68
#define DHCP_DPORT 67

/* netif state; extern in net_internal.h so the transport TUs can read it. */
uint8_t self_mac[ETH_ADDR_LEN];

/* DHCP outcome (filled by the exchange). */
uint8_t my_ip[4];
int dhcp_have_lease;

/* Static-IP mode (epic #97): set when the `ip=` boot token configured an
 * address. When set, my_ip is authoritative, the DHCP client is skipped,
 * and net_has_addr() is true without a lease. `static_pending` bridges
 * the parse-before-net_init ordering. */
int static_link;
static int static_pending;
static uint8_t static_ip[4];

/* True once the stack has a usable source address — by DHCP lease OR by
 * static configuration. The five sites that used to hard-check
 * dhcp_have_lease (routing, DNS, dst validation, readiness, self-test)
 * all consult this so static mode behaves like a leased one. */
int net_has_addr(void) {
    return dhcp_have_lease || static_link;
}

void net_set_static_ip(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        static_ip[i] = ip[i];
    }
    static_pending = 1;
}

/* Field-coupling peers (epic #97; N-way society #139): the `peer=` boot token
 * names the other node(s) — one IP for a 2-VM society, a comma list for N.
 * Stored packed so SYSINFO_PEER<index> hands each to fieldsyncd in a single
 * uncapped syscall return, and SYSINFO_PEER_COUNT reports how many. */
static uint32_t peer_ip_packed[MAX_PEERS];
static int peer_count;

/* Append one peer IP (bounded INTERNALLY — peer= is an untrusted boot-cmdline
 * boundary, so the array must never be overrun regardless of the parser). */
void net_add_peer_ip(const uint8_t ip[4]) {
    if (peer_count >= MAX_PEERS) {
        return;
    }
    peer_ip_packed[peer_count++] = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) |
                                   ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24);
}

/* Legacy single-peer accessor: peer[0] (0 = none). */
uint32_t net_get_peer_ip(void) {
    return peer_count > 0 ? peer_ip_packed[0] : 0;
}

int net_get_peer_count(void) {
    return peer_count;
}

/* Peer at index i, packed (0 if out of range). */
uint32_t net_get_peer_at(int i) {
    return (i >= 0 && i < peer_count) ? peer_ip_packed[i] : 0;
}

/* ---- packet headers (all packed). eth_hdr_t / ip_hdr_t / udp_hdr_t are
 * shared and live in net_internal.h; arp_pkt_t and dhcp_hdr_t are used
 * only by the control plane and stay here. ---- */
typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[ETH_ADDR_LEN];
    uint8_t spa[4];
    uint8_t tha[ETH_ADDR_LEN];
    uint8_t tpa[4];
} arp_pkt_t;

/* BOOTP/DHCP fixed header (before options). */
typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t siaddr[4];
    uint8_t giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic; /* 0x63825363 */
} dhcp_hdr_t;

#define DHCP_XID 0x51050403u
#define DHCP_MAGIC 0x63825363u
#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY 2
#define DHCP_OPT_MSGTYPE 53
#define DHCP_OPT_REQIP 50
#define DHCP_OPT_SERVERID 54
#define DHCP_OPT_END 255
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5

/* DHCP client state. */
static int dhcp_state; /* 0 idle, 1 sent DISCOVER, 2 sent REQUEST */
static uint8_t dhcp_offer_ip[4];
static uint8_t dhcp_server_id[4];

/* The net thread's ONLY window onto the RX queue: every frame is offered
 * to the socket demux BEFORE the caller's own matcher sees it, so no
 * wait loop (ARP, DHCP, ICMP, DNS) can ever destroy a user datagram —
 * netdev_receive is destructive and has no putback. No code in this
 * file may call netdev_receive directly except this function. */
static void arp_maybe_reply(const uint8_t *frame, uint16_t len);

static uint16_t net_rx(uint8_t *buf, uint16_t max) {
    uint16_t n = netdev_receive(buf, max);
    if (n > 0) {
        /* Answer peer ARP requests before demux — on a two-guest socket
         * L2 there is no SLIRP to do it for us (epic #97). */
        arp_maybe_reply(buf, n);
        udp_rx_demux(buf, n);
        tcp_rx_demux(buf, n);
    }
    return n;
}

/* ---- ARP cache (net thread only — no handshake needed) ----
 *
 * The design attack found the TX drain would otherwise re-ARP (a 200-tick
 * blocking pump) on every datagram: nothing cached MACs before — the
 * self-test's gateway MAC died as a stack local. */
#define ARP_CACHE_MAX 4
static struct {
    uint8_t ip[4];
    uint8_t mac[ETH_ADDR_LEN];
    int valid;
} arp_cache[ARP_CACHE_MAX];

static int arp_cache_get(const uint8_t *ip, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            for (int b = 0; b < ETH_ADDR_LEN; b++) {
                out_mac[b] = arp_cache[i].mac[b];
            }
            return 1;
        }
    }
    return 0;
}

static void arp_cache_put(const uint8_t *ip, const uint8_t *mac) {
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_MAX; i++) {
        if (arp_cache[i].valid && ip_eq(arp_cache[i].ip, ip)) {
            slot = i;
            break;
        }
        if (slot < 0 && !arp_cache[i].valid) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0; /* full: evict the oldest entry deterministically */
    }
    for (int b = 0; b < 4; b++) {
        arp_cache[slot].ip[b] = ip[b];
    }
    for (int b = 0; b < ETH_ADDR_LEN; b++) {
        arp_cache[slot].mac[b] = mac[b];
    }
    arp_cache[slot].valid = 1;
}

/* 16-bit one's-complement checksum over `len` bytes. */
static uint16_t checksum16(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((data[0] << 8) | data[1]);
        data += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint32_t)(data[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void net_init(void) {
    if (!netdev_present()) {
        return;
    }
    netdev_get_mac(self_mac);
    dhcp_have_lease = 0;
    dhcp_state = 0;
    /* Apply a `ip=` static address parsed before net_init ran. */
    if (static_pending) {
        for (int i = 0; i < 4; i++) {
            my_ip[i] = static_ip[i];
        }
        static_link = 1;
    }
}

/* Answer an inbound ARP REQUEST for our own address (epic #97). SLIRP
 * always did this FOR the guest, so nothing here ever needed it — but two
 * QuantumOS guests on a raw socket L2 must resolve each other themselves.
 * Called from net_rx for every ARP frame; a non-request or a request for
 * someone else's IP is ignored. Also learns the requester's MAC for free
 * so the reply's eventual return traffic needs no second round trip. */
static void arp_maybe_reply(const uint8_t *frame, uint16_t len) {
    if (!net_has_addr() || len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        return;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_ARP) {
        return;
    }
    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    if (ntohs(arp->oper) != ARP_OP_REQUEST || !ip_eq(arp->tpa, my_ip)) {
        return;
    }
    /* Learn the asker so our reply's return path is warm. */
    arp_cache_put(arp->spa, arp->sha);

    uint8_t out[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *reth = (eth_hdr_t *)out;
    arp_pkt_t *rarp = (arp_pkt_t *)(out + sizeof(eth_hdr_t));
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        reth->dst[i] = arp->sha[i];
        reth->src[i] = self_mac[i];
    }
    reth->type = htons(ETH_TYPE_ARP);
    rarp->htype = htons(ARP_HTYPE_ETH);
    rarp->ptype = htons(ARP_PTYPE_IP);
    rarp->hlen = ETH_ADDR_LEN;
    rarp->plen = 4;
    rarp->oper = htons(ARP_OP_REPLY);
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        rarp->sha[i] = self_mac[i];
        rarp->tha[i] = arp->sha[i];
    }
    for (int i = 0; i < 4; i++) {
        rarp->spa[i] = my_ip[i];
        rarp->tpa[i] = arp->spa[i];
    }
    netdev_transmit(out, sizeof(out));
}

/* ---- ARP ---- */
static int arp_send_request(const uint8_t *target_ip, const uint8_t *sender_ip) {
    uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = 0xFF;
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_ARP);
    arp->htype = htons(ARP_HTYPE_ETH);
    arp->ptype = htons(ARP_PTYPE_IP);
    arp->hlen = ETH_ADDR_LEN;
    arp->plen = 4;
    arp->oper = htons(ARP_OP_REQUEST);
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        arp->sha[i] = self_mac[i];
        arp->tha[i] = 0x00;
    }
    for (int i = 0; i < 4; i++) {
        arp->spa[i] = sender_ip[i];
        arp->tpa[i] = target_ip[i];
    }
    return netdev_transmit(frame, sizeof(frame));
}

static int arp_is_reply_from(const uint8_t *frame, uint16_t len, const uint8_t *want_ip,
                             uint8_t *out_mac) {
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_ARP) {
        return 0;
    }
    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    if (ntohs(arp->oper) != ARP_OP_REPLY || !ip_eq(arp->spa, want_ip)) {
        return 0;
    }
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        out_mac[i] = arp->sha[i];
    }
    return 1;
}

/* ---- DHCP ---- */

/* Build eth+ip+udp+dhcp for a broadcast DHCP message of the given type.
 * `req_ip`/`server_id` are used only for REQUEST (option 50/54). Returns
 * the total frame length. */
static uint16_t dhcp_build(uint8_t *frame, uint8_t msgtype, const uint8_t *req_ip,
                           const uint8_t *server_id) {
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    dhcp_hdr_t *dh = (dhcp_hdr_t *)((uint8_t *)udp + sizeof(udp_hdr_t));
    uint8_t *opt = (uint8_t *)dh + sizeof(dhcp_hdr_t);
    uint8_t *opt_start = opt;

    /* Ethernet: broadcast. */
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = MAC_BCAST[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    /* DHCP fixed header. */
    for (uint32_t i = 0; i < sizeof(dhcp_hdr_t); i++) {
        ((uint8_t *)dh)[i] = 0;
    }
    dh->op = DHCP_OP_REQUEST;
    dh->htype = 1;
    dh->hlen = ETH_ADDR_LEN;
    dh->xid = htonl(DHCP_XID);
    dh->flags = htons(0x8000); /* broadcast reply (we have no IP yet) */
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        dh->chaddr[i] = self_mac[i];
    }
    dh->magic = htonl(DHCP_MAGIC);

    /* Options. */
    *opt++ = DHCP_OPT_MSGTYPE;
    *opt++ = 1;
    *opt++ = msgtype;
    if (msgtype == DHCP_REQUEST) {
        *opt++ = DHCP_OPT_REQIP;
        *opt++ = 4;
        for (int i = 0; i < 4; i++) {
            *opt++ = req_ip[i];
        }
        *opt++ = DHCP_OPT_SERVERID;
        *opt++ = 4;
        for (int i = 0; i < 4; i++) {
            *opt++ = server_id[i];
        }
    }
    *opt++ = DHCP_OPT_END;

    uint16_t dhcp_len = (uint16_t)(sizeof(dhcp_hdr_t) + (opt - opt_start));
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + dhcp_len);
    uint16_t ip_len = (uint16_t)(sizeof(ip_hdr_t) + udp_len);

    /* UDP (checksum 0 = not computed, legal for IPv4). */
    udp->sport = htons(DHCP_SPORT);
    udp->dport = htons(DHCP_DPORT);
    udp->len = htons(udp_len);
    udp->checksum = 0;

    /* IPv4: broadcast src 0.0.0.0 -> 255.255.255.255. */
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(ip_len);
    ip->id = htons(0x1234);
    ip->frag = htons(0x0000);
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip->checksum = 0;
    for (int i = 0; i < 4; i++) {
        ip->src[i] = IP_ZERO[i];
        ip->dst[i] = IP_BCAST[i];
    }
    ip->checksum = htons(checksum16((const uint8_t *)ip, sizeof(ip_hdr_t)));

    return (uint16_t)(sizeof(eth_hdr_t) + ip_len);
}

/* Find DHCP option `code` in the options area; returns its value pointer
 * and length via out-params, or 0 if absent. */
static int dhcp_find_opt(const uint8_t *opts, uint32_t optlen, uint8_t code, const uint8_t **val,
                         uint8_t *vlen) {
    uint32_t i = 0;
    while (i + 1 < optlen) {
        uint8_t c = opts[i];
        if (c == DHCP_OPT_END) {
            break;
        }
        if (c == 0) { /* pad */
            i++;
            continue;
        }
        uint8_t l = opts[i + 1];
        if (i + 2 + l > optlen) {
            break;
        }
        if (c == code) {
            *val = opts + i + 2;
            *vlen = l;
            return 1;
        }
        i += 2 + l;
    }
    return 0;
}

/* Parse a received DHCP reply of the wanted type; on match, copy yiaddr
 * and the server-id option out. Returns the message type, or 0. */
static uint8_t dhcp_parse(const uint8_t *frame, uint16_t len, uint8_t *yiaddr, uint8_t *server_id) {
    uint32_t need = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t);
    if (len < need) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return 0;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_UDP) {
        return 0;
    }
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + sizeof(ip_hdr_t));
    if (ntohs(udp->sport) != DHCP_DPORT || ntohs(udp->dport) != DHCP_SPORT) {
        return 0;
    }
    const dhcp_hdr_t *dh = (const dhcp_hdr_t *)((const uint8_t *)udp + sizeof(udp_hdr_t));
    /* xid and magic are stored in network order on both sides, so compare
     * the raw (already byte-swapped) values directly. */
    if (dh->op != DHCP_OP_REPLY || dh->xid != htonl(DHCP_XID) || dh->magic != htonl(DHCP_MAGIC)) {
        return 0;
    }

    const uint8_t *opts = (const uint8_t *)dh + sizeof(dhcp_hdr_t);
    uint32_t optlen = len - need;
    const uint8_t *v;
    uint8_t vl;
    if (!dhcp_find_opt(opts, optlen, DHCP_OPT_MSGTYPE, &v, &vl) || vl < 1) {
        return 0;
    }
    uint8_t msgtype = v[0];
    for (int i = 0; i < 4; i++) {
        yiaddr[i] = dh->yiaddr[i];
    }
    if (dhcp_find_opt(opts, optlen, DHCP_OPT_SERVERID, &v, &vl) && vl == 4) {
        for (int i = 0; i < 4; i++) {
            server_id[i] = v[i];
        }
    }
    return msgtype;
}

/* Feed one received frame to the DHCP state machine. */
static void dhcp_rx(const uint8_t *frame, uint16_t len) {
    uint8_t yiaddr[4], server_id[4] = {0, 0, 0, 0};
    uint8_t t = dhcp_parse(frame, len, yiaddr, server_id);
    if (t == DHCP_OFFER && dhcp_state == 1) {
        for (int i = 0; i < 4; i++) {
            dhcp_offer_ip[i] = yiaddr[i];
            dhcp_server_id[i] = server_id[i];
        }
        uint8_t
            out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t) + 32];
        uint16_t n = dhcp_build(out, DHCP_REQUEST, dhcp_offer_ip, dhcp_server_id);
        netdev_transmit(out, n);
        dhcp_state = 2;
    } else if (t == DHCP_ACK && dhcp_state == 2) {
        for (int i = 0; i < 4; i++) {
            my_ip[i] = yiaddr[i];
        }
        dhcp_have_lease = 1;
    }
}

/* ---- IPv4 unicast + ICMP + DNS (phase 3) ---- */

#define IP_PROTO_ICMP 1
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY 0
#define DNS_PORT 53
static const uint8_t IP_DNS[4] = {10, 0, 2, 3}; /* SLIRP's DNS proxy */

/* Fill an IPv4 header for a unicast packet of `payload_len` bytes to
 * `dst`, from `my_ip`. Computes the header checksum. */
void ip_fill(ip_hdr_t *ip, const uint8_t *dst, uint8_t proto, uint16_t payload_len,
             uint16_t ident) {
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + payload_len));
    ip->id = htons(ident);
    ip->frag = htons(0x0000);
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    for (int i = 0; i < 4; i++) {
        ip->src[i] = my_ip[i];
        ip->dst[i] = dst[i];
    }
    ip->checksum = htons(checksum16((const uint8_t *)ip, sizeof(ip_hdr_t)));
}

/* Resolve `ip`'s MAC via ARP (bounded, pumping the RX queue through
 * net_rx so user datagrams arriving mid-wait reach their sockets).
 * Returns 1 and fills out_mac on success. */
static int arp_resolve(const uint8_t *ip, const uint8_t *sender_ip, uint8_t *out_mac) {
    arp_send_request(ip, sender_ip);
    uint8_t frame[NET_FRAME_MAX];
    for (int tries = 0; tries < 200; tries++) {
        uint16_t n = net_rx(frame, sizeof(frame));
        if (n > 0 && arp_is_reply_from(frame, n, ip, out_mac)) {
            return 1;
        }
        if (n == 0) {
            if (tries > 0 && tries % 64 == 0) {
                arp_send_request(ip, sender_ip);
            }
            __asm__ volatile("hlt");
        }
    }
    return 0;
}

/* Cache-first MAC lookup (net thread only): hit is instant; a miss runs
 * one bounded arp_resolve and remembers the answer. */
int resolve_mac(const uint8_t *ip, uint8_t *out_mac) {
    if (!ip) {
        return 0; /* net_next_hop returned "unroutable" (static, off-link) */
    }
    if (arp_cache_get(ip, out_mac)) {
        return 1;
    }
    if (!arp_resolve(ip, my_ip, out_mac)) {
        return 0;
    }
    arp_cache_put(ip, out_mac);
    return 1;
}

/* ICMP echo request to `dst` (dst_mac is its resolved link address).
 * `ident`/`seq` tag the request; returns the built frame length. */
static uint16_t icmp_build(uint8_t *frame, const uint8_t *dst, const uint8_t *dst_mac,
                           uint16_t ident, uint16_t seq) {
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    uint8_t *icmp = (uint8_t *)ip + sizeof(ip_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = dst_mac[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    /* ICMP echo: type(1) code(1) csum(2) id(2) seq(2) + 32 payload bytes. */
    uint16_t icmp_len = 8 + 32;
    for (uint16_t i = 0; i < icmp_len; i++) {
        icmp[i] = 0;
    }
    icmp[0] = ICMP_ECHO_REQUEST;
    icmp[1] = 0;
    icmp[4] = (uint8_t)(ident >> 8);
    icmp[5] = (uint8_t)ident;
    icmp[6] = (uint8_t)(seq >> 8);
    icmp[7] = (uint8_t)seq;
    for (uint16_t i = 0; i < 32; i++) {
        icmp[8 + i] = (uint8_t)('a' + (i % 26));
    }
    uint16_t csum = checksum16(icmp, icmp_len);
    icmp[2] = (uint8_t)(csum >> 8);
    icmp[3] = (uint8_t)csum;

    ip_fill(ip, dst, IP_PROTO_ICMP, icmp_len, ident);
    return (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + icmp_len);
}

/* Is `frame` an ICMP echo reply from `src` with our id/seq? */
static int icmp_is_reply(const uint8_t *frame, uint16_t len, const uint8_t *src, uint16_t ident,
                         uint16_t seq) {
    uint32_t need = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 8;
    if (len < need) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return 0;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_ICMP || !ip_eq(ip->src, src)) {
        return 0;
    }
    const uint8_t *icmp = (const uint8_t *)ip + sizeof(ip_hdr_t);
    uint16_t rid = (uint16_t)((icmp[4] << 8) | icmp[5]);
    uint16_t rseq = (uint16_t)((icmp[6] << 8) | icmp[7]);
    return icmp[0] == ICMP_ECHO_REPLY && rid == ident && rseq == seq;
}

/* Build a DNS A-query for `name` to the resolver. UDP over IPv4. `sport` is the
 * per-query ephemeral source port the reply must be addressed back to (checked
 * in dns_parse — response-port binding). Returns the frame length, or 0 if the
 * name doesn't fit. */
static uint16_t dns_build(uint8_t *frame, const uint8_t *dns_mac, const char *name, uint16_t txid,
                          uint16_t sport) {
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    uint8_t *dns = (uint8_t *)udp + sizeof(udp_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = dns_mac[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    /* DNS header: id, flags 0x0100 (recursion desired), qdcount 1. */
    dns[0] = (uint8_t)(txid >> 8);
    dns[1] = (uint8_t)txid;
    dns[2] = 0x01;
    dns[3] = 0x00;
    dns[4] = 0x00;
    dns[5] = 0x01; /* qdcount = 1 */
    dns[6] = dns[7] = dns[8] = dns[9] = dns[10] = dns[11] = 0;
    int p = 12;

    /* QNAME: length-prefixed labels. */
    int label_start = p++;
    int label_len = 0;
    for (const char *c = name; *c; c++) {
        if (*c == '.') {
            dns[label_start] = (uint8_t)label_len;
            label_start = p++;
            label_len = 0;
        } else {
            if (p >= 200) {
                return 0;
            }
            dns[p++] = (uint8_t)*c;
            label_len++;
        }
    }
    dns[label_start] = (uint8_t)label_len;
    dns[p++] = 0x00; /* root label */
    dns[p++] = 0x00; /* QTYPE A = 1 */
    dns[p++] = 0x01;
    dns[p++] = 0x00; /* QCLASS IN = 1 */
    dns[p++] = 0x01;

    uint16_t dns_len = (uint16_t)p;
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + dns_len);
    udp->sport = htons(sport);
    udp->dport = htons(DNS_PORT);
    udp->len = htons(udp_len);
    udp->checksum = 0;

    ip_fill(ip, IP_DNS, IP_PROTO_UDP, udp_len, txid);
    return (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + udp_len);
}

/* Parse a DNS response; on a matching txid with at least one A record,
 * copy the first A address into out_ip and return 1.
 *
 * The answer is bound to the query the kernel actually issued on FOUR axes, so
 * an on-link node cannot inject a spoofed answer to poison ring-3 resolution
 * (SYS_RESOLVE trusts this result): it must come FROM the resolver (ip->src ==
 * IP_DNS), be addressed back TO our ephemeral query port (udp->dport == sport),
 * FROM the DNS port (udp->sport == 53), and carry our transaction id. `sport`
 * and `txid` are drawn per-query from the qseed-mixed PRNG, so a blind attacker
 * cannot predict the {sport, txid} pair it must match. (A sniffing on-link
 * attacker is out of scope for plain DNS-over-UDP — that is DNSSEC's job.) */
static int dns_parse(const uint8_t *frame, uint16_t len, uint16_t txid, uint16_t sport,
                     uint8_t *out_ip) {
    uint32_t base = sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t);
    if (len < base + 12) {
        return 0;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return 0;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_UDP || !ip_eq(ip->src, IP_DNS)) {
        return 0; /* must be FROM the resolver we queried (mirrors icmp_is_reply) */
    }
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + sizeof(ip_hdr_t));
    if (ntohs(udp->sport) != DNS_PORT || ntohs(udp->dport) != sport) {
        return 0; /* must be from :53 and addressed back to our ephemeral port */
    }
    const uint8_t *dns = frame + base;
    uint16_t rid = (uint16_t)((dns[0] << 8) | dns[1]);
    if (rid != txid) {
        return 0;
    }
    uint16_t qd = (uint16_t)((dns[4] << 8) | dns[5]);
    uint16_t an = (uint16_t)((dns[6] << 8) | dns[7]);
    if (an == 0) {
        return 0;
    }

    /* Skip the header (12) and the question section. */
    uint32_t off = base + 12;
    for (uint16_t q = 0; q < qd; q++) {
        while (off < len && frame[off] != 0) {
            if ((frame[off] & 0xC0) == 0xC0) { /* compression pointer */
                off += 2;
                goto qdone;
            }
            off += frame[off] + 1;
        }
        off += 1; /* the 0 root label */
    qdone:
        off += 4; /* QTYPE + QCLASS */
    }

    /* Walk answers; return the first A record (type 1, class IN, rdlen 4). */
    for (uint16_t a = 0; a < an; a++) {
        if (off + 2 > len) {
            return 0;
        }
        if ((frame[off] & 0xC0) == 0xC0) {
            off += 2; /* compressed name */
        } else {
            /* Labels, possibly terminated by a compression pointer part-way
             * through (RFC 1035 4.1.4, legal) — mirror the question walk above,
             * or a mid-name pointer's 0xC0 byte is misread as a label length
             * and off overshoots the record (a valid A answer is then missed). */
            while (off < len && frame[off] != 0) {
                if ((frame[off] & 0xC0) == 0xC0) {
                    off += 2;
                    goto name_done;
                }
                off += frame[off] + 1;
            }
            off += 1;
        name_done:;
        }
        if (off + 10 > len) {
            return 0;
        }
        uint16_t type = (uint16_t)((frame[off] << 8) | frame[off + 1]);
        uint16_t rdlen = (uint16_t)((frame[off + 8] << 8) | frame[off + 9]);
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            for (int i = 0; i < 4; i++) {
                out_ip[i] = frame[off + i];
            }
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

/* Assemble a minimal well-formed DNS A-response frame into buf (eth+ip+udp+dns,
 * one compressed-name A answer) with caller-chosen source IP, dest (ephemeral)
 * port, transaction id, and answer address — so the guard self-test can forge
 * both legitimate and spoofed replies deterministically. Returns the length.
 * Uses no netif state (fixed L2/MAC/dst), so it is safe to run before net_init. */
static uint16_t dns_build_response(uint8_t *buf, const uint8_t *src_ip, uint16_t dport,
                                   uint16_t txid, const uint8_t *answer_ip) {
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    ip_hdr_t *ip = (ip_hdr_t *)(buf + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)((uint8_t *)ip + sizeof(ip_hdr_t));
    uint8_t *dns = (uint8_t *)udp + sizeof(udp_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = 0;
        eth->src[i] = 0;
    }
    eth->type = htons(ETH_TYPE_IP);

    dns[0] = (uint8_t)(txid >> 8);
    dns[1] = (uint8_t)txid;
    dns[2] = 0x81; /* response, recursion available */
    dns[3] = 0x80;
    dns[4] = dns[5] = 0; /* qdcount 0 */
    dns[6] = 0;
    dns[7] = 1;                              /* ancount 1 */
    dns[8] = dns[9] = dns[10] = dns[11] = 0; /* ns/ar counts 0 */
    int p = 12;
    dns[p++] = 0xC0; /* compressed name -> header offset 12 */
    dns[p++] = 0x0C;
    dns[p++] = 0x00; /* type A */
    dns[p++] = 0x01;
    dns[p++] = 0x00; /* class IN */
    dns[p++] = 0x01;
    dns[p++] = 0x00; /* ttl 60 */
    dns[p++] = 0x00;
    dns[p++] = 0x00;
    dns[p++] = 0x3C;
    dns[p++] = 0x00; /* rdlen 4 */
    dns[p++] = 0x04;
    dns[p++] = answer_ip[0];
    dns[p++] = answer_ip[1];
    dns[p++] = answer_ip[2];
    dns[p++] = answer_ip[3];

    uint16_t dns_len = (uint16_t)p;
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + dns_len);
    udp->sport = htons(DNS_PORT);
    udp->dport = htons(dport);
    udp->len = htons(udp_len);
    udp->checksum = 0;

    /* Fill the IP header directly (ip_fill would force src=my_ip; a response
     * must carry the sender's src). dns_parse checks ver_ihl/proto/src only. */
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + udp_len));
    ip->id = 0;
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip->checksum = 0;
    for (int i = 0; i < 4; i++) {
        ip->src[i] = src_ip[i];
        ip->dst[i] = 0;
    }
    return (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + udp_len);
}

/* Boot self-test for the DNS answer-binding fix (adversarial bug-hunt of the
 * untrusted-input surface). Proves dns_parse binds a response to the query on
 * source IP and destination port, not just a (formerly constant) txid:
 *   1. a legitimate answer (from IP_DNS, to our port, our txid) is ACCEPTED;
 *   2. a spoofed SOURCE (attacker IP) is REJECTED;
 *   3. a wrong DEST PORT (blind attacker guessed wrong) is REJECTED.
 * Pure in-memory parser test (no NIC), so it runs on every boot. Returns 0 on
 * success. Anti-vacuous: without the binding checks, cases 2 and 3 are accepted
 * (return 1), the self-test fails, and the boot never prints its gate line. */
int net_dns_guard_selftest(void) {
    static const uint8_t good_src[4] = {10, 0, 2, 3}; /* == IP_DNS (the resolver) */
    static const uint8_t evil_src[4] = {6, 6, 6, 6};
    static const uint8_t answer[4] = {1, 2, 3, 4};
    const uint16_t txid = 0x1234;
    const uint16_t sport = 0xC117; /* our query's ephemeral source port */
    uint8_t buf[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + 32];
    uint8_t out[4];

    uint16_t n = dns_build_response(buf, good_src, sport, txid, answer);
    if (dns_parse(buf, n, txid, sport, out) != 1 || !ip_eq(out, answer)) {
        return -1; /* a legitimate answer must be accepted */
    }
    n = dns_build_response(buf, evil_src, sport, txid, answer);
    if (dns_parse(buf, n, txid, sport, out) != 0) {
        return -2; /* spoofed source IP must be rejected */
    }
    n = dns_build_response(buf, good_src, (uint16_t)(sport ^ 1u), txid, answer);
    if (dns_parse(buf, n, txid, sport, out) != 0) {
        return -3; /* wrong destination port must be rejected */
    }
    return 0;
}

/* Resolve `host` to an IPv4 A record via SLIRP's DNS proxy. Bounded,
 * pumps the RX queue (must run in the IF=1 net thread, not a cli'd
 * syscall). Returns 0 and fills out_ip on success, -1 on failure.
 * Requires a NIC and a DHCP lease (for the unicast source address). */
static int dns_resolve(const char *host, uint16_t txid, uint16_t sport, uint8_t *out_ip) {
    if (!netdev_present() || !net_has_addr()) {
        return -1;
    }
    uint8_t dns_mac[ETH_ADDR_LEN];
    if (!resolve_mac(IP_DNS, dns_mac)) {
        return -1;
    }
    uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + 256];
    uint16_t n = dns_build(out, dns_mac, host, txid, sport);
    if (n == 0) {
        return -1;
    }
    netdev_transmit(out, n);

    uint8_t frame[NET_FRAME_MAX];
    for (int tries = 0; tries < 400; tries++) {
        uint16_t m = net_rx(frame, sizeof(frame));
        if (m > 0 && dns_parse(frame, m, txid, sport, out_ip)) {
            return 0;
        }
        if (m == 0) {
            if (tries > 0 && tries % 100 == 0) {
                netdev_transmit(out, n);
            }
            __asm__ volatile("hlt");
        }
    }
    return -1;
}

/* On-link (SLIRP's 10.0.2.0/24) destinations get ARPed directly; anything
 * else routes via the gateway's MAC. Shared by the UDP drain and TCP. */
const uint8_t *net_next_hop(const uint8_t *dip) {
    /* On-link (same /24) once we have ANY source address — DHCP or static. */
    if (net_has_addr() && dip[0] == my_ip[0] && dip[1] == my_ip[1] && dip[2] == my_ip[2]) {
        return dip;
    }
    /* Static mode has NO gateway (raw peer L2): an off-link destination is
     * unroutable — return NULL so callers drop rather than ARP the
     * nonexistent 10.0.2.2 forever. DHCP mode keeps the SLIRP gateway. */
    if (static_link && !dhcp_have_lease) {
        return 0;
    }
    return IP_GATEWAY;
}

/* ---- ring-3 resolve service (epic #73 follow-up, #77) ----
 *
 * A syscall runs with interrupts disabled, so it cannot pump the RX ring
 * (the NIC IRQ can never fire) — DNS must happen in the IF=1 net thread.
 * SYS_RESOLVE posts a hostname here and the net thread services it; the
 * user polls. Single writer per field on one CPU, `resolve_state` (a
 * volatile, written last on post) is the handshake flag. */
#define RESOLVE_IDLE 0
#define RESOLVE_PENDING 1
#define RESOLVE_DONE 2
#define RESOLVE_FAILED 3
#define RESOLVE_HOST_MAX 64

static volatile int resolve_state;
static char resolve_host[RESOLVE_HOST_MAX];
static uint8_t resolve_ip[4];

/* Per-query DNS transaction id + ephemeral source port, drawn from the
 * qseed-mixed kernel PRNG. Source-port + txid randomization is the barrier a
 * blind on-link spoofer must clear (it cannot see the query), replacing the old
 * constant txid 0x2000 / sport 40000 that made every lookup trivially forgeable.
 * The ephemeral port stays in the IANA 49152..65535 range and is never 0. */
static uint16_t dns_rand_txid(void) {
    return (uint16_t)quantum_kernel_rand();
}
static uint16_t dns_rand_sport(void) {
    return (uint16_t)(0xC000u | (quantum_kernel_rand() & 0x3FFFu));
}

int net_ready(void) {
    return netdev_present() && net_has_addr();
}

int net_nic_present(void) {
    return netdev_present();
}

/* Post a resolve request (from SYS_RESOLVE). Returns 0 accepted, -1 if
 * the network isn't ready or a request is already in flight. */
int net_request_resolve(const char *host) {
    if (!net_ready()) {
        return -1;
    }
    if (resolve_state == RESOLVE_PENDING) {
        return -1; /* one at a time */
    }
    int i = 0;
    while (i < RESOLVE_HOST_MAX - 1 && host[i]) {
        resolve_host[i] = host[i];
        i++;
    }
    resolve_host[i] = '\0';
    resolve_state = RESOLVE_PENDING; /* written last — the handshake */
    return 0;
}

/* Poll the outcome (from SYS_RESOLVE). Returns 1 done (out_ip filled),
 * 0 still pending, -1 failed (also resets to idle). */
int net_poll_resolve(uint8_t *out_ip) {
    if (resolve_state == RESOLVE_DONE) {
        for (int i = 0; i < 4; i++) {
            out_ip[i] = resolve_ip[i];
        }
        resolve_state = RESOLVE_IDLE;
        return 1;
    }
    if (resolve_state == RESOLVE_FAILED) {
        resolve_state = RESOLVE_IDLE;
        return -1;
    }
    return 0;
}

/* Net thread body after the self-test: service ring-3 requests forever.
 * Wakes on every timer tick (hlt). Order per wake: retire CLOSING socket
 * slots (safe here — by construction not mid-copy), pump the RX queue
 * (net_rx demuxes to bound UDP sockets AND the TCP connection; frames for
 * nobody are dropped), drain queued SENDTOs, drive the TCP state machine
 * (connect/retransmit/close/TIME_WAIT), then service a pending resolve. */
void net_service_loop(void) {
    uint8_t frame[NET_FRAME_MAX];
    for (;;) {
        udp_retire_closing();
        while (net_rx(frame, sizeof(frame)) > 0) {
        }
        udp_tx_drain();
        tcp_service();
        if (resolve_state == RESOLVE_PENDING) {
            uint8_t ip[4];
            if (dns_resolve(resolve_host, dns_rand_txid(), dns_rand_sport(), ip) == 0) {
                for (int i = 0; i < 4; i++) {
                    resolve_ip[i] = ip[i];
                }
                resolve_state = RESOLVE_DONE;
            } else {
                resolve_state = RESOLVE_FAILED;
            }
        }
        __asm__ volatile("hlt");
    }
}

/* ---- self-test: ARP then DHCP ---- */

static void log_ip(const char *label, const uint8_t *ip) {
    char b[48];
    int o = 0;
    while (*label) {
        b[o++] = *label++;
    }
    for (int i = 0; i < 4; i++) {
        unsigned v = ip[i];
        char t[3];
        int n = 0;
        if (v == 0) {
            t[n++] = '0';
        }
        while (v) {
            t[n++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (n) {
            b[o++] = t[--n];
        }
        if (i < 3) {
            b[o++] = '.';
        }
    }
    b[o] = 0;
    boot_log(b);
}

void net_selftest(void) {
    if (!netdev_present()) {
        boot_log("NET: no NIC — self-test skipped");
        return;
    }

    /* Static mode (epic #97): there is no SLIRP gateway, DHCP server, or
     * DNS on a raw peer L2 — the SLIRP-oriented phases below would each
     * burn a multi-second doomed timeout. Announce the static address
     * (un-typeable provenance) and skip them. The ARP RESPONDER
     * (net_rx/arp_maybe_reply) is already live, so a peer can reach us. */
    if (static_link) {
        log_ip("NET: static ip ", my_ip);
        boot_log("NET: static mode — DHCP/gateway self-test skipped");
        return;
    }

    /* Phase 1: ARP-resolve the gateway (link-layer proof). We have no
     * DHCP lease yet, but SLIRP answers ARP regardless of the requester's
     * address, so use the address SLIRP assigns (10.0.2.15) as sender. */
    static const uint8_t IP_PRE_DHCP[4] = {10, 0, 2, 15};
    arp_send_request(IP_GATEWAY, IP_PRE_DHCP);

    uint8_t frame[NET_FRAME_MAX];
    uint8_t gw_mac[ETH_ADDR_LEN];
    int arp_ok = 0;
    for (int tries = 0; tries < 200 && !arp_ok; tries++) {
        uint16_t n = net_rx(frame, sizeof(frame));
        if (n > 0 && arp_is_reply_from(frame, n, IP_GATEWAY, gw_mac)) {
            boot_log("NET: ARP 10.0.2.2 is at MAC:");
            for (int i = 0; i < ETH_ADDR_LEN; i++) {
                early_console_write_hex(gw_mac[i]);
            }
            arp_cache_put(IP_GATEWAY, gw_mac); /* warm the cache for the TX drain */
            arp_ok = 1;
        } else if (n == 0) {
            if (tries > 0 && tries % 64 == 0) {
                arp_send_request(IP_GATEWAY, IP_PRE_DHCP);
            }
            __asm__ volatile("hlt");
        }
    }
    if (!arp_ok) {
        boot_log("NET: ARP 10.0.2.2 timed out (no reply)");
    }

    /* Phase 2: full DHCP exchange (DISCOVER -> OFFER -> REQUEST -> ACK).
     * Getting the lease exercises Ethernet, IPv4, UDP, checksums, and
     * broadcast, both directions. */
    {
        uint8_t
            out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_hdr_t) + 32];
        uint16_t n = dhcp_build(out, DHCP_DISCOVER, IP_ZERO, IP_ZERO);
        if (netdev_transmit(out, n) == 0) {
            dhcp_state = 1;
        }
    }
    for (int tries = 0; tries < 600 && !dhcp_have_lease; tries++) {
        uint16_t n = net_rx(frame, sizeof(frame));
        if (n > 0) {
            dhcp_rx(frame, n);
        } else {
            if (tries > 0 && tries % 128 == 0 && dhcp_state == 1) {
                /* Re-DISCOVER if the offer was lost. */
                uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + sizeof(udp_hdr_t) +
                            sizeof(dhcp_hdr_t) + 32];
                uint16_t m = dhcp_build(out, DHCP_DISCOVER, IP_ZERO, IP_ZERO);
                netdev_transmit(out, m);
            }
            __asm__ volatile("hlt");
        }
    }
    if (dhcp_have_lease) {
        log_ip("NET: DHCP lease ", my_ip);
    } else {
        boot_log("NET: DHCP timed out (no lease)");
    }

    /* Phase 3 needs a lease (unicast IP source) and the gateway's MAC. */
    if (!arp_ok || !dhcp_have_lease) {
        return;
    }

    /* Phase 3a: ICMP echo to the gateway. SLIRP answers ping to 10.0.2.2
     * internally, so this is a fully self-contained round trip that
     * exercises unicast IPv4 + ICMP + the header checksum both ways. */
    {
        uint16_t ident = 0xBEEF, seq = 1;
        uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 40];
        uint16_t n = icmp_build(out, IP_GATEWAY, gw_mac, ident, seq);
        netdev_transmit(out, n);

        int pong = 0;
        for (int tries = 0; tries < 300 && !pong; tries++) {
            uint16_t m = net_rx(frame, sizeof(frame));
            if (m > 0 && icmp_is_reply(frame, m, IP_GATEWAY, ident, seq)) {
                pong = 1;
            } else if (m == 0) {
                if (tries > 0 && tries % 64 == 0) {
                    netdev_transmit(out, n);
                }
                __asm__ volatile("hlt");
            }
        }
        boot_log(pong ? "NET: ping 10.0.2.2 reply received" : "NET: ping 10.0.2.2 timed out");
    }

    /* Phase 3b: resolve a hostname through SLIRP's DNS proxy (10.0.2.3),
     * which forwards to the host resolver. Getting a well-formed A record
     * back is the capstone: ARP -> IPv4 -> UDP -> DNS, both directions. */
    {
        uint8_t resolved[4];
        if (dns_resolve("example.com", dns_rand_txid(), dns_rand_sport(), resolved) == 0) {
            log_ip("NET: DNS example.com -> ", resolved);
        } else {
            boot_log("NET: DNS example.com timed out (no answer)");
        }
    }
}
