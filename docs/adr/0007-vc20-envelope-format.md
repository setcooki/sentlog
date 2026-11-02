# ADR 0007 — W3C Verifiable Credentials 2.0 envelope format

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.25](../technical-specification.md#1-architectural-decisions)

## Context

The SDK ships evidence (ADR 0006). Evidence has to be verifiable by
tooling that has only the envelope and a public key, weeks or months
later. Three envelope-format options were considered:

1. **Bespoke JSON.** Smallest immediate cost. But locks the project
   into a wire format that no other tooling understands, and forces
   us to maintain our own verifier as the only valid implementation.
2. **JWT / JWS.** Mature, broadly understood. But JWT is designed
   around short-lived auth claims, not durable evidence — and the
   data model is flat, awkward for the multi-dimensional shape of a
   Sentlog result.
3. **W3C Verifiable Credentials 2.0.** A standards-track data model
   explicitly designed for verifiable claims with durable proofs.

## Decision

The envelope conforms to **W3C Verifiable Credentials 2.0**. The
required structure:

```
{
  "@context": [
    "https://www.w3.org/ns/credentials/v2",
    "https://sentlog.org/contexts/evidence/v1"
  ],
  "type": ["VerifiableCredential", "SentlogEvidence"],
  "issuer": "<integrator-supplied URI or DID>",
  "validFrom": "<ISO 8601>",
  "credentialSubject": { <Sentlog payload> },
  "proof": {
    "type": "DataIntegrityProof",
    "cryptosuite": "eddsa-jcs-2022",    // or "ecdsa-jcs-2019" for hardware (Phase 7)
    "created": "<ISO 8601>",
    "verificationMethod": "<integrator-supplied key id>",
    "proofPurpose": "assertionMethod",
    "proofValue": "z<base58btc signature>"
  },
  "tsa_timestamp": { ... }              // optional, RFC 3161
}
```

The proof cryptosuite is **`eddsa-jcs-2022`** when signing in software
(Ed25519), or **`ecdsa-jcs-2019`** when signing via hardware
(Secure Enclave / KeyStore — ECDSA-P256; Phase 7).

JCS canonicalisation (RFC 8785) rather than RDFC because the latter
requires a URDNA2015 implementation (~5000 lines of RDF dataset
canonicalisation logic) with no small embedded variant worth
vendoring. Both `eddsa-jcs-2022` and `ecdsa-jcs-2019` are
W3C-registered DataIntegrity cryptosuites; the choice is
implementation-driven, not interoperability-driven.

**Sentlog does not run a key infrastructure.** The integrator supplies
`issuer` (any URI or DID) and `signing_key_id` at init. The SDK ensures
the envelope is signed by the configured key — it makes no claim about
who the issuer is or whether the key chain is trusted.

The envelope wire format is locked. An explicit `envelope_format`
field inside `credentialSubject` lets verifiers refuse formats they
don't understand. Breaking changes to the envelope require an
`envelope_format` major bump and a co-existence period.

## Consequences

**Enabled:**

- Sentlog evidence flows directly into VC-aware ecosystems (wallets,
  DID resolvers, presentation exchanges) without re-wrapping. Any
  off-the-shelf VC verifier can validate the signature.
- The `tools/evidence-verifier/` reference implementation has a
  standards-track contract to test against, not a bespoke format.
- On-chain attestation flows are well-supported: an oracle can hash
  the envelope under a domain separator and write the digest as proof.
- Future alignment with IETF SCITT or similar evidence-supply-chain
  standards becomes feasible.

**Constrained:**

- The envelope is more verbose than a bespoke JSON would be. Adds a
  few hundred bytes per envelope — negligible for HttpSink, real for
  `EvidenceFingerprint` at 15% sample on large volumes (still well
  under 1 KB total per envelope).
- Integrators have to provide an `issuer` value. The SDK validates it
  at init but never interprets it; doesn't require a real DID.
- Hardware-backed signing on iOS requires ECDSA-P256 (Secure Enclave
  doesn't do Ed25519). The proof cryptosuite differs between
  software-signed and hardware-signed envelopes — verifiers must
  handle both, which the reference verifier does.
