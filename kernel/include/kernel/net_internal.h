/**
 * QuantumOS network stack — shared internal surface.
 *
 * Private glue shared by the network translation units after net.c was
 * split by transport: the control plane stays in kernel/src/net.c
 * (Ethernet, ARP, IPv4, ICMP, DHCP, DNS, resolve, boot self-test), the
 * ring-3 UDP sockets live in kernel/src/net_udp.c (epic #80) and the
 * ring-3 TCP client in kernel/src/net_tcp.c (epic #82).
 *
 * Only genuinely cross-TU items belong here — the shared wire structs, the
 * byte-order/utility helpers, the netif globals, and the handful of
 * functions one net TU calls in another. Layer-local headers, constants
 * and helpers stay private to their owning .c file. This is NOT a public
 * API header; user-facing prototypes live in <kernel/net.h>.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include <kernel/types.h>
#include <kernel/netdev.h> /* ETH_ADDR_LEN, NET_FRAME_MAX, netdev_* */
#include <kernel/net.h>    /* public API + UDP_PAYLOAD_MAX + NET_UDP_ and NET_TCP_ codes */

/* The two protocol constants used by more than one net TU. Layer-local
 * values (ETH_TYPE_ARP, ARP_*, IP_PROTO_ICMP/TCP, DHCP_*, DNS ports) stay
 * private in the .c that owns them. */
#define ETH_TYPE_IP 0x0800
#define IP_PROTO_UDP 17

/* Shared helpers as static inline / macros: every TU that includes this
 * header gets its own copy, so an unused one is dropped with no
 * -Werror=unused-function, and there is no multiple-definition at link. */
static inline uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
#define ntohs(v) htons(v)
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

static inline int ip_eq(const uint8_t *a, const uint8_t *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* Order plain stores before the volatile publish store. */
#define publish_barrier() __asm__ volatile("" ::: "memory")

/* ---- shared wire headers (all packed; moved verbatim from net.c) ----
 * arp_pkt_t and dhcp_hdr_t are used only by the control plane and stay
 * private in net.c. */
typedef struct __attribute__((packed)) {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t ver_ihl; /* 0x45: IPv4, 5*4=20 byte header */
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint8_t src[4];
    uint8_t dst[4];
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t checksum;
} udp_hdr_t;

/* Compile-time tripwire: a dropped __attribute__((packed)) or a reordered
 * field would corrupt the wire layout while still building — these catch it. */
_Static_assert(sizeof(eth_hdr_t) == 14, "eth_hdr_t must be 14 bytes on the wire");
_Static_assert(sizeof(ip_hdr_t) == 20, "ip_hdr_t must be 20 bytes on the wire");
_Static_assert(sizeof(udp_hdr_t) == 8, "udp_hdr_t must be 8 bytes on the wire");

/* ---- netif state owned by net.c, read by the transport TUs ---- */
extern uint8_t self_mac[ETH_ADDR_LEN];
extern uint8_t my_ip[4];
extern int dhcp_have_lease;
extern int static_link; /* epic #97: static-IP mode (no DHCP, no gateway) */
int net_has_addr(void); /* dhcp_have_lease || static_link */
extern const uint8_t IP_ZERO[4];
extern const uint8_t IP_BCAST[4];

/* ---- cross-TU functions (definitions in the file named in the comment) ---- */
/* net.c — the IPv4/ARP output spine the transports build on. */
void ip_fill(ip_hdr_t *ip, const uint8_t *dst, uint8_t proto, uint16_t payload_len, uint16_t ident);
const uint8_t *net_next_hop(const uint8_t *dip);
int resolve_mac(const uint8_t *ip, uint8_t *out_mac);
/* net_udp.c — driven by net.c's rx path and service loop. */
void udp_rx_demux(const uint8_t *frame, uint16_t len);
void udp_retire_closing(void);
void udp_tx_drain(void);
/* net_tcp.c — driven by net.c's rx path and service loop. */
void tcp_rx_demux(const uint8_t *frame, uint16_t len);
void tcp_service(void);

#endif /* NET_INTERNAL_H */
