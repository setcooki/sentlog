# ADR 0003 — Baseline-bundled, Ed25519-signed-manifest OTA

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.3](../technical-specification.md#1-architectural-decisions)

## Context

Model improvements happen faster than mobile release cycles. The
engagement and liveness heads will retrain as the labelled corpus
grows, and the SDK has to be able to ship those improvements without
forcing the integrator to publish a new app version. At the same
time, swapping model files on disk is a security-sensitive operation:
an attacker who can replace the model can make the SDK report
arbitrary scores.

Three OTA approaches were considered:

1. **No OTA — bundle only.** Simple, no security surface. But model
   improvements require app store releases on the integrator's
   timeline.
2. **Firebase ML Custom Models.** Hosted, has built-in delivery and
   A/B. Still labelled beta. TFLite only — no Core ML support. Adds
   vendor lock-in on the delivery layer.
3. **Roll-your-own signed-manifest channel.** Same pattern as Sigstore
   / Cosign / minisign: an Ed25519-signed JSON manifest pointing at
   model artefacts on a CDN. SDK pins public keys and verifies before
   any swap.

## Decision

**Bundle the baseline model in the SDK artefacts.** First-run works
offline; app store review sees the actual model.

**OTA updates happen via an Ed25519-signed JSON manifest** at a URL
configured in `SentlogConfig`. The SDK pins **two rotatable public
keys** (one "active", one "standby") to allow seamless rotation. Each
model file has its SHA-256 included in the manifest; the SDK verifies
both the manifest signature and the file hash before swapping
anything on disk.

**Fetch is integrator-triggered only,** via the explicit
`sentlog_check_for_updates()` API. `sentlog_evaluate()` never
initiates OTA. This is consistent with the inert-by-default principle
([whitepaper §2](../whitepaper.md#2-foundational-principle--a-pure-library-inert-by-default)) —
the SDK does no background work.

Integrators may override both the manifest URL and the pinned keys to
ship their own model improvements. Overriding requires overriding
both (mixed-source configuration is rejected at `init()`).

## Consequences

**Enabled:**

- Model improvements ship at our cadence, not the integrators'
  release cadence.
- The SDK has zero network behaviour unless the integrator calls
  `check_for_updates()`. Truly inert by default.
- Integrators can self-host the manifest with their own keys —
  reinforces the open-source positioning and removes "OTA is our
  cloud" as a critique.
- Key rotation by overlap is built in.

**Constrained:**

- Apple Store §2.5.2 review may scrutinise apps that download "code"
  — but Apple itself treats model *weights* as data, and major apps
  (Shazam, Snap, Google Translate) ship model updates this way
  routinely. Risk acknowledged in the risk register.
- A failed verification leaves the previously known-good model in
  place; the SDK does *not* fall back to an unverified file under any
  circumstance. This may produce stuck-on-old-model scenarios; the
  telemetry event surfaces those to the integrator.

## Threat model

The signing key compromising the entire SDK fleet is the largest
single risk. Mitigations:

- Two pinned public keys, rotation by overlap.
- Signing key custody is out-of-scope for the SDK itself; integrators
  who override `manifest_pubkeys` own their own key custody.
- The reference verifier (`tools/evidence-verifier/`) and the SDK use
  the same verification path, so the verification logic is exercised
  in two independent contexts.
