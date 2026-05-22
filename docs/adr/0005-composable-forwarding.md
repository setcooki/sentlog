# ADR 0005 — Composable forwarding for all output destinations

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.9](../technical-specification.md#1-architectural-decisions)

## Context

The original whitepaper had a single `audit_clip_enabled: Boolean` knob
gating one fixed audit channel pointing at a SDK-named backend. Two
problems:

1. **Named backends couple the SDK to specific vendors.** Any named
   backend can be discontinued, repriced, or repositioned outside the
   SDK's control.
2. **One audit channel is too narrow.** Real integrators want to fan
   the same result to several places — content moderation, analytics
   warehouse, fraud queue, on-chain attestation, accessibility
   transcripts — with different payload subsets per destination.

## Decision

**All output goes through composable forwarders.** A forwarder is:

```
Forwarder { name, sink, projection, sample_rate, retry }
```

where:

- **Sink** is a primitive: `HttpSink`, `WebhookSink`, `LocalQueueSink`,
  `CallbackSink`. Not a *destination* — the integrator names URLs.
- **Projection** is what subset of the result to send: `Full`,
  `Full+AudioClip`, `ScoresOnly`, `TranscriptOnly`,
  `EvidenceFingerprint`, `EvidenceClip`, or `Custom`.
- **`sample_rate`** lets the integrator implement the whitepaper's
  "15% audit" pattern as a config option, not a code path.
- **`retry`** is per-forwarder backoff configuration.

After every `evaluate()`, the SDK fans the result through all
configured forwarders on the I/O thread. Forwarders do not block
`evaluate()`'s return to the caller.

`forwarders: []` is the default — the SDK transmits nothing unless the
integrator explicitly wires up at least one forwarder.

## Consequences

**Enabled:**

- The SDK is backend-agnostic. The Hume sunset stops being our
  problem; integrators name destinations.
- The same primitive supports analytics, fraud, content moderation,
  audit, accessibility — everything an integrator wants to ship the
  result to.
- The audit pattern from the whitepaper becomes a configuration
  example, not a special case.
- Privacy posture is determined entirely by the integrator's forwarder
  configuration (see the table in spec §5.2).

**Constrained:**

- The SDK is responsible for retry, backoff, and offline queueing.
  Bug surface is real — needs dedicated tests in Phase 4.
- The `CallbackSink` escape hatch covers "anything else" but is
  awkward for streaming destinations. WebSocket/gRPC sinks remain
  post-v1.0.
- Misconfiguration risk: an integrator could accidentally configure a
  `Full+AudioClip` forwarder when they meant `EvidenceFingerprint`,
  shipping voice they didn't intend to. Documentation calls it out;
  the `Projection` enum is verbose enough that the intent is explicit.
