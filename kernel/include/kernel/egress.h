#ifndef EGRESS_H
#define EGRESS_H

#include <kernel/types.h>

/* The egress grant (ADR-0030, #252). Networking is off until the boot command line
 * grants it:
 *
 *   egress=ttl=600;bytes=4194304;allow=10.0.2.2:53/udp,example.com:80/tcp,*;listen=8080
 *
 *   ttl     seconds from boot, from the timer. After it the stack quiesces.
 *   bytes   total frame bytes, transmitted plus received, infrastructure included.
 *   allow   destinations ring 3 may send to: ip|host:port/udp|tcp. A host entry matches
 *           the kernel's own DNS answer for that name. `*` is the wide grant for test
 *           harnesses and benchmarks: any destination, still budgeted and receipted.
 *   listen  local ports inbound may be accepted on (TCP listen, UDP bind). Empty by default.
 *
 * Every verdict is written to the kernel's audit ledger as AUDIT_EGRESS, keyed by
 * (pid, destination, port/proto), so attempted and allowed are both on the record and a
 * citizen can neither forge nor suppress its own row. */

#define EGRESS_PROTO_UDP 17
#define EGRESS_PROTO_TCP 6

/* Verdicts from egress_check / egress_listen_ok (0 = allowed). */
#define EGRESS_OK 0
#define EGRESS_E_NOGRANT (-1) /* no grant was ever given */
#define EGRESS_E_EXPIRED (-2) /* the ttl ran out */
#define EGRESS_E_BUDGET (-3)  /* the byte budget ran out */
#define EGRESS_E_DEST (-4)    /* destination not in the allowlist */
#define EGRESS_E_PORT (-5)    /* destination allowed, but not this port/proto */
#define EGRESS_E_LISTEN (-6)  /* local port not in `listen` */

/* Parse and activate a grant from its spec (no spaces). 0 on success, -1 on a spec the
 * kernel refuses (it logs which field). A second call replaces the grant. */
int egress_configure(const char *spec);

/* A grant was given at boot, whatever its state now. */
int egress_configured(void);

/* The grant is live: given, not expired, budget left. Checks the ttl against the timer
 * and quiesces (logs + receipts) the first time it finds it over. */
int egress_active(void);

/* May `pid` send to dip:dport over proto? Receipted either way. */
int egress_check(uint32_t pid, const uint8_t dip[4], uint16_t dport, uint8_t proto);

/* May `pid` accept inbound on local port `lport`? Receipted either way. */
int egress_listen_ok(uint32_t pid, uint16_t lport);

/* The kernel resolved `host` to `ip`: an allow entry naming that host now matches it. */
void egress_note_resolution(const char *host, const uint8_t ip[4]);

/* Count `len` frame bytes (TX or RX) against the budget; trips the budget when spent. */
void egress_account(uint32_t len);

/* The reason string for a verdict, for log lines and the shell. */
const char *egress_reason(int verdict);

#endif /* EGRESS_H */
