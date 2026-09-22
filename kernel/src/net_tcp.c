/**
 * QuantumOS ring-3 TCP (epics #82 client, #98 server) — split out of net.c.
 *
 * Two static connections and their net-thread state machine: `tcb` is the
 * single active-open (client) connection, `tcb_srv` the single passive-open
 * (server) connection. All cross-layer wire structs, byte-order helpers and
 * the IPv4/ARP output spine (ip_fill / net_next_hop / resolve_mac) come
 * from net_internal.h; the design notes are on the section comment just
 * below.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/net_internal.h>
#include <kernel/interrupts.h> /* timer_get_ticks (TCP timers) */

/* ============================================================================
 * ring-3 TCP (epic #82 client, epic #98 server)
 *
 * One active-open connection and one passive-open connection at a time
 * (two static TCBs — the same "one in flight" honesty as the resolver,
 * per side). The two are fully disjoint by construction: the client's
 * local port is ephemeral (>= 49152), the server's is its listen port,
 * so no single segment can 4-tuple-match both. The IF=1 net thread owns
 * the ENTIRE state machine: all segment TX, timers, and retransmission
 * run there (a syscall can't touch the NIC). Syscalls only read/write
 * shared TCB fields under the publish discipline and post one-shot
 * request flags (connect_req / listen_req / close_req / abort_req) that
 * the net thread services and clears — independent flags so a reaper's
 * ABORT can never be clobbered by a syscall's CLOSE (the design-attack
 * blocker).
 *
 * Honest scope: stop-and-wait send (one outstanding segment <= MSS),
 * in-order receive only (a segment must start exactly at rcv_nxt;
 * anything else is dropped and the peer retransmits), window = free
 * receive-ring space, short TIME_WAIT (~1s, not 2MSL). The server
 * accepts ONE connection at a time: a SYN arriving while it is busy
 * matches nothing and is silently dropped — the peer's own SYN retry
 * lands after the next re-listen. At most one half-open can exist
 * (SYN_RCVD occupies the single server TCB) and it self-heals: SYN|ACK
 * retransmit exhaustion reaps it back to LISTEN. No congestion control,
 * no out-of-order reassembly, no options sent but MSS.
 * ============================================================================ */

#define IP_PROTO_TCP 6
#define TCP_MSS 1460
#define TCP_RX_BUF 8192 /* power of two: free-running index & (size-1) */
#define TCP_EPHEMERAL_BASE 49152
#define TCP_RETX_TICKS 50      /* ~0.5s at 100 Hz between (re)transmits */
#define TCP_MAX_RETRIES 8      /* then the connection enters ERROR */
#define TCP_TIMEWAIT_TICKS 100 /* ~1s (documented deviation from 2MSL) */

/* TCP flags. */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

/* Connection states. */
#define TCPS_CLOSED 0
#define TCPS_SYN_SENT 1
#define TCPS_ESTABLISHED 2
#define TCPS_FIN_WAIT_1 3
#define TCPS_FIN_WAIT_2 4
#define TCPS_CLOSING 5
#define TCPS_CLOSE_WAIT 6
#define TCPS_LAST_ACK 7
#define TCPS_TIME_WAIT 8
#define TCPS_ERROR 9
#define TCPS_LISTEN 10   /* server: armed, no peer yet (epic #98) */
#define TCPS_SYN_RCVD 11 /* server: SYN|ACK sent, awaiting the ACK */

/* Signed 32-bit sequence comparison (wrap-safe): a is "after" b iff the
 * signed difference is positive. */
#define SEQ_GT(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)

typedef struct {
    volatile int state; /* MUST stay the first field: the reset helpers
                         * zero everything AFTER it, then store it last */
    uint32_t owner_pid;
    uint8_t rip[4];
    uint16_t rport, lport;
    uint8_t nexthop_mac[ETH_ADDR_LEN];
    int have_mac;

    uint32_t iss;              /* our initial send seq */
    uint32_t snd_una, snd_nxt; /* unacked / next — wrapping */
    uint32_t rcv_nxt;          /* next expected receive seq */

    uint8_t rx[TCP_RX_BUF];
    volatile uint32_t rx_head; /* producer: net thread demux */
    volatile uint32_t rx_tail; /* consumer: RECV syscall */

    uint8_t tx[TCP_MSS];
    uint16_t tx_len;
    volatile int tx_pending; /* syscall stages data + sets 1; net thread clears on ACK */

    int fin_sent;     /* our FIN transmitted (occupies one seq) */
    int fin_received; /* peer FIN consumed */
    uint32_t last_adv_wnd;

    uint32_t retx_tick; /* net thread only: last (re)transmit */
    int retries;
    uint32_t timer_tick; /* net thread only: TIME_WAIT clock */

    volatile int connect_req, close_req, abort_req;
    uint8_t creq_ip[4]; /* pending connect params (written before connect_req) */
    uint16_t creq_port;

    volatile int listen_req; /* server: arm a listener (epic #98) */
    uint16_t lreq_port;      /* pending listen port (written before listen_req) */
} tcp_conn_t;

static tcp_conn_t tcb;     /* the one active-open (client) connection */
static tcp_conn_t tcb_srv; /* the one passive-open (server) connection */
static uint16_t tcp_ephemeral_next = TCP_EPHEMERAL_BASE;

/* TCP checksum over the pseudo-header ++ segment as ONE running sum
 * (checksum16 finalizes — it cannot be composed across two buffers).
 * Returns 0 for a valid received segment (its stored checksum in place)
 * and the value to store for a transmitted one (checksum field zeroed). */
static uint16_t tcp_cksum(const uint8_t *sip, const uint8_t *dip, const uint8_t *seg,
                          uint16_t seg_len) {
    uint32_t sum = 0;
    sum += (uint32_t)((sip[0] << 8) | sip[1]);
    sum += (uint32_t)((sip[2] << 8) | sip[3]);
    sum += (uint32_t)((dip[0] << 8) | dip[1]);
    sum += (uint32_t)((dip[2] << 8) | dip[3]);
    sum += IP_PROTO_TCP;
    sum += seg_len;
    uint32_t i = 0;
    while (i + 1 < seg_len) {
        sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
        i += 2;
    }
    if (i < seg_len) {
        sum += (uint32_t)(seg[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Free space in the receive ring = the window we advertise. */
static uint32_t tcp_rx_free(const tcp_conn_t *c) {
    return TCP_RX_BUF - (c->rx_head - c->rx_tail);
}

/* Build and transmit one segment (net thread only). `seq` is the sequence
 * number to stamp; `flags` the control bits; `payload`/`plen` the data
 * (<= MSS); `with_mss` adds the MSS option (SYN and SYN|ACK, data offset
 * 6). The static frame buffer is safe: both connections are serviced on
 * the single net thread, calls strictly sequential. */
static void tcp_xmit(tcp_conn_t *c, uint32_t seq, uint8_t flags, const uint8_t *payload,
                     uint16_t plen, int with_mss) {
    if (!c->have_mac) {
        return;
    }
    static uint8_t out[sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 24 + TCP_MSS]; /* net thread only */
    eth_hdr_t *eth = (eth_hdr_t *)out;
    ip_hdr_t *ip = (ip_hdr_t *)(out + sizeof(eth_hdr_t));
    uint8_t *tcp = (uint8_t *)ip + sizeof(ip_hdr_t);

    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        eth->dst[i] = c->nexthop_mac[i];
        eth->src[i] = self_mac[i];
    }
    eth->type = htons(ETH_TYPE_IP);

    uint8_t doff = with_mss ? 6 : 5;
    uint16_t thlen = (uint16_t)(doff * 4);
    for (uint16_t i = 0; i < thlen; i++) {
        tcp[i] = 0;
    }
    tcp[0] = (uint8_t)(c->lport >> 8);
    tcp[1] = (uint8_t)c->lport;
    tcp[2] = (uint8_t)(c->rport >> 8);
    tcp[3] = (uint8_t)c->rport;
    tcp[4] = (uint8_t)(seq >> 24);
    tcp[5] = (uint8_t)(seq >> 16);
    tcp[6] = (uint8_t)(seq >> 8);
    tcp[7] = (uint8_t)seq;
    tcp[8] = (uint8_t)(c->rcv_nxt >> 24);
    tcp[9] = (uint8_t)(c->rcv_nxt >> 16);
    tcp[10] = (uint8_t)(c->rcv_nxt >> 8);
    tcp[11] = (uint8_t)c->rcv_nxt;
    tcp[12] = (uint8_t)(doff << 4);
    tcp[13] = flags;
    uint32_t wnd = tcp_rx_free(c);
    if (wnd > 0xFFFF) {
        wnd = 0xFFFF;
    }
    tcp[14] = (uint8_t)(wnd >> 8);
    tcp[15] = (uint8_t)wnd;
    /* checksum (16,17) and urgent (18,19) already zero */
    if (with_mss) {
        tcp[20] = 2; /* MSS option kind */
        tcp[21] = 4; /* length */
        tcp[22] = (uint8_t)(TCP_MSS >> 8);
        tcp[23] = (uint8_t)TCP_MSS;
    }
    for (uint16_t i = 0; i < plen; i++) {
        tcp[thlen + i] = payload[i];
    }
    uint16_t seg_len = (uint16_t)(thlen + plen);
    uint16_t csum = tcp_cksum(my_ip, c->rip, tcp, seg_len);
    tcp[16] = (uint8_t)(csum >> 8);
    tcp[17] = (uint8_t)csum;

    ip_fill(ip, c->rip, IP_PROTO_TCP, seg_len, (uint16_t)(0x8000 + (seq & 0x7FFF)));
    netdev_transmit(out, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + seg_len));

    /* Every ACK-bearing segment refreshes the advertised window. */
    if (flags & TCP_ACK) {
        c->last_adv_wnd = wnd;
    }
}

/* Send a bare ACK of the current rcv_nxt (also the window refresh). */
static void tcp_ack(tcp_conn_t *c) {
    tcp_xmit(c, c->snd_nxt, TCP_ACK, NULL, 0, 0);
}

/* Zero the whole TCB and return it to CLOSED (net thread only). The
 * leading `state` field is zeroed LAST, behind a publish barrier: a
 * preempting syscall that reads `state` mid-zero still sees the old
 * (busy) state and keeps polling, instead of posting a request into a
 * half-zeroed TCB (the epic-#98 design-attack blocker). A pristine
 * CLOSED TCB is the ONLY claimable state — the UDP three-state lesson. */
static void tcp_reset_closed(tcp_conn_t *c) {
    uint8_t *p = (uint8_t *)c;
    for (uint32_t i = sizeof(c->state); i < sizeof(*c); i++) {
        p[i] = 0;
    }
    publish_barrier();
    c->state = TCPS_CLOSED;
}

/* Return the server connection to a clean LISTEN (net thread only),
 * preserving owner_pid and lport — reusing tcp_reset_closed here would
 * strand the owner polling an unowned TCB and leave lport zeroed so no
 * SYN could ever match again. Same store-state-last discipline. */
static void tcp_rearm_listen(tcp_conn_t *c) {
    uint32_t owner = c->owner_pid;
    uint16_t lport = c->lport;
    uint8_t *p = (uint8_t *)c;
    for (uint32_t i = sizeof(c->state); i < sizeof(*c); i++) {
        p[i] = 0;
    }
    c->owner_pid = owner;
    c->lport = lport;
    publish_barrier();
    c->state = TCPS_LISTEN;
}

/* Feed one validated TCP segment to connection `c` (net thread only).
 * Returns 1 if the segment belonged to this connection (consumed or
 * deliberately dropped), 0 if it did not match its 4-tuple / listen
 * port. ONE ordered pass: RST, then handshake states, then the ACK
 * field, then in-order payload, then the FIN flag — never mutually-
 * exclusive branches (a data+FIN segment must have both consumed and
 * one ACK covering both). */
static int tcp_rx_one(tcp_conn_t *c, const eth_hdr_t *eth, const ip_hdr_t *ip, uint16_t sport,
                      uint16_t dport, uint8_t flags, uint32_t seq, uint32_t ack,
                      const uint8_t *payload, uint16_t payload_len) {
    int st = c->state;
    if (st == TCPS_CLOSED || st == TCPS_ERROR) {
        return 0; /* ERROR is terminal for the net thread until abort retires it */
    }

    if (st == TCPS_LISTEN) {
        /* Passive open (epic #98): a clean SYN to our listening port
         * claims the connection. RST- or ACK-bearing segments never
         * match, so a forged SYN|RST cannot burn the half-open slot and
         * a client SYN|ACK expected by SYN_SENT can never land here. */
        if (dport != c->lport || (flags & (TCP_SYN | TCP_ACK | TCP_RST)) != TCP_SYN) {
            return 0;
        }
        for (int i = 0; i < 4; i++) {
            c->rip[i] = ip->src[i];
        }
        c->rport = sport;
        /* The frame that reached us proves the L2 return path: reply to
         * its source MAC (under SLIRP the gateway, on a socket netdev
         * the peer NIC) — never call the blocking ARP pump from demux. */
        for (int i = 0; i < ETH_ADDR_LEN; i++) {
            c->nexthop_mac[i] = eth->src[i];
        }
        c->have_mac = 1;
        c->rcv_nxt = seq + 1;
        c->iss = (uint32_t)timer_get_ticks() * 2654435761u + 0x2000;
        c->snd_una = c->snd_nxt = c->iss + 1; /* the SYN|ACK occupies iss */
        c->retries = 0;
        c->retx_tick = (uint32_t)timer_get_ticks();
        c->state = TCPS_SYN_RCVD;
        tcp_xmit(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0, 1);
        return 1;
    }

    /* 4-tuple match against this connection. */
    if (!ip_eq(ip->src, c->rip)) {
        return 0;
    }
    if (sport != c->rport || dport != c->lport) {
        return 0;
    }

    if (flags & TCP_RST) {
        /* RFC 5961: a blind/off-path RST that merely matches the 4-tuple must
         * NOT tear down the connection — validate the sequence first. In
         * SYN_SENT the RST must ACK our SYN; in every synchronized state it
         * must sit exactly at rcv_nxt (this stack accepts in-order only).
         * An out-of-window RST is dropped. */
        int rst_ok;
        if (st == TCPS_SYN_SENT) {
            rst_ok = (flags & TCP_ACK) && ack == c->snd_nxt;
        } else {
            rst_ok = (seq == c->rcv_nxt);
        }
        if (!rst_ok) {
            return 1; /* out-of-window RST — ignore */
        }
        if (st == TCPS_SYN_RCVD) {
            tcp_rearm_listen(c); /* a failed half-open must not kill the server */
        } else {
            c->state = TCPS_ERROR;
        }
        return 1;
    }

    /* SYN_SENT: the only acceptable segment is our SYN|ACK. */
    if (st == TCPS_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->state = TCPS_ESTABLISHED;
            tcp_ack(c); /* completes the handshake */
        }
        return 1;
    }

    /* SYN_RCVD: either a duplicate of the opening SYN (our SYN|ACK was
     * lost — resend the FULL SYN|ACK, and only for the SAME sequence, so
     * a rebooted peer's fresh SYN is not answered with a stale one), or
     * the completing ACK. */
    if (st == TCPS_SYN_RCVD) {
        if ((flags & (TCP_SYN | TCP_ACK)) == TCP_SYN) {
            if (seq + 1 == c->rcv_nxt) {
                tcp_xmit(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0, 1);
            }
            return 1;
        }
        if (!(flags & TCP_ACK) || ack != c->snd_nxt) {
            return 1; /* not the completing ACK — drop */
        }
        c->state = TCPS_ESTABLISHED;
        /* FALL THROUGH — unlike SYN_SENT, the completing ACK may carry
         * the peer's first data and even its FIN (curl sends the GET
         * immediately); the ordered pass below consumes both in this
         * same call. */
    }

    /* (1) ACK field — advance snd_una (forward only, wrap-safe). */
    if (flags & TCP_ACK) {
        int advanced = 0;
        if (SEQ_GT(ack, c->snd_una) && !SEQ_GT(ack, c->snd_nxt)) {
            c->snd_una = ack;
            advanced = 1;
        }
        /* Clear tx_pending ONLY when this ACK actually advanced snd_una. A
         * pure/duplicate ACK finds snd_una == snd_nxt whenever the app has
         * STAGED data that the net thread has not transmitted yet (net_tcp_send
         * sets tx_pending but leaves snd_nxt until tcp_service sends), so an
         * unconditional clear here silently drops the staged response — e.g. a
         * half-closing client's FIN|ACK landing between stage and transmit. */
        if (advanced && c->snd_una == c->snd_nxt) {
            c->tx_pending = 0; /* our outstanding data (or FIN) fully acknowledged */
            if (c->fin_sent) {
                if (c->state == TCPS_FIN_WAIT_1) {
                    c->state = TCPS_FIN_WAIT_2;
                } else if (c->state == TCPS_LAST_ACK) {
                    tcp_reset_closed(c); /* clean close complete */
                    return 1;
                } else if (c->state == TCPS_CLOSING) {
                    c->state = TCPS_TIME_WAIT;
                    c->timer_tick = (uint32_t)timer_get_ticks();
                }
            }
        }
    }

    /* (2) in-order payload. */
    if (payload_len > 0 && seq == c->rcv_nxt) {
        if (payload_len <= tcp_rx_free(c)) {
            uint32_t head = c->rx_head;
            for (uint16_t i = 0; i < payload_len; i++) {
                c->rx[(head + i) & (TCP_RX_BUF - 1)] = payload[i];
            }
            publish_barrier();
            c->rx_head = head + payload_len;
            c->rcv_nxt += payload_len;
        }
        /* else ring full: drop, don't advance — the ACK below advertises
         * the (small) window and the peer retransmits. */
    }

    /* (3) FIN — only once every preceding byte is in-order-consumed, i.e.
     * the FIN's sequence number equals the (possibly just-advanced)
     * rcv_nxt. */
    if ((flags & TCP_FIN) && (seq + payload_len) == c->rcv_nxt) {
        c->rcv_nxt += 1;
        c->fin_received = 1;
        if (c->state == TCPS_ESTABLISHED) {
            c->state = TCPS_CLOSE_WAIT;
        } else if (c->state == TCPS_FIN_WAIT_1) {
            c->state = TCPS_CLOSING;
        } else if (c->state == TCPS_FIN_WAIT_2) {
            c->state = TCPS_TIME_WAIT;
            c->timer_tick = (uint32_t)timer_get_ticks();
        }
    }

    /* (4) ACK anything that occupied sequence space (data or FIN) OR an
     * old/duplicate segment — a pure ACK gets no reply (no ACK storm).
     * One ACK, reflecting the final rcv_nxt (covers data and FIN). */
    if (payload_len > 0 || (flags & TCP_FIN)) {
        tcp_ack(c);
    }
    return 1;
}

/* Feed one received frame to the connections (net thread only, via
 * net_rx). Header validation, the checksum, and field extraction happen
 * ONCE, before any connection can match — so a corrupt SYN can never
 * burn the single server half-open slot. Client first, then server: the
 * two 4-tuples are disjoint (ephemeral vs listen lport), so at most one
 * consumes the segment. */
void tcp_rx_demux(const uint8_t *frame, uint16_t len) {
    if (len < sizeof(eth_hdr_t) + sizeof(ip_hdr_t) + 20) {
        return;
    }
    const eth_hdr_t *eth = (const eth_hdr_t *)frame;
    if (ntohs(eth->type) != ETH_TYPE_IP) {
        return;
    }
    const ip_hdr_t *ip = (const ip_hdr_t *)(frame + sizeof(eth_hdr_t));
    if (ip->ver_ihl != 0x45 || ip->proto != IP_PROTO_TCP) {
        return;
    }
    if (ntohs(ip->frag) & 0x3FFF) {
        return; /* fragment — no reassembly */
    }
    if (!ip_eq(ip->dst, my_ip)) {
        return; /* not addressed to us (also parks pre-DHCP segments) */
    }
    uint16_t total = ntohs(ip->total_len);
    if (total < sizeof(ip_hdr_t) + 20) {
        return;
    }
    if ((uint32_t)sizeof(eth_hdr_t) + total > len) {
        return; /* claims more than the frame carries */
    }
    /* seg length from IP total_len, NEVER the frame length — control
     * segments (54B) are padded to the 60B Ethernet minimum. */
    uint16_t seg_len = (uint16_t)(total - sizeof(ip_hdr_t));
    const uint8_t *tcp = frame + sizeof(eth_hdr_t) + sizeof(ip_hdr_t);
    if (tcp_cksum(ip->src, ip->dst, tcp, seg_len) != 0) {
        return; /* mandatory checksum — SLIRP computes real ones */
    }
    uint8_t doff = (uint8_t)(tcp[12] >> 4);
    if (doff < 5 || (uint16_t)(doff * 4) > seg_len) {
        return; /* validate BEFORE computing payload_len (no underflow) */
    }
    uint16_t sport = (uint16_t)((tcp[0] << 8) | tcp[1]);
    uint16_t dport = (uint16_t)((tcp[2] << 8) | tcp[3]);
    uint8_t flags = tcp[13];
    uint32_t seq =
        ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) | ((uint32_t)tcp[6] << 8) | tcp[7];
    uint32_t ack =
        ((uint32_t)tcp[8] << 24) | ((uint32_t)tcp[9] << 16) | ((uint32_t)tcp[10] << 8) | tcp[11];
    uint16_t payload_off = (uint16_t)(doff * 4);
    uint16_t payload_len = (uint16_t)(seg_len - payload_off);
    const uint8_t *payload = tcp + payload_off;

    if (tcp_rx_one(&tcb, eth, ip, sport, dport, flags, seq, ack, payload, payload_len)) {
        return;
    }
    tcp_rx_one(&tcb_srv, eth, ip, sport, dport, flags, seq, ack, payload, payload_len);
}

/* Allocate the next TCP ephemeral port (advances every active open so a
 * stale in-flight segment from a prior connection cannot 4-tuple match). */
static uint16_t tcp_ephemeral(void) {
    uint16_t p = tcp_ephemeral_next;
    tcp_ephemeral_next = (uint16_t)(p == 65535 ? TCP_EPHEMERAL_BASE : p + 1);
    return p;
}

/* Net-thread servicing for ONE connection: retire aborts, start connects/
 * listens, drive retransmission/close/TIME_WAIT. Every early return here
 * is per-connection — the caller services the other TCB regardless (a
 * client parked in ERROR must never deafen the server). */
static void tcp_service_one(tcp_conn_t *c, uint32_t now) {
    /* abort_req wins over everything and always retires to CLOSED. A
     * LISTEN-state conn has have_mac == 0, so it retires without an RST. */
    if (c->abort_req) {
        if (c->state != TCPS_CLOSED && c->state != TCPS_SYN_SENT && c->have_mac) {
            tcp_xmit(c, c->snd_nxt, TCP_RST | TCP_ACK, NULL, 0, 0);
        }
        tcp_reset_closed(c);
        return;
    }
    if (c->state == TCPS_ERROR) {
        return; /* terminal until a syscall/cleanup posts abort_req */
    }
    if (c->state == TCPS_CLOSED) {
        if (c->listen_req) {
            c->listen_req = 0;
            c->lport = c->lreq_port;
            c->state = TCPS_LISTEN; /* armed — no packet, no MAC needed yet */
            return;
        }
        if (c->connect_req) {
            c->connect_req = 0;
            for (int i = 0; i < 4; i++) {
                c->rip[i] = c->creq_ip[i];
            }
            c->rport = c->creq_port;
            if (!resolve_mac(net_next_hop(c->rip), c->nexthop_mac)) {
                c->state = TCPS_ERROR;
                return;
            }
            c->have_mac = 1;
            c->iss = (uint32_t)timer_get_ticks() * 2654435761u + 0x1000;
            c->snd_una = c->snd_nxt = c->iss + 1; /* the SYN occupies iss */
            c->rcv_nxt = 0;
            c->state = TCPS_SYN_SENT;
            c->retries = 0;
            c->retx_tick = now;
            tcp_xmit(c, c->iss, TCP_SYN, NULL, 0, 1);
        }
        return;
    }
    if (c->state == TCPS_LISTEN) {
        return; /* nothing to drive — the demux claims connections */
    }

    /* SYN retransmit — state-driven, independent of snd_una/snd_nxt. */
    if (c->state == TCPS_SYN_SENT) {
        if ((uint32_t)(now - c->retx_tick) >= TCP_RETX_TICKS) {
            if (++c->retries > TCP_MAX_RETRIES) {
                c->state = TCPS_ERROR;
                return;
            }
            c->retx_tick = now;
            tcp_xmit(c, c->iss, TCP_SYN, NULL, 0, 1);
        }
        return;
    }

    /* SYN|ACK retransmit — mirror of SYN_SENT, but exhaustion reaps the
     * half-open back to LISTEN (never ERROR): the single half-open is
     * bounded by construction and must self-heal, not wedge the server. */
    if (c->state == TCPS_SYN_RCVD) {
        if ((uint32_t)(now - c->retx_tick) >= TCP_RETX_TICKS) {
            if (++c->retries > TCP_MAX_RETRIES) {
                tcp_rearm_listen(c);
                return;
            }
            c->retx_tick = now;
            tcp_xmit(c, c->iss, TCP_SYN | TCP_ACK, NULL, 0, 1);
        }
        return;
    }

    /* Data: transmit fresh, or retransmit an outstanding segment. */
    if (c->tx_pending && (c->state == TCPS_ESTABLISHED || c->state == TCPS_CLOSE_WAIT)) {
        if (c->snd_una == c->snd_nxt) { /* fresh (nothing outstanding) */
            tcp_xmit(c, c->snd_nxt, TCP_PSH | TCP_ACK, c->tx, c->tx_len, 0);
            c->snd_nxt += c->tx_len;
            c->retries = 0;
            c->retx_tick = now;
        } else if ((uint32_t)(now - c->retx_tick) >= TCP_RETX_TICKS) {
            if (++c->retries > TCP_MAX_RETRIES) {
                c->state = TCPS_ERROR;
                return;
            }
            tcp_xmit(c, c->snd_una, TCP_PSH | TCP_ACK, c->tx, c->tx_len, 0);
            c->retx_tick = now;
        }
    }

    /* Graceful close: once our data has drained, send the FIN. */
    if (c->close_req && !c->fin_sent && !c->tx_pending && c->snd_una == c->snd_nxt &&
        (c->state == TCPS_ESTABLISHED || c->state == TCPS_CLOSE_WAIT)) {
        tcp_xmit(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0, 0);
        c->snd_nxt += 1;
        c->fin_sent = 1;
        c->retries = 0;
        c->retx_tick = now;
        c->state = (c->state == TCPS_ESTABLISHED) ? TCPS_FIN_WAIT_1 : TCPS_LAST_ACK;
    }

    /* FIN retransmit. */
    if (c->fin_sent && SEQ_GT(c->snd_nxt, c->snd_una) &&
        (c->state == TCPS_FIN_WAIT_1 || c->state == TCPS_LAST_ACK || c->state == TCPS_CLOSING)) {
        if ((uint32_t)(now - c->retx_tick) >= TCP_RETX_TICKS) {
            if (++c->retries > TCP_MAX_RETRIES) {
                c->state = TCPS_ERROR;
                return;
            }
            c->retx_tick = now;
            tcp_xmit(c, c->snd_nxt - 1, TCP_FIN | TCP_ACK, NULL, 0, 0);
        }
    }

    /* Proactive window update: after the app drains the ring, tell the
     * peer the window reopened (it will not probe promptly on its own). */
    if ((c->state == TCPS_ESTABLISHED || c->state == TCPS_CLOSE_WAIT) &&
        tcp_rx_free(c) >= TCP_MSS && c->last_adv_wnd < TCP_MSS) {
        tcp_ack(c);
    }

    /* TIME_WAIT expiry (shortened). */
    if (c->state == TCPS_TIME_WAIT && (uint32_t)(now - c->timer_tick) >= TCP_TIMEWAIT_TICKS) {
        tcp_reset_closed(c);
    }
}

/* Net-thread TCP servicing: both connections, every service-loop wake. */
void tcp_service(void) {
    uint32_t now = (uint32_t)timer_get_ticks();
    tcp_service_one(&tcb, now);
    tcp_service_one(&tcb_srv, now);
}

/* ---- syscall-facing TCP ops (cli'd context; never touch the NIC) ---- */

/* Resolve the connection `pid` owns — client TCB first (the documented
 * tiebreak; the one-connection-per-pid invariant in connect/listen makes
 * a double match unreachable). NULL if the pid owns neither. */
static tcp_conn_t *tcp_conn_for(uint32_t pid) {
    if (tcb.owner_pid == pid) {
        return &tcb;
    }
    if (tcb_srv.owner_pid == pid) {
        return &tcb_srv;
    }
    return NULL;
}

long net_tcp_connect(uint32_t pid, const uint8_t *ip, uint16_t port) {
    if (!net_nic_present()) {
        return NET_TCP_ENONET;
    }
    if (tcb_srv.owner_pid == pid) {
        return NET_TCP_EINVAL; /* one connection per pid: this pid listens */
    }
    int st = tcb.state;
    if (st == TCPS_ESTABLISHED && tcb.owner_pid == pid) {
        /* Idempotent only while nothing is pending: a recycled pid must
         * not adopt its dead predecessor's half-torn connection. */
        return (tcb.abort_req || tcb.close_req) ? NET_TCP_WOULDBLOCK : 0;
    }
    if (st == TCPS_ERROR && tcb.owner_pid == pid) {
        tcb.abort_req = 1; /* let the net thread retire it, then retry */
        return NET_TCP_EIO;
    }
    if (st == TCPS_CLOSED) {
        if (tcb.connect_req) {
            return (tcb.owner_pid == pid) ? NET_TCP_WOULDBLOCK : NET_TCP_EINVAL;
        }
        for (int i = 0; i < 4; i++) {
            tcb.creq_ip[i] = ip[i];
        }
        tcb.creq_port = port;
        tcb.lport = tcp_ephemeral();
        tcb.owner_pid = pid;
        publish_barrier();
        tcb.connect_req = 1; /* the publish store */
        return NET_TCP_WOULDBLOCK;
    }
    if (tcb.owner_pid == pid) {
        return NET_TCP_WOULDBLOCK; /* SYN_SENT / TIME_WAIT / closing — poll */
    }
    return NET_TCP_EINVAL; /* someone else owns the client connection */
}

/* Arm the single passive-open listener on `port` (epic #98). WOULDBLOCK
 * while arming or while a previous connection drains — the caller polls
 * until 0 (armed or already carrying a connection on that port). The
 * request is re-postable: if the arming store lost a race with a reset,
 * the next poll simply posts it again. */
long net_tcp_listen(uint32_t pid, uint16_t port) {
    if (!net_nic_present()) {
        return NET_TCP_ENONET;
    }
    if (port == 0) {
        return NET_TCP_EINVAL;
    }
    if (tcb.owner_pid == pid) {
        return NET_TCP_EINVAL; /* one connection per pid: this pid connects */
    }
    tcp_conn_t *c = &tcb_srv;
    int st = c->state;
    if (st == TCPS_CLOSED) {
        if (c->listen_req) {
            return (c->owner_pid == pid) ? NET_TCP_WOULDBLOCK : NET_TCP_EINVAL;
        }
        c->owner_pid = pid;
        c->lreq_port = port;
        publish_barrier();
        c->listen_req = 1; /* the publish store */
        return NET_TCP_WOULDBLOCK;
    }
    if (c->owner_pid != pid) {
        /* A foreign owner that died had abort_req posted by its cleanup:
         * poll until the net thread retires the conn, then claim it. A
         * live foreign owner is a real conflict. */
        return c->abort_req ? NET_TCP_WOULDBLOCK : NET_TCP_EINVAL;
    }
    if (c->abort_req || c->close_req) {
        return NET_TCP_WOULDBLOCK; /* draining — the recycled-pid guard */
    }
    if (st == TCPS_LISTEN || st == TCPS_SYN_RCVD || st == TCPS_ESTABLISHED ||
        st == TCPS_CLOSE_WAIT) {
        return (c->lport == port) ? 0 : NET_TCP_EINVAL;
    }
    if (st == TCPS_ERROR) {
        c->abort_req = 1; /* retire, then the next poll re-arms */
        return NET_TCP_WOULDBLOCK;
    }
    return NET_TCP_WOULDBLOCK; /* TIME_WAIT / FIN_WAIT / CLOSING / LAST_ACK */
}

/* Poll for an accepted connection on the armed listener. 0 once a peer
 * is connected (ESTABLISHED, or CLOSE_WAIT — the request bytes are in
 * the ring even if the peer already half-closed); WOULDBLOCK while the
 * listener waits or a handshake runs; EIO in every other state (the
 * caller's recovery is CLOSE then re-LISTEN). */
long net_tcp_accept(uint32_t pid) {
    tcp_conn_t *c = &tcb_srv;
    if (c->owner_pid != pid) {
        return NET_TCP_EINVAL;
    }
    if (c->abort_req || c->close_req) {
        return NET_TCP_WOULDBLOCK; /* draining — poll until retired */
    }
    int st = c->state;
    if (st == TCPS_ESTABLISHED || st == TCPS_CLOSE_WAIT) {
        return 0;
    }
    if (st == TCPS_LISTEN || st == TCPS_SYN_RCVD) {
        return NET_TCP_WOULDBLOCK;
    }
    if (st == TCPS_CLOSED) {
        return c->listen_req ? NET_TCP_WOULDBLOCK : NET_TCP_EIO;
    }
    return NET_TCP_EIO; /* ERROR / closing states — CLOSE then re-LISTEN */
}

long net_tcp_send(uint32_t pid, const uint8_t *buf, uint16_t len) {
    tcp_conn_t *c = tcp_conn_for(pid);
    if (c == NULL) {
        return NET_TCP_EINVAL;
    }
    if (c->state == TCPS_ERROR) {
        return NET_TCP_EIO;
    }
    /* CLOSE_WAIT is a legal send state: a server must be able to reply
     * to a peer that half-closed right after its request (the net
     * thread already transmits staged data in CLOSE_WAIT). */
    if (c->state != TCPS_ESTABLISHED && c->state != TCPS_CLOSE_WAIT) {
        return NET_TCP_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (len > TCP_MSS) {
        len = TCP_MSS;
    }
    if (c->tx_pending) {
        return NET_TCP_WOULDBLOCK; /* a segment is still outstanding */
    }
    for (uint16_t i = 0; i < len; i++) {
        c->tx[i] = buf[i];
    }
    c->tx_len = len;
    publish_barrier();
    c->tx_pending = 1;
    return len;
}

long net_tcp_recv(uint32_t pid, uint8_t *buf, uint16_t maxlen) {
    tcp_conn_t *c = tcp_conn_for(pid);
    if (c == NULL) {
        return NET_TCP_EINVAL;
    }
    uint32_t avail = c->rx_head - c->rx_tail;
    if (avail == 0) {
        if (c->fin_received) {
            return 0; /* EOF — peer closed and the ring is drained */
        }
        if (c->state == TCPS_ERROR) {
            return NET_TCP_EIO;
        }
        if (c->state == TCPS_CLOSED) {
            return NET_TCP_EIO;
        }
        return NET_TCP_WOULDBLOCK;
    }
    uint32_t n = avail;
    if (n > maxlen) {
        n = maxlen;
    }
    uint32_t tail = c->rx_tail;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = c->rx[(tail + i) & (TCP_RX_BUF - 1)];
    }
    publish_barrier();
    c->rx_tail = tail + n;
    return (long)n;
}

long net_tcp_close(uint32_t pid) {
    tcp_conn_t *c = tcp_conn_for(pid);
    if (c == NULL) {
        return 0; /* nothing owned — idempotent */
    }
    if (c->state == TCPS_CLOSED && !c->listen_req && !c->connect_req) {
        return 0;
    }
    if (c->state == TCPS_ERROR || c->state == TCPS_LISTEN || c->state == TCPS_SYN_RCVD ||
        (c->state == TCPS_CLOSED && (c->listen_req || c->connect_req))) {
        c->abort_req = 1; /* no graceful close exists for these — retire */
        return 0;
    }
    c->close_req = 1;
    return NET_TCP_WOULDBLOCK; /* poll until CLOSED */
}

long net_tcp_status(uint32_t pid) {
    tcp_conn_t *c = tcp_conn_for(pid);
    if (c == NULL) {
        return NET_TCP_EINVAL;
    }
    int st = c->state;
    if (st == TCPS_ESTABLISHED) {
        return 0;
    }
    if (st == TCPS_ERROR) {
        return NET_TCP_EIO;
    }
    return NET_TCP_WOULDBLOCK;
}

void net_tcp_cleanup(uint32_t pid) {
    /* `|| tcb.connect_req`: a client that requested connect but whose SYN the
     * net thread has not sent yet is still TCPS_CLOSED — without this its
     * pending connect_req would be serviced AFTER the owner died, opening a
     * connection owned by a dead/recycled pid (the leak the server side already
     * guards with listen_req). */
    if ((tcb.state != TCPS_CLOSED || tcb.connect_req) && tcb.owner_pid == pid) {
        tcb.abort_req = 1; /* the net thread RSTs (if past SYN) and retires */
    }
    if ((tcb_srv.state != TCPS_CLOSED || tcb_srv.listen_req) && tcb_srv.owner_pid == pid) {
        tcb_srv.abort_req = 1; /* covers LISTEN/SYN_RCVD and a pending arm */
    }
}
