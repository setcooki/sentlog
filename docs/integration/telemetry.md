# Telemetry

Telemetry is opt-in. Pass an `onTelemetry` callback to `initialize`
to receive structured events; pass `nil` / `null` and the SDK runs
silent. Events carry no PII, no audio bytes, no transcript text, no
video frames, no user identifier — the union is exhaustive (see
`shared/types/sentlog.ts` for the source of truth) and the SDK never
adds fields beyond it.

Use telemetry to detect silent fallbacks (hardware-signing
unavailable, forwarder drops), measure pipeline cost in production,
and build histograms of score distributions for the populations your
app serves.

## Event reference

Every event is a JSON object with a `type` discriminator plus the
fields shown below. Events carry no timestamp or session id. The bindings
deserialize into a sealed-class / enum hierarchy; in JS the union is
typed in `@sentlog/react-native` / `@sentlog/capacitor`.

### `PipelineTiming`

```json
{ "type": "PipelineTiming", "stage": "evaluate", "duration_ms": 142 }
```

One per successful `evaluate()`. `stage` is currently always
`"evaluate"` (full Pipeline::run wall-clock); per-substage timings
(vad, transcript, prosody) are reserved for a future expansion.

Use to track p50 / p95 evaluation cost across devices, gate slow
devices, or spot regressions after a model update.

### `ScoreHistogram`

```json
{ "type": "ScoreHistogram", "field": "liveness.score", "bucket": "0.7" }
```

One per top-level score (`liveness.score`, `presence.summary`,
`engagement.score`) on every successful `evaluate()`. `bucket` is the
lower bound of a 0.1-wide window — `"0.0"` through `"1.0"`.

The discretisation is permanent: raw scores never appear in
telemetry. Use to monitor score distributions in production without
ingesting the underlying assessments.

### `ForwarderOutcome`

```json
{ "type": "ForwarderOutcome", "forwarder_name": "audit-log",
  "outcome": "ok",      "attempts": 1 }
{ "type": "ForwarderOutcome", "forwarder_name": "audit-log",
  "outcome": "dropped", "attempts": 0 }
```

One per dispatch attempt per forwarder. Outcomes:

- `ok` — envelope crossed the FFI boundary into the integrator's
  dispatcher (HTTP / webhook / callback) OR was written to the
  local-queue file. `attempts = 1` (the SDK has no forwarder HTTP sender and
  no retry; your dispatcher does the delivery and any retrying). So `ok`
  doesn't mean the destination received it.
- `dropped` — envelope was not delivered. Possible reasons (not yet
  surfaced as a field, but inferable from configuration):
  - sampling: `sample_rate < 1.0` and this run rolled below
  - `no_dispatcher_registered` (no `setEnvelopeDispatcher` call)
  - `local_queue_open_failed`
  - `envelope_build_failed`
  - `hardware_signing_required_failed`
  - `tsa_required_failed`
  - `unknown_projection`

The `retry` outcome is defined in the union but not emitted yet.

### `CaptureAborted`

```json
{ "type": "CaptureAborted", "reason": "cancel_during_capture" }
```

Emitted only when `cancel()` actually interrupts an active phase.
Defensive cancel-on-idle is silent — your binding can call
`cancel()` unconditionally on teardown. Reasons: `cancel_during_
capture`, `cancel_during_evaluate`.

### `ManifestVerifyFailed`

```json
{ "type": "ManifestVerifyFailed",
  "reason": "bad_signature",
  "attempted_version": "0.0.6" }
```

Emitted when `checkForUpdates()` rejects a manifest. `reason` is one
of: `invalid_json`, `missing_fields`, `unknown_key_id`,
`bad_signature`, `unsupported_schema_version`. `attempted_version` is
the manifest's claimed version when JSON parsed; empty when parse
failed.

### `HardwareSigningUnavailable`

```json
{ "type": "HardwareSigningUnavailable",
  "reason": "signer_call_failed",
  "fell_back_to": "software_ed25519" }
```

Emitted when `use_hardware_signing` is true, the hardware-signer
callback returns failure (`signer_call_failed`) or none is registered
(`signer_not_registered`), AND `hardware_signing_required` is false
(so the SDK fell back to the software path). `fell_back_to` is
`software_ed25519`, or `none` when no software key is set and the
envelope ships unsigned. When `hardware_signing_required` is true the
envelope is dropped instead — see the corresponding `ForwarderOutcome`.

### `TsaRequestFailed`

```json
{ "type": "TsaRequestFailed",
  "tsa_url": "https://tsa.example.com",
  "reason": "tsa_provider_returned_error" }
```

Emitted when `tsa_endpoint` is set and no timestamp could be attached.
`reason` is one of: `no_tsa_provider_registered`,
`tsa_provider_returned_error`, `no_proof_to_stamp`,
`envelope_parse_failed`. The envelope ships without `tsa_timestamp`,
unless `tsa_required` is true, in which case it is dropped.

### `ModelLoadError`, `PermissionDenied`

Not implemented yet: both are defined in the union, but nothing emits
them today.

## Sample handler

```swift
try Sentlog.initialize(config: cfg) { event in
    switch event {
    case .pipelineTiming(let t):
        metrics.histogram("sentlog.evaluate.ms").observe(Double(t.durationMs))
    case .scoreHistogram(let h):
        metrics.counter("sentlog.score.\(h.field).\(h.bucket)").increment()
    case .hardwareSigningUnavailable(let e):
        log.warn("sentlog: hardware signing fell back: \(e.reason)")
    case .forwarderOutcome(let o) where o.outcome == .dropped:
        log.warn("sentlog: forwarder \(o.forwarderName) dropped envelope")
    default:
        break
    }
}
```

```kotlin
Sentlog.initialize(cfg, applicationContext) { ev ->
    when (ev) {
        is TelemetryEvent.PipelineTiming   -> metrics.observe("evaluate.ms", ev.durationMs)
        is TelemetryEvent.ScoreHistogram   -> metrics.inc("score.${ev.field}.${ev.bucket}")
        is TelemetryEvent.HardwareSigningUnavailable ->
            log.w("sentlog", "hardware signing fallback: ${ev.reason}")
        is TelemetryEvent.ForwarderOutcome
            if ev.outcome == TelemetryEvent.ForwarderOutcome.Outcome.Dropped ->
            log.w("sentlog", "forwarder ${ev.forwarderName} dropped envelope")
        else -> {}
    }
}
```

## What telemetry is *not*

- Not an alternative to logging. The SDK does not write to stderr,
  stdout, or any log file by default. Telemetry events are values you
  observe; you decide where they go.
- Not a billing channel. There is no telemetry endpoint anywhere in
  the SDK to phone home to. The callback is the only outbound path.
- Not exhaustive of every SDK internal. The union is the
  contractually-stable surface; new event types are additive across
  minor versions, never breaking.
