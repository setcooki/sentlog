# ADR 0006 — The SDK output is evidence, not verdicts

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.22](../technical-specification.md#1-architectural-decisions)

## Context

Early drafts framed Sentlog as a "trust scoring SDK" — a system that
answers "is this a real human?" with a yes/no verdict (or a threshold
test). Two problems with that framing:

1. **It overclaims.** Empirical research on modern voice cloning
   (ElevenLabs, OpenAI, XTTS) shows on-device liveness sitting at
   60–80% clean and 55–70% over telephony. A yes/no verdict over
   numbers that bad is not honest.
2. **It centralises a decision that belongs to integrators.** Each
   integrator has its own threshold appetite, calibration, and
   downstream consequences. The SDK has no business prejudging that.

## Decision

The SDK's output is **evidence**, not a verdict. Evidence has four
properties the SDK is designed to produce:

- **Verifiable** — signed by an integrator-owned key (Ed25519 or
  ECDSA-P256 hardware-backed).
- **Durable** — wrapped in a W3C Verifiable Credentials 2.0 envelope
  (ADR 0007) that can be stored, replayed, and verified weeks or
  months later by tooling that has only the envelope and a public key.
- **Multi-dimensional** — separate fields for liveness, engagement,
  direction, presence, optional affect, fingerprint, transcript,
  model versions, evidence grade. Integrators compose verdicts.
- **Demographically honest** — accuracy reported per-attack-class and
  (post-MVP) per-demographic-segment; no headline accuracy.

Concrete consequences in the API:

- Scores are **raw [0,1] probabilities.** No SDK-side calibration or
  capping. `thresholds_met` is the only field that consults the
  threshold values from config — and that's a *threshold-test result*,
  not an authentication verdict.
- Forwarder projections are named *Evidence*, not *Audit*:
  `EvidenceFingerprint`, `EvidenceClip`. The SDK ships evidence;
  downstream consumers audit it.
- `evidence_grade` declares the assurance level (`DeviceUnattested`,
  `DeviceAttested`, `EnclaveSigned`) so downstream consumers can apply
  differential trust without reinventing it.

## Consequences

**Enabled:**

- Honest external messaging that survives scrutiny.
- Composable downstream verdicts — integrators set their own bars and
  carry their own legal/compliance responsibility for the decisions.
- The SDK is structurally aligned with regulatory frames that ask
  "what is the evidence?" rather than "what's the answer?"
- The on-chain attestation use case becomes natural: a signed
  envelope is a primitive an oracle can hash and write on-chain.

**Constrained:**

- We cannot market Sentlog as an "authenticator" or "fraud verdict"
  service.
- Integrators who want a simple yes/no must build it themselves on
  top of the SDK's scores and thresholds.
- Documentation and onboarding have to explain the difference, which
  is more work than shipping a simple `is_real: Boolean`.
