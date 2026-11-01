/**
 * QuantumOS QSAV1 — authenticated encryption for off-site saves
 * (#281 slice 1, ADR-0034).
 *
 * Freestanding, integer-only, header-only; built on user/sha256.h and nothing
 * else, so the host checker (scripts/qsave_check.py) reproduces it byte for
 * byte with Python's hmac/hashlib.
 *
 * Suite 1 = HMAC-SHA256-CTR stream cipher + HMAC-SHA256 encrypt-then-MAC:
 *
 *   enc_key   = HMAC(master, "QSAVE1-enc")      mac_key = HMAC(master, "QSAVE1-mac")
 *   nonce_key = HMAC(master, "QSAVE1-nonce")
 *   keystream block i (32 bytes) = HMAC(enc_key, nonce[16] || i as u64 LE)
 *   ciphertext = plaintext XOR keystream
 *   mac        = HMAC(mac_key, header[0..32) || ciphertext)
 *   nonce      = HMAC(nonce_key, rng[16] || plaintext)[0..16)   (synthetic, SIV-style:
 *                a weak or repeated RNG cannot repeat a nonce for different plaintext)
 *
 * Blob layout (all integers little-endian):
 *    0  magic "QSAV"        4  version u16 (=1)     6  suite u16 (=1)
 *    8  nonce[16]          24  plaintext length u64
 *   32  mac[32]            64  ciphertext[length]
 * The MAC is checked (constant time) BEFORE any byte is decrypted, and the
 * blob must be exactly 64+length bytes: no trailing data is accepted.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef QSAVE_H
#define QSAVE_H

#include <stddef.h>
#include <stdint.h>
#include "sha256.h"

#define QSAVE_VERSION 1
#define QSAVE_SUITE 1
#define QSAVE_HDR_FIELDS 32 /* bytes covered by the MAC, before the tag */
#define QSAVE_HDR_LEN 64
#define QSAVE_KEY_LEN 32
#define QSAVE_NONCE_LEN 16

#define QSAVE_OK 0
#define QSAVE_EBADBLOB (-1) /* too short, bad magic/version/suite, length mismatch */
#define QSAVE_EAUTH (-2)    /* MAC mismatch: wrong key or tampered blob */
#define QSAVE_ESPACE (-3)   /* output buffer too small */

/* Volatile-pointer wipe: the compiler may not elide it as a dead store. */
static inline void qsave_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

static inline void qsave_copy(uint8_t *d, const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static inline void qsave_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void qsave_put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}
static inline uint16_t qsave_get_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint64_t qsave_get_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* HMAC-SHA256 over the concatenation a||b without building it (RFC 2104;
 * identical to hmac_sha256 on the joined message). Keys over 64 bytes are hashed first (RFC 2104). */
static inline void qsave_hmac2(const uint8_t *key, uint32_t key_len, const uint8_t *a,
                               uint32_t a_len, const uint8_t *b, uint32_t b_len,
                               uint8_t out[32]) {
    uint8_t k[64], pad[64], inner[32];
    sha256_ctx c;
    for (int i = 0; i < 64; i++) {
        k[i] = 0;
    }
    if (key_len > 64) {
        sha256(key, key_len, k);
    } else {
        for (uint32_t i = 0; i < key_len; i++) {
            k[i] = key[i];
        }
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x36);
    }
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, a, a_len);
    sha256_update(&c, b, b_len);
    sha256_final(&c, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
    qsave_wipe(k, sizeof(k));
    qsave_wipe(inner, sizeof(inner));
    qsave_wipe(pad, sizeof(pad));
}

typedef struct {
    uint8_t enc[32];
    uint8_t mac[32];
    uint8_t nonce[32];
} qsave_keys;

static inline void qsave_derive(const uint8_t master[QSAVE_KEY_LEN], qsave_keys *k) {
    hmac_sha256(master, QSAVE_KEY_LEN, (const uint8_t *)"QSAVE1-enc", 10, k->enc);
    hmac_sha256(master, QSAVE_KEY_LEN, (const uint8_t *)"QSAVE1-mac", 10, k->mac);
    hmac_sha256(master, QSAVE_KEY_LEN, (const uint8_t *)"QSAVE1-nonce", 12, k->nonce);
}

/* XOR `len` bytes of src into dst with the HMAC-CTR keystream (src==dst ok). */
static inline void qsave_xor_stream(const uint8_t enc_key[32], const uint8_t nonce[16],
                                    const uint8_t *src, uint8_t *dst, uint32_t len) {
    uint8_t msg[24], ks[32];
    qsave_copy(msg, nonce, 16);
    for (uint64_t blk = 0; (uint64_t)blk * 32 < len; blk++) {
        qsave_put_le64(msg + 16, blk);
        hmac_sha256(enc_key, 32, msg, 24, ks);
        uint32_t off = (uint32_t)(blk * 32);
        for (uint32_t i = 0; i < 32 && off + i < len; i++) {
            dst[off + i] = (uint8_t)(src[off + i] ^ ks[i]);
        }
    }
    qsave_wipe(ks, sizeof(ks));
}

/* Seal pt[0..len) into out. rng supplies 16 bytes of (possibly weak) entropy.
 * Returns the blob length (64+len) or QSAVE_ESPACE. */
static inline long qsave_seal(const uint8_t master[QSAVE_KEY_LEN], const uint8_t rng[16],
                              const uint8_t *pt, uint32_t len, uint8_t *out, uint32_t cap) {
    if ((uint64_t)cap < (uint64_t)QSAVE_HDR_LEN + len) {
        return QSAVE_ESPACE;
    }
    qsave_keys k;
    uint8_t full[32];
    qsave_derive(master, &k);
    qsave_hmac2(k.nonce, 32, rng, 16, pt, len, full);

    out[0] = 'Q';
    out[1] = 'S';
    out[2] = 'A';
    out[3] = 'V';
    qsave_put_le16(out + 4, QSAVE_VERSION);
    qsave_put_le16(out + 6, QSAVE_SUITE);
    qsave_copy(out + 8, full, QSAVE_NONCE_LEN);
    qsave_put_le64(out + 24, len);
    qsave_xor_stream(k.enc, out + 8, pt, out + QSAVE_HDR_LEN, len);
    qsave_hmac2(k.mac, 32, out, QSAVE_HDR_FIELDS, out + QSAVE_HDR_LEN, len, out + 32);

    qsave_wipe(&k, sizeof(k));
    qsave_wipe(full, sizeof(full));
    return (long)QSAVE_HDR_LEN + (long)len;
}

/* Verify, then decrypt. Returns plaintext length or a negative QSAVE_E*.
 * Nothing is written to `out` unless the MAC verified. */
static inline long qsave_open(const uint8_t master[QSAVE_KEY_LEN], const uint8_t *blob,
                              uint32_t blob_len, uint8_t *out, uint32_t cap) {
    if (blob_len < QSAVE_HDR_LEN || blob[0] != 'Q' || blob[1] != 'S' || blob[2] != 'A' ||
        blob[3] != 'V' || qsave_get_le16(blob + 4) != QSAVE_VERSION ||
        qsave_get_le16(blob + 6) != QSAVE_SUITE) {
        return QSAVE_EBADBLOB;
    }
    uint64_t len = qsave_get_le64(blob + 24);
    if (len != (uint64_t)blob_len - QSAVE_HDR_LEN) {
        return QSAVE_EBADBLOB;
    }
    if (len > cap) {
        return QSAVE_ESPACE;
    }
    qsave_keys k;
    uint8_t tag[32];
    qsave_derive(master, &k);
    qsave_hmac2(k.mac, 32, blob, QSAVE_HDR_FIELDS, blob + QSAVE_HDR_LEN, (uint32_t)len, tag);
    long rc = hmac_sha256_equal(tag, blob + 32) ? (long)len : QSAVE_EAUTH;
    if (rc >= 0) {
        qsave_xor_stream(k.enc, blob + 8, blob + QSAVE_HDR_LEN, out, (uint32_t)len);
    }
    qsave_wipe(&k, sizeof(k));
    qsave_wipe(tag, sizeof(tag));
    return rc;
}

#endif /* QSAVE_H */
