# Sentlog evidence verifier

Single-file Python reference verifier for the W3C VC 2.0 envelopes the
SDK emits. Reads a signed envelope and checks its `eddsa-jcs-2022` or
`ecdsa-jcs-2019` Data Integrity proof against an integrator-supplied
public key.

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
Ed25519 and ECDSA verify primitives.

## Run

```bash
python3 verify.py envelope.json <pubkey-hex>
```

`<pubkey-hex>` is either:

- a 32-byte Ed25519 public key as 64 hex chars. The bytes match what
  the SDK was handed at `sentlog_set_signing_key` time (the last 32
  bytes of the 64-byte seed||pubkey concatenation).
- a P-256 public key as a SEC1 point (33 bytes compressed or 65 bytes
  uncompressed), for hardware-signed envelopes.

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

- The envelope is strict JSON: no duplicate keys, no `NaN`.
- `proof` has `type: DataIntegrityProof`,
  `proofPurpose: assertionMethod`, a supported `cryptosuite`, and no
  unknown fields. A `proof.@context` must be a prefix of the envelope's.
- `proof.proofValue` is multibase base58btc and decodes to 64 bytes.
- The signature verifies over the signing input below.

## What it does NOT check

- `evidence_grade`. A valid signature only shows that the key holder
  made the claim, so confirm separately that the key is hardware-backed
  before trusting `EnclaveSigned`.
- `tsa_timestamp`. It's a sidecar added after signing, so the proof
  doesn't cover it, and the RFC 3161 token isn't parsed.
- The `device_attestation` token.
- `did:key:` resolution for `proof.verificationMethod`. Today the
  integrator passes the pubkey bytes directly.
- Envelope schema validity beyond the proof — that's
  `tools/reference-consumer/` territory.

## Algorithm reference

Both suites follow the W3C
[vc-di-eddsa](https://www.w3.org/TR/vc-di-eddsa/) and
[vc-di-ecdsa](https://www.w3.org/TR/vc-di-ecdsa/) `jcs` algorithms, and
the self-test runs their published test vectors:

```
signing_input = SHA-256(JCS(proof without proofValue))
             || SHA-256(JCS(envelope without proof and tsa_timestamp))
```

`eddsa-jcs-2022` signs that with Ed25519. `ecdsa-jcs-2019` signs it with
ECDSA P-256 and SHA-256, and `proofValue` carries the raw `r || s`.
JCS is RFC 8785.

## Updating the fixture

`fixtures/cpp_signed_envelope.json` is produced by the C++ core. After
an intentional format change, regenerate it from the test binary:

```bash
SENTLOG_UPDATE_FIXTURES=1 ./tests/sentlog_tests \
  --gtest_filter='VerifierFixtureTest.*'
```
