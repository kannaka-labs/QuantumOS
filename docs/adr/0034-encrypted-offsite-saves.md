# 34. Encrypted off-site saves: threat model, key handling without secure storage, the QSAV1 format, and the first slice

Date: 2026-11-01

Status: Proposed (slice 1 implemented; its CI gate has not yet run under QEMU, see Verification)

## Context

Issue #281 is the storage thread named in ADR-0026 (the off-site backup, OCI Object Storage,
PR #282) and ADR-0033 (the "storage work" that November kernel capacity goes to). This ADR covers
only its first slice, **encrypted off-site saves**. When this was written the issue text itself
was not readable from the working environment (`gh` needed approval), so the scope below is taken
from the task statement and from what ADR-0026, ADR-0033 and the tree say about #281. Check it
against the issue before accepting.

What a "save" is today (read from the tree, not assumed):

- `sync` / `SYS_SYNC` (qsh `sync`, user/usys.h `sync_()`) flushes the RAM overlay as a ustar
  archive to the QDSK volume (ADR-0007), together with the field and audit sections. QDSK has
  per-section checksums (rotate-xor additive, seed 0x517E7A11). That is an **error detector, not
  an authenticator**: anyone who can write the disk can produce a valid checksum. ADR-0007
  already states the disk is "attacker-writable while the machine is off".
- Only qsh holds the filesystem-write grant (`citizens.c` `grant_fswrite`, `syscall.c`
  `has_fs_write_cap`). A program started with `run /bin/x` can read files and print, and cannot
  write one.
- `SYS_QRAND` can be a timer-seeded generator, not a CSPRNG. The boot labels say so
  ("Entropy: unseeded at boot (timer default, not from a QPU)"; ADR-0025).
- There is no secure storage in the guest: no TPM, no keyring, no sealed memory, one flat ring-3
  address space per process, and the kernel does not zero a process's pages or its args page on exit.

## Threat model

Assets: the confidentiality and integrity of a save (a file or the synced archive) once it
leaves the guest.

Adversaries, in scope for slice 1:

1. **Off-site storage operator or network observer** (the future IPFS or OCI hop) who can read,
   copy, replay, truncate, extend or modify a stored blob. Needs: confidentiality, and
   tamper/truncation/extension detection before any plaintext is produced.
2. **Whoever holds the disk or the blob while the machine is off**: same needs.
3. **A forger without the key**: must not be able to make a blob that verifies.

Out of scope for slice 1, stated so nobody assumes otherwise:

- A **compromised guest** while it holds the key. Ring-3 code that can read the args page or
  process memory can read the key. There is no defence against that without secure storage.
- **Rollback / replay of an older valid blob.** A genuine old blob still verifies. Freshness needs
  a monotonic counter or a signed manifest, which is a later slice.
- **Traffic analysis**: the blob length equals plaintext length + 64, so size leaks.
- **Console exposure**: slice 1 prints the key on the command line the operator types, which
  the serial log shows (see Key handling).
- Deniability, forward secrecy, key rotation.

## Decision

### Key handling inside a guest with no secure storage

The honest position: the guest cannot protect a key at rest, so **the key is never at rest in
the guest**. Slice 1 therefore:

- takes a 32-byte master key as 64 hex characters in argv (`run /bin/qsave seal <path> <key>`)
  and holds it only in the tool's stack, wiping every derived key and the master copy with a
  volatile write before exit (`qsave_wipe`). The kernel-owned read-only args page cannot be
  wiped by the process and is not cleared on exit. That is a known residual, not a fix.
- never writes the key or any plaintext derivative to the RAM overlay or to QDSK. `/bin/qsave`
  holds no write grant, so it **cannot** create a file. A blob leaves only through the console
  (slice 1) and later through a transport (slices 2+).
- derives three independent subkeys with HMAC (below) so one master key never keys two
  primitives directly.
- tolerates a weak RNG. The nonce is synthetic (SIV-style), so a repeated or predictable
  `SYS_QRAND` output cannot repeat a nonce for two different plaintexts.

Where the key comes from in later slices (not built here): a host-admitted session key delivered
over the authenticated COM2 plane (ADR-0019 pattern: host holds the secret, the guest gets a
per-boot value), or derived on the host and passed on the kernel command line like `qseed=`.
Both put the long-term secret on the host, not in the guest. Slice 1's typed-in key is a
**test-grade** channel: it is in the serial log. The smoke key is a public constant for that
reason; it protects nothing.

### Cipher and integrity: only primitives already in the tree

`user/sha256.h` already provides SHA-256 and `hmac_sha256` (RFC 2104) and a constant-time
`hmac_sha256_equal`. Nothing new is imported. Suite 1 is **HMAC-SHA256 in counter mode as the
stream cipher, with HMAC-SHA256 encrypt-then-MAC**. HMAC-SHA256 is a standard PRF, so counter-mode
keystream from it is a sound stream cipher. It is slow (two SHA-256 passes plus key padding per
32 bytes) but needs no table, no SIMD and no new code, which matches ADR-0004 and the
`-mno-sse` user flags.

```
enc_key   = HMAC(master, "QSAVE1-enc")
mac_key   = HMAC(master, "QSAVE1-mac")
nonce_key = HMAC(master, "QSAVE1-nonce")
keystream block i (32 B) = HMAC(enc_key, nonce[16] || i as u64 LE)
ciphertext = plaintext XOR keystream
mac        = HMAC(mac_key, header[0..32) || ciphertext)
nonce      = HMAC(nonce_key, rng[16] || plaintext)[0..16)
```

Rejected: a bare `SHA256(key || nonce || ctr)` keystream and a `SHA256(key || msg)` MAC
(length-extension-prone, no key separation); porting ChaCha20-Poly1305 or AES-GCM (new
audited-code surface for no slice-1 gain; revisit if throughput matters).

Limits to know: per key, the synthetic nonce is 128 bits, so collisions are negligible at any
realistic number of saves; identical plaintext under one key and identical `rng` gives an
identical blob (an equality leak, accepted).

### Save format QSAV1 (little-endian, self-describing)

```
off  size  field
  0     4  magic "QSAV"
  4     2  version = 1
  6     2  suite   = 1  (HMAC-SHA256-CTR + HMAC-SHA256 EtM)
  8    16  nonce
 24     8  plaintext length
 32    32  mac
 64     n  ciphertext
```

The integrity proof is the MAC over `header[0..32) || ciphertext`, so version, suite, nonce
and length are all authenticated. Verification order is fixed: structural checks (length ≥ 64,
magic, version, suite, `length == blob size - 64` so truncation and trailing bytes fail), then
the MAC in constant time, and only then decryption. An unknown version or suite is refused, never
guessed at. The same rules hold in `user/qsave.h` and in `scripts/qsave_check.py`, which exists
as an **independent implementation** (hashlib/hmac only), so a wrong C primitive cannot
agree with it by accident.

### Minimal first slice (this change)

| piece | file |
|---|---|
| freestanding seal/open + wipe, header-only | `user/qsave.h` |
| tool `/bin/qsave seal\|open <path> <key>` (initrd, not a service) | `user/qsave.c` |
| host parser, MAC verifier, decryptor, fixture sealer | `scripts/qsave_check.py` |
| host unit tests: all lengths, every single-bit flip, wrong key, truncation, extension, log parsing | `scripts/test_qsave_check.py` |
| gate | `make ci-smoke-qsave` (also `make qsave-check-test`, host only), CI job `encrypted-save` |

`seal` reads a file of up to 3072 bytes (a hard slice-1 cap; larger saves need streaming),
prints the blob as `QSAVE: blob <128 hex>` lines plus `QSAVE: blob-end len=N`, then proves in
process: the blob opens to the input, and a flipped ciphertext bit, a flipped nonce bit and a
wrong key are each refused. `open` takes a raw or hex-text blob and prints the plaintext length and
SHA-256, not the plaintext. The gate then has the **host** extract the blob from the QEMU
log, verify the MAC with the known key, decrypt, and compare to `rootfs/docs/hello.txt`; the
same blob must fail under a wrong key (anti-vacuous, ADR-0016).

### Not in this slice (ordered)

1. Streaming / large saves and sealing the real QDSK archive (needs a write path for a non-qsh
   citizen or a kernel-side hook; the write grant is deliberately qsh-only).
2. Key delivery from the host (COM2 session key or command line), removing the typed key.
3. Transport: egress-granted upload (ADR-0030) to OCI/IPFS, content addressing of the blob.
4. Freshness: a signed manifest or counter against rollback.

## Consequences

- Off-site holders see only ciphertext, and any change to a blob is detected before plaintext exists.
- The key problem is moved, not solved: the guest still sees the key while it runs.
- The format is versioned and suite-tagged, so a faster AEAD can be added as suite 2 without
  breaking readers.
- Cost: ~64 bytes overhead, and speed bounded by the user-space HMAC (fine for KB-sized saves).

## Verification (what was and was not run)

Run: `python3 scripts/test_qsave_check.py` (9 tests, pass).

Not run: the C tool was **not compiled**, `make` was **not run** and `make ci-smoke-qsave` was
**not run**. The authoring machine has no gcc, make, nasm or qemu on PATH (only Python and
clang). C/Python agreement is therefore unproven until CI runs the gate; that is the first thing
to check. An earlier, abandoned `qenc` draft (bare `SHA256(key||...)` keystream, the construction
rejected above) was removed rather than kept as a second path.

## References

ADR-0004 (integer-only), ADR-0007 (QDSK), ADR-0016 (anti-vacuous gates), ADR-0017 (honest
slices), ADR-0019 (authenticated plane), ADR-0025 (entropy labels), ADR-0026, ADR-0030
(egress grant), ADR-0033; issue #281.

