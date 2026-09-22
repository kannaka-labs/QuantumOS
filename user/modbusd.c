/**
 * QuantumOS modbusd — a Modbus/TCP server, in ring 3 (0xSCADA stage 4).
 *
 * The point of this program is not that QuantumOS can speak an industrial
 * protocol. It is WHERE the refusal lives.
 *
 * Every SCADA stack in the field authenticates, authorises, executes and
 * then writes the record of what it did, all inside one process. A log a
 * process writes about itself is the same class of evidence as a status
 * field a process sets about itself. This agent is the smallest honest
 * demonstration of the alternative: a control write arrives over the wire
 * and is REFUSED, by default, with the refusal counted and readable.
 *
 * Read the caveat in "AUTHORITY" below before believing too much of that.
 *
 * Protocol: Modbus/TCP (MBAP header + PDU), big-endian on the wire.
 *   FC 0x03  read holding registers   -> live kernel telemetry
 *   FC 0x04  read input registers     -> same bank, read-only alias
 *   FC 0x06  write single register    -> REFUSED unless armed
 * Anything else earns exception 0x01. This is deliberately a small
 * dialect: a protocol surface is an attack surface, and every function
 * code not implemented is one that cannot be got wrong.
 *
 * Capabilities: grant_net only, the same coarse cap httpd holds. As there,
 * that cap is honestly WIDER than the job needs — it also gates SYS_UDP,
 * SYS_RESOLVE and outbound TCP_CONNECT — so this program contains no
 * outbound operation of any kind. It listens, answers, and closes.
 *
 * AUTHORITY, stated plainly because the whole value of this program is
 * that the claim is true: the refusal below is enforced by THIS PROCESS,
 * not by the kernel. QuantumOS capabilities are per-resource-class
 * (CAP_RESOURCE_DEVICE over DEVICE_ID_NET); there is no capability
 * meaning "may write holding register 40001". So modbusd today is exactly
 * the application-level enforcement that 0xSCADA ADR-0028 criticises, and
 * it is written this way on purpose: it is the working example that makes
 * the missing kernel primitive concrete and testable. Closing that gap —
 * a per-point control capability the kernel itself denies — is the next
 * ADR, not this file. Until then this program does not claim to be safe
 * for real plant; it claims to be honest about why it is not.
 *
 * DoS bounds, following httpd: a TOTAL request deadline captured once and
 * never reset on progress (slow-loris), a hard frame cap, and a bounded
 * close. No NIC -> the listen fails hard and modbusd idles silently, so
 * the default NIC-less boot is unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h" /* usys.h + the ghost_put string builders */

#define MB_PORT 502
#define MB_FRAME_CAP 260 /* MBAP(7) + max PDU(253) */
#define MB_REQ_DEADLINE 200
#define MB_SEND_DEADLINE 500
#define MB_CLOSE_DEADLINE 800

/* MBAP */
#define MBAP_LEN 7
#define MB_PROTO_ID 0

/* Function codes */
#define FC_READ_HOLDING 0x03
#define FC_READ_INPUT 0x04
#define FC_WRITE_SINGLE 0x06

/* Exceptions */
#define EX_ILLEGAL_FUNCTION 0x01
#define EX_ILLEGAL_ADDRESS 0x02
#define EX_ILLEGAL_VALUE 0x03
#define EX_DEVICE_FAILURE 0x04

/* The register bank. Small and fixed: a register map that can grow at
 * runtime is a register map nobody can audit. */
#define MB_NREG 8
#define REG_UPTIME_S 0  /* seconds since boot (wraps at 65535) */
#define REG_REQUESTS 1  /* requests served, rises — proves liveness */
#define REG_REFUSALS 2  /* control writes REFUSED. The important one. */
#define REG_SETPOINT 3  /* the only writable point, and it is armed off */
#define REG_PROTO_ERR 4 /* malformed frames rejected */
#define REG_ARMED 5     /* 1 if control writes are permitted at all */
#define REG_VERSION 6   /* agent version, so a poller can tell drift */
#define REG_RESERVED 7

#define MB_VERSION 1

/* Control authority. Off. Changing this constant is the entire "arming"
 * mechanism today, which is precisely the weakness documented above: it
 * is a decision compiled into the program being governed. A kernel
 * capability would be a decision held OVER it. */
#define MB_CONTROL_ARMED 0

static unsigned served;
static unsigned refusals;
static unsigned proto_err;
static unsigned short setpoint;

static void req_clear(tcp_req_t *req) {
    for (unsigned i = 0; i < sizeof(*req); i++) {
        ((unsigned char *)req)[i] = 0;
    }
}

/* Poll TCP_CLOSE until the connection retires (bounded, heartbeating). */
static void mb_close(void) {
    long t0 = ticks();
    for (;;) {
        heartbeat();
        tcp_req_t req;
        req_clear(&req);
        long r = tcp_(TCP_CLOSE, &req);
        if (r != UDP_WOULDBLOCK) {
            break;
        }
        if (ticks() - t0 > MB_CLOSE_DEADLINE) {
            break;
        }
        yield();
    }
}

static unsigned short rd16(const unsigned char *p) {
    return (unsigned short)((p[0] << 8) | p[1]);
}

static void wr16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFF);
}

/* Current value of one register. Computed at READ TIME, never cached:
 * a poller that sees REG_UPTIME_S advance knows the OS answered live,
 * which is the same property httpd's rising counter gives the CI gate. */
static unsigned short reg_read(unsigned short addr) {
    switch (addr) {
    case REG_UPTIME_S:
        return (unsigned short)((ticks() / 100) & 0xFFFF);
    case REG_REQUESTS:
        return (unsigned short)(served & 0xFFFF);
    case REG_REFUSALS:
        return (unsigned short)(refusals & 0xFFFF);
    case REG_SETPOINT:
        return setpoint;
    case REG_PROTO_ERR:
        return (unsigned short)(proto_err & 0xFFFF);
    case REG_ARMED:
        return MB_CONTROL_ARMED;
    case REG_VERSION:
        return MB_VERSION;
    default:
        return 0;
    }
}

/* Build an exception response into out (PDU only). Returns PDU length. */
static int pdu_exception(unsigned char *out, unsigned char fc, unsigned char code) {
    out[0] = (unsigned char)(fc | 0x80);
    out[1] = code;
    return 2;
}

/* Handle one PDU. Returns the response PDU length, or 0 to send nothing. */
static int handle_pdu(const unsigned char *in, int inlen, unsigned char *out, int outcap) {
    if (inlen < 1) {
        proto_err++;
        return 0;
    }
    unsigned char fc = in[0];

    if (fc == FC_READ_HOLDING || fc == FC_READ_INPUT) {
        if (inlen < 5) {
            proto_err++;
            return pdu_exception(out, fc, EX_ILLEGAL_VALUE);
        }
        unsigned short addr = rd16(in + 1);
        unsigned short qty = rd16(in + 3);
        /* Modbus caps a read at 125 registers; ours is smaller still. */
        if (qty == 0 || qty > MB_NREG) {
            return pdu_exception(out, fc, EX_ILLEGAL_VALUE);
        }
        if (addr >= MB_NREG || (unsigned)(addr + qty) > MB_NREG) {
            return pdu_exception(out, fc, EX_ILLEGAL_ADDRESS);
        }
        int need = 2 + qty * 2;
        if (need > outcap) {
            return pdu_exception(out, fc, EX_DEVICE_FAILURE);
        }
        out[0] = fc;
        out[1] = (unsigned char)(qty * 2);
        for (unsigned short i = 0; i < qty; i++) {
            wr16(out + 2 + i * 2, reg_read((unsigned short)(addr + i)));
        }
        return need;
    }

    if (fc == FC_WRITE_SINGLE) {
        if (inlen < 5) {
            proto_err++;
            return pdu_exception(out, fc, EX_ILLEGAL_VALUE);
        }
        unsigned short addr = rd16(in + 1);
        unsigned short val = rd16(in + 3);

        /* THE REFUSAL. Counted before anything else, so a refused write
         * is visible to the next reader even though it changed nothing.
         * An unrecorded refusal is indistinguishable from no request. */
        if (!MB_CONTROL_ARMED) {
            refusals++;
            write_str("modbusd: control write REFUSED (not armed)");
            return pdu_exception(out, fc, EX_DEVICE_FAILURE);
        }
        if (addr != REG_SETPOINT) {
            refusals++;
            return pdu_exception(out, fc, EX_ILLEGAL_ADDRESS);
        }
        setpoint = val;
        /* Echo the request, as the spec requires. */
        out[0] = fc;
        wr16(out + 1, addr);
        wr16(out + 3, val);
        return 5;
    }

    return pdu_exception(out, fc, EX_ILLEGAL_FUNCTION);
}

void _start(void) {
    for (;;) {
        heartbeat();

        long r;
        for (;;) {
            heartbeat();
            tcp_req_t req;
            req_clear(&req);
            req.port = MB_PORT;
            r = tcp_(TCP_LISTEN, &req);
            if (r != UDP_WOULDBLOCK) {
                break;
            }
            yield();
        }
        if (r != 0) {
            /* Name the actual cause. The three are not the same problem
             * and cost very different amounts to diagnose:
             *   -1 EINVAL  another process already holds THE listener.
             *              QuantumOS arms exactly one passive TCP
             *              listener, and httpd takes :8080 at boot, so
             *              this is the EXPECTED result today. modbusd
             *              idles rather than fight for it.
             *   -4 EPERM   no network capability (grant_net missing).
             *   -5 EIO     no NIC at all (the default boot).
             * A single "no network" for all three would send a reader
             * hunting a cable fault when the answer is a busy socket. */
            if (r == -1) {
                write_str("modbusd: TCP listener already held (httpd) - idle");
            } else if (r == -4) {
                write_str("modbusd: no network capability - idle");
            } else {
                write_str("modbusd: no network - idle");
            }
            for (;;) {
                heartbeat();
                yield();
            }
        }

        for (;;) {
            heartbeat();
            tcp_req_t req;
            req_clear(&req);
            r = tcp_(TCP_ACCEPT, &req);
            if (r != UDP_WOULDBLOCK) {
                break;
            }
            yield();
        }
        if (r != 0) {
            mb_close();
            continue;
        }

        /* One request per connection. A TOTAL deadline captured once and
         * never reset on progress — a byte-dribbling peer is cut off. */
        static unsigned char frame[MB_FRAME_CAP];
        int got = 0;
        int have = 0;
        long t0 = ticks();
        while (!have) {
            heartbeat();
            if (ticks() - t0 > MB_REQ_DEADLINE) {
                break;
            }
            if (got >= MB_FRAME_CAP) {
                proto_err++;
                break;
            }
            tcp_req_t req;
            req_clear(&req);
            req.buf = (char *)frame + got;
            req.len = (unsigned short)(MB_FRAME_CAP - got);
            long n = tcp_(TCP_RECV, &req);
            if (n > 0) {
                got += (int)n;
                if (got >= MBAP_LEN) {
                    /* MBAP length counts the unit id plus the PDU. */
                    unsigned short blen = rd16(frame + 4);
                    if (blen == 0 || blen > MB_FRAME_CAP - 6) {
                        proto_err++;
                        break;
                    }
                    if (got >= 6 + blen) {
                        have = 1;
                    }
                }
            } else if (n == 0) {
                break; /* peer half-closed before a whole frame */
            } else if (n == UDP_WOULDBLOCK) {
                yield();
            } else {
                break;
            }
        }
        if (!have) {
            mb_close();
            continue;
        }

        /* Protocol id must be 0; anything else is not Modbus/TCP. */
        if (rd16(frame + 2) != MB_PROTO_ID) {
            proto_err++;
            mb_close();
            continue;
        }

        served++;
        unsigned short txn = rd16(frame + 0);
        unsigned char unit = frame[6];
        unsigned short blen = rd16(frame + 4);
        int pdulen = (int)blen - 1;
        if (pdulen < 0) {
            proto_err++;
            mb_close();
            continue;
        }

        static unsigned char resp[MB_FRAME_CAP];
        int rlen = handle_pdu(frame + MBAP_LEN, pdulen, resp + MBAP_LEN, MB_FRAME_CAP - MBAP_LEN);
        if (rlen > 0) {
            wr16(resp + 0, txn);
            wr16(resp + 2, MB_PROTO_ID);
            wr16(resp + 4, (unsigned short)(rlen + 1)); /* unit + PDU */
            resp[6] = unit;

            t0 = ticks();
            for (;;) {
                heartbeat();
                tcp_req_t req;
                req_clear(&req);
                req.buf = (char *)resp;
                req.len = (unsigned short)(MBAP_LEN + rlen);
                long sr = tcp_(TCP_SEND, &req);
                if (sr != UDP_WOULDBLOCK) {
                    break;
                }
                if (ticks() - t0 > MB_SEND_DEADLINE) {
                    break;
                }
                yield();
            }
        }

        mb_close();
    }
}
