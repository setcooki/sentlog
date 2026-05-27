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

Every event is a JSON object with a `type` discriminator. The bindings
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
  local-queue file. `attempts = 1` (the SDK does not own wire-level
  retry; the binding's HTTP client does).
- `dropped` — envelope was not delivered. Possible reasons (not yet
  surfaced as a field, but inferable from configuration):
  - sampling: `sample_rate < 1.0` and this run rolled below
  - `no_dispatcher_registered` (no `setEnvelopeDispatcher` call)
  - `local_queue_open_failed`
  - `envelope_build_failed`
  - `hardware_signing_required_failed`

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

Emitted when the hardware-signer callback returns failure AND
`hardware_signing_required` is false (so the SDK fell back to the
software path). When `hardware_signing_required` is true the
envelope is dropped instead — see the corresponding `ForwarderOutcome`.

### `ModelLoadError`, `PermissionDenied`, `TsaRequestFailed`

These are emitted by platform-side code paths (model loading,
permission requests, TSA timestamping). `ModelLoadError` and
`PermissionDenied` are wired today; `TsaRequestFailed` is reserved
for the deferred TSA work (Phase 4h).

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
            if ev.outcome == ForwarderOutcomeKind.DROPPED ->
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
