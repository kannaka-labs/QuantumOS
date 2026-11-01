/**
 * QuantumOS /bin/qsave - seal and open encrypted off-site saves (#281 slice 1, ADR-0034).
 *
 *   run /bin/qsave seal <path> <key64hex>   read <path> (initrd or overlay), seal it into a
 *                                           QSAV1 blob, print the blob as hex lines, then
 *                                           prove in-process that it opens and that tampered
 *                                           copies and a wrong key are refused
 *   run /bin/qsave open <path> <key64hex>   read a blob file (raw, or hex text), verify the
 *                                           MAC, decrypt, print length + sha256 of the plaintext
 *
 * Output lines (the host checker and `make ci-smoke-qsave` grep these):
 *   QSAVE: blob <hex>            the blob, 64 bytes (128 hex chars) per line, in order
 *   QSAVE: blob-end len=<n>
 *   QSAVE: pt-sha256 <hex>
 *   QSAVE: roundtrip ok | tamper rejected | open ok len=<n> | open FAILED (...)
 * Needs no capability beyond reading files and SYS_QRAND; it never writes a file
 * (the key must not land in the overlay or on the QDSK disk), and has no network yet.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"
#include "qsave.h"

#define QS_MAX_PT 3072
#define QS_MAX_BLOB (QS_MAX_PT + QSAVE_HDR_LEN)

static uint8_t g_in[2 * QS_MAX_BLOB + 64]; /* file bytes (raw or hex text) */
static uint8_t g_blob[QS_MAX_BLOB];
static uint8_t g_copy[QS_MAX_BLOB];
static uint8_t g_pt[QS_MAX_PT];

static __attribute__((noreturn)) void die(const char *msg, long code) {
    write_str(msg);
    exit_(code);
    for (;;) {
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_key(const char *s, uint8_t key[QSAVE_KEY_LEN]) {
    for (int i = 0; i < QSAVE_KEY_LEN; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hi < 0 ? -1 : hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        key[i] = (uint8_t)(hi * 16 + lo);
    }
    return s[2 * QSAVE_KEY_LEN] == 0 ? 0 : -1; /* exactly 64 hex chars */
}

/* Read a whole file into buf; returns length, or -1 (missing) / -2 (too big). */
static long slurp(const char *path, uint8_t *buf, long cap) {
    long fd = open_(path);
    if (fd < 0) {
        return -1;
    }
    long n = 0;
    for (;;) {
        long r = read_(fd, buf + n, cap - n);
        if (r < 0) {
            n = -1;
            break;
        }
        if (r == 0) {
            break;
        }
        n += r;
        if (n >= cap) {
            long extra = read_(fd, g_copy, 1); /* anything left over? */
            n = extra > 0 ? -2 : n;
            break;
        }
    }
    close_(fd);
    return n;
}

static void put_hex_line(const char *prefix, const uint8_t *p, int n) {
    static const char hx[] = "0123456789abcdef";
    char line[160];
    int o = 0;
    while (*prefix) {
        line[o++] = *prefix++;
    }
    for (int i = 0; i < n; i++) {
        line[o++] = hx[p[i] >> 4];
        line[o++] = hx[p[i] & 15];
    }
    line[o] = 0;
    write_str(line);
}

static void print_sha(const uint8_t *p, uint32_t n) {
    uint8_t d[32];
    sha256(p, n, d);
    put_hex_line("QSAVE: pt-sha256 ", d, 32);
}

static void say_int(const char *fmt, long v) {
    char b[96];
    snprintf(b, sizeof(b), fmt, (int)v);
    write_str(b);
}

static int cmd_seal(const char *path, const uint8_t key[QSAVE_KEY_LEN]) {
    long n = slurp(path, g_in, QS_MAX_PT);
    if (n == -1) {
        die("QSAVE: seal FAILED (cannot read input)", 2);
    }
    if (n == -2) {
        die("QSAVE: seal FAILED (input over the 3072-byte slice-1 cap)", 2);
    }
    uint8_t rng[16];
    if (qrand_fill(rng, sizeof(rng)) < 0) {
        die("QSAVE: seal FAILED (no entropy syscall)", 2);
    }
    long bl = qsave_seal(key, rng, g_in, (uint32_t)n, g_blob, sizeof(g_blob));
    if (bl < 0) {
        die("QSAVE: seal FAILED (buffer)", 2);
    }
    say_int("QSAVE: sealed %d plaintext bytes", n);
    for (long off = 0; off < bl; off += 64) {
        put_hex_line("QSAVE: blob ", g_blob + off, (int)(bl - off < 64 ? bl - off : 64));
    }
    say_int("QSAVE: blob-end len=%d", bl);
    print_sha(g_in, (uint32_t)n);

    /* In-process proof 1: the blob opens to exactly the input. */
    long pl = qsave_open(key, g_blob, (uint32_t)bl, g_pt, sizeof(g_pt));
    if (pl != n || memcmp(g_pt, g_in, (size_t)n) != 0) {
        die("QSAVE: roundtrip FAILED", 3);
    }
    write_str("QSAVE: roundtrip ok");

    /* In-process proof 2 (anti-vacuous): one flipped ciphertext bit, one flipped
     * header (nonce) bit, and a wrong key must each be refused with EAUTH. */
    int refused = 0;
    memcpy(g_copy, g_blob, (size_t)bl);
    if (n > 0) {
        g_copy[QSAVE_HDR_LEN + n / 2] ^= 0x01;
        refused += qsave_open(key, g_copy, (uint32_t)bl, g_pt, sizeof(g_pt)) == QSAVE_EAUTH;
        g_copy[QSAVE_HDR_LEN + n / 2] ^= 0x01;
    } else {
        refused++;
    }
    g_copy[9] ^= 0x80;
    refused += qsave_open(key, g_copy, (uint32_t)bl, g_pt, sizeof(g_pt)) == QSAVE_EAUTH;
    g_copy[9] ^= 0x80;
    uint8_t wrong[QSAVE_KEY_LEN];
    memcpy(wrong, key, sizeof(wrong));
    wrong[0] ^= 0x01;
    refused += qsave_open(wrong, g_copy, (uint32_t)bl, g_pt, sizeof(g_pt)) == QSAVE_EAUTH;
    qsave_wipe(wrong, sizeof(wrong));
    if (refused != 3) {
        die("QSAVE: tamper NOT rejected", 4);
    }
    write_str("QSAVE: tamper rejected");
    qsave_wipe(g_pt, sizeof(g_pt));
    return 0;
}

static int cmd_open(const char *path, const uint8_t key[QSAVE_KEY_LEN]) {
    long n = slurp(path, g_in, (long)sizeof(g_in));
    if (n < 0) {
        die("QSAVE: open FAILED (cannot read input)", 2);
    }
    const uint8_t *blob = g_in;
    long bl = n;
    if (n < 4 || g_in[0] != 'Q' || g_in[1] != 'S' || g_in[2] != 'A' || g_in[3] != 'V') {
        /* Not a raw blob: treat as hex text (whitespace ignored, hex digits only). */
        long o = 0;
        int hi = -1;
        for (long i = 0; i < n; i++) {
            int v = hexval((char)g_in[i]);
            if (v < 0) {
                if (g_in[i] == ' ' || g_in[i] == '\n' || g_in[i] == '\r' || g_in[i] == '\t') {
                    continue;
                }
                die("QSAVE: open FAILED (not a blob: bad hex)", 2);
            }
            if (hi < 0) {
                hi = v;
            } else {
                if (o >= (long)sizeof(g_blob)) {
                    die("QSAVE: open FAILED (blob too large)", 2);
                }
                g_blob[o++] = (uint8_t)(hi * 16 + v);
                hi = -1;
            }
        }
        if (hi >= 0) {
            die("QSAVE: open FAILED (odd hex length)", 2);
        }
        blob = g_blob;
        bl = o;
    } else if (n > (long)sizeof(g_blob)) {
        die("QSAVE: open FAILED (blob too large)", 2);
    }
    long pl = qsave_open(key, blob, (uint32_t)bl, g_pt, sizeof(g_pt));
    if (pl == QSAVE_EAUTH) {
        die("QSAVE: open FAILED (authentication: wrong key or tampered blob)", 5);
    }
    if (pl < 0) {
        die("QSAVE: open FAILED (malformed blob)", 6);
    }
    say_int("QSAVE: open ok len=%d", pl);
    print_sha(g_pt, (uint32_t)pl);
    qsave_wipe(g_pt, sizeof(g_pt));
    return 0;
}

void _start(void) {
    char *argv[USER_ARGS_MAX];
    int argc = get_args(argv, USER_ARGS_MAX);
    if (argc != 4) {
        die("QSAVE: usage: qsave seal|open <path> <64-hex-char key>", 1);
    }
    uint8_t key[QSAVE_KEY_LEN];
    if (parse_key(argv[3], key) != 0) {
        die("QSAVE: key must be exactly 64 hex characters", 1);
    }
    int rc;
    if (memcmp(argv[1], "seal", 5) == 0) {
        rc = cmd_seal(argv[2], key);
    } else if (memcmp(argv[1], "open", 5) == 0) {
        rc = cmd_open(argv[2], key);
    } else {
        die("QSAVE: usage: qsave seal|open <path> <64-hex-char key>", 1);
    }
    qsave_wipe(key, sizeof(key));
    exit_(rc);
    for (;;) {
    }
}

