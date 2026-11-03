# Vendored tweetnacl

| Field | Value |
|---|---|
| Upstream | https://tweetnacl.cr.yp.to/ |
| Release | 20140427 (the canonical tweetnacl release) |
| Files | `tweetnacl.c`, `tweetnacl.h` (byte-for-byte from upstream) |
| Source licence | Public domain (Daniel J. Bernstein and contributors) |
| Vendored on | 2026-05-25 |

## Why vendored

Phase 4b needs Ed25519 signing for the W3C VC 2.0
`DataIntegrityProof` block. tweetnacl is the canonical
small-footprint, single-file, public-domain reference
implementation of NaCl — used in WireGuard derivatives, libnacl
ports, and countless embedded products. ~800 lines of C, exposes a
clean `crypto_sign` API. No external dependency tree.

Compared to the alternatives:
- **libsodium** is hardened-fork-of-NaCl and a fine library, but
  drags in an autotools build that doesn't play cleanly with
  SwiftPM. ~10× the binary footprint for primitives we don't use.
- **BoringSSL / OpenSSL** are huge for the single primitive we need.
- **monocypher** is an option, but tweetnacl is older and
  better-vetted in academic / embedded contexts.

## What we use

`crypto_sign(sm, smlen, m, mlen, sk)` — Ed25519 sign. Deterministic
per RFC 8032; no RNG needed. Used by
[`shared/src/internal/signing.{h,cpp}`](../../src/internal/signing.h).

`crypto_sign_open(m, mlen, sm, smlen, pk)` — Ed25519 verify. Used by
[`shared/src/internal/ota.cpp`](../../src/internal/ota.cpp) to verify
signed manifests.

We do NOT generate keypairs in the SDK — the integrator supplies a
signing key via the C ABI's `sentlog_set_signing_key`. That means
the tweetnacl `extern void randombytes(...)` symbol is satisfied by
a stub that aborts; `crypto_sign_keypair` is never called from our
code path. The stub lives in `randombytes_stub.c`.

## Bumping the pin

tweetnacl hasn't been updated since 2014 — Bernstein considers it
done. If a security-relevant issue ever surfaces upstream, replace
both files byte-for-byte from `https://tweetnacl.cr.yp.to/<date>/`.
