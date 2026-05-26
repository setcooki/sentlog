# Sentlog evidence verifier

Single-file Python reference verifier for the W3C VC 2.0 envelopes the
SDK emits. Reads a signed envelope, re-canonicalises per RFC 8785
JCS, and checks the `proof.proofValue` Ed25519 signature against an
integrator-supplied public key.

This is the canonical tool integrators run to confirm that envelopes
they receive are byte-identical to what the SDK signed — useful for
incident response, downstream re-verification, and CI gates.

## Install

```bash
cd tools/evidence-verifier
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Single dependency: [`cryptography`](https://cryptography.io) for the
Ed25519 verify primitive.

## Run

```bash
python3 verify.py envelope.json <pubkey-hex>
```

`<pubkey-hex>` is the 32-byte Ed25519 public key encoded as 64 hex
chars. The bytes match what the SDK was handed at
`sentlog_set_signing_key` time (the last 32 bytes of the 64-byte
seed||pubkey concatenation).

Stdin form:

```bash
cat envelope.json | python3 verify.py - <pubkey-hex>
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | signature verifies |
| 1 | signature does NOT verify (tampered or wrong key) |
| 2 | envelope is malformed / missing proof / unsupported cryptosuite |
| 3 | usage error (bad args, missing files) |

## What it checks

- `proof.cryptosuite` is `eddsa-jcs-2022` (the Phase 4b ship). The
  Phase 7 hardware path will emit `ecdsa-jcs-2019`; that needs a
  second branch here when it lands.
- `proof.proofValue` decodes as base64url-no-pad.
- Re-canonicalises the envelope-without-proofValue per RFC 8785 JCS
  using the same logic as `shared/src/internal/signing.cpp`'s
  `jcs_canonicalize` so the byte stream matches what the SDK signed.
- Calls `cryptography`'s Ed25519 verify primitive.

## What it does NOT yet check

- TSA timestamp (Phase 4h adds the block; the verifier will need
  RFC 3161 TimeStampToken parsing then).
- `did:key:` resolution for `proof.verificationMethod`. Today the
  integrator passes the pubkey bytes directly; in a future chunk
  the verifier will resolve well-known DID methods.
- Hardware-backed `ecdsa-jcs-2019` envelopes (Phase 7).
- Envelope schema validity beyond the proof — that's
  `tools/reference-consumer/` territory.

## Algorithm reference

The signing input per W3C Data Integrity §4.4 with
`eddsa-jcs-2022`:

1. Take the envelope WITH the proof block but WITHOUT
   `proof.proofValue`.
2. Canonicalise with RFC 8785 JCS:
   - object keys sorted lexicographically
   - no insignificant whitespace
   - canonical number/string formatting
3. The signing input bytes are the UTF-8 encoding of that
   canonical JSON.
4. Verify the signature over those bytes with Ed25519 and the
   integrator's public key.

The SDK side of this algorithm lives in
`shared/src/internal/signing.cpp:sign_envelope_eddsa_jcs`. The two
implementations must produce identical canonical bytes for the
same logical envelope.
