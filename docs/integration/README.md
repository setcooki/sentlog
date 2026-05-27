# Integrating Sentlog

Sentlog is the evidence-producing layer of a presence/liveness flow.
Your app captures voice (and optionally motion + face), the SDK
returns a multi-dimensional assessment + a W3C VC 2.0 envelope; you
decide what to do with it.

This directory is the integrator-facing documentation. The
[whitepaper](../whitepaper.md) covers *why*; the [technical
specification](../technical-specification.md) covers *what*; this
section covers *how* — by feature, not by API.

## Pick your platform

| Stack | SDK package |
|---|---|
| Native iOS (Swift) | `Sentlog` Swift Package (`ios/Package.swift`) |
| Native Android (Kotlin) | `me.cooki.sentlog:sentlog` AAR (`android/sentlog/`) |
| React Native | `@sentlog/react-native` (`rn/`) |
| Capacitor | `@sentlog/capacitor` (`capacitor/`) |

Every binding exposes the same surface — `initialize`, `startCapture`,
`evaluate`, `cancel`, `destroy`, `getPermissions`, `getCapabilities`,
`modelVersions`, `checkForUpdates`, `drainQueue`, plus the optional
signing + dispatcher + hardware-signing extensions. The wire format
of the envelope is identical across platforms.

## Working samples

`tools/sample-react-native/` and `tools/sample-capacitor/` are
copy-paste-ready snippets demonstrating the golden path: initialize,
capture, evaluate, present the result, handle a permission-denied
recovery. They are the recommended starting point — easier than
reading the API reference cold.

## Feature guides

The features that need a paragraph of explanation past the API
signature get their own page:

- [hardware-signing.md](hardware-signing.md) — wire a Secure Enclave
  / Android KeyStore key into the envelope's proof block. Promotes
  `evidence_grade` to `EnclaveSigned`. Optional.
- [ota.md](ota.md) — fetch + verify model-bundle manifests via the
  built-in `checkForUpdates()` flow, or override `manifest_url` +
  `manifest_pubkeys` to publish your own.
- [telemetry.md](telemetry.md) — the `TelemetryEvent` union and what
  each event tells you about an evaluate / dispatch cycle.

Features that work the same way on every platform and are
straightforward from the spec — `startCapture`, `evaluate`, drain
queue, projections — are documented in
[../technical-specification.md §3 + §4](../technical-specification.md).

## Permissions

`getPermissions()` reports what the OS has granted today. The SDK
**never requests permissions on your behalf** — your app prompts when
it judges the moment is right, then calls `initialize`. See
[../permissions.md](../permissions.md) for the per-capability mapping
across iOS + Android.

## Forwarders + posture

The default is `forwarders: []` — nothing transmits, nothing is
queued. Adding a forwarder is the only way data leaves the device.
Pick a projection to control what's in the envelope; see
[../technical-specification.md §3.4 + §4.2](../technical-specification.md).

| Posture | Config |
|---|---|
| Pure on-device evaluation | `forwarders: []` |
| Offline-first, drain later | `local_queue` forwarder + call `drainQueue()` on connectivity |
| Real-time fire-and-forget | `http` / `webhook` forwarder + the integrator-supplied dispatcher |
| Sampled telemetry | any of the above with `sample_rate < 1.0` |

## Getting help

- Bug in the SDK: open an issue against the GitHub repo with a
  reproduction.
- Conceptual question: re-read the [whitepaper](../whitepaper.md). The
  positioning section tells you whether Sentlog is the right shape for
  what you're trying to build.
- API drift across binding versions: check the [technical
  specification](../technical-specification.md) §1 decision rows —
  the load-bearing choices are documented there with the rationale.
