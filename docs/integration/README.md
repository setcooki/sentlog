# Integrating Sentlog

Sentlog is the evidence-producing layer of a presence/liveness flow.
Your app captures voice (and optionally motion + face), the SDK
returns a multi-dimensional assessment + a W3C VC 2.0 envelope; you
decide what to do with it.

> ⚠️ **Pre-v1.0 — NOT production ready.** The architecture pass is
> complete and CI is green, but the SDK has not been through its
> ≥72 h on-device soak and has not been cut as v1.0. The face-affect
> head is a placeholder (`model_versions.affect = "placeholder-derived"`
> until the labelled corpus lands in months 7–12). The public API
> surface and envelope wire format may shift; the
> [deprecation policy](../deprecation-policy.md) only applies from
> v1.0 onward. Build against the SDK now to inform integration design
> + give us feedback; don't ship it to production users yet.

This directory is the integrator-facing documentation. The
[whitepaper](../whitepaper.md) covers *why*; the [technical
specification](../technical-specification.md) covers *what*; this
section covers *how* — by feature, not by API.

## Pick your platform

| Stack | SDK package |
|---|---|
| Native iOS (Swift) | `Sentlog` Swift Package (repo root [`Package.swift`](../../Package.swift)) |
| Native Android (Kotlin) | `me.cooki.sentlog:sentlog` AAR (`android/sentlog/`) |
| React Native | `@sentlog/react-native` (`rn/`) |
| Capacitor | `@sentlog/capacitor` (`capacitor/`) |

On iOS, add the repository URL as a Swift package dependency, with an iOS
16 deployment target:
`.package(url: "https://github.com/setcooki/sentlog.git", branch: "main")`.
Use `branch` until the first tagged release. The bridges' iOS requirements
are in [`rn/README.md`](../../rn/README.md) and
[`capacitor/README.md`](../../capacitor/README.md).

Every binding exposes the same core surface — `initialize`, `startCapture`,
`evaluate`, `cancel`, `destroy`, `getPermissions`, `getCapabilities`,
`modelVersions`, `checkForUpdates`, `drainQueue`, plus the optional
signing + dispatcher + attestation + face-frame extensions. The JS
bridges expose only `checkForUpdates(manifestJson)`. `setHardwareSigner`,
`setAudioEncoder`, `setTsaProvider` and the bundled helpers are
Kotlin / Swift only. The wire format of the envelope is identical across
platforms.

## Samples

`tools/sample-react-native/` and `tools/sample-capacitor/` are
copy-paste-ready snippets, not runnable apps, demonstrating the golden
path: initialize, capture, evaluate, present the result, report a
permission-denied error. They are the recommended starting point — easier than
reading the API reference cold.

## Feature guides

The features that need a paragraph of explanation past the API
signature get their own page:

- [hardware-signing.md](hardware-signing.md) — wire a Secure Enclave
  / Android KeyStore key into the envelope's proof block. Promotes
  `evidence_grade` to `EnclaveSigned`. Optional.
- [ota.md](ota.md) — fetch + verify model-bundle manifests via the
  built-in `checkForUpdates()` flow, or override `manifest_url` +
  `manifest_pubkeys` to publish your own. Sentlog doesn't publish a
  manifest yet, so you need the override for now.
- [telemetry.md](telemetry.md) — the `TelemetryEvent` union and what
  each event tells you about an evaluate / dispatch cycle.
- [device-attestation.md](device-attestation.md) — push a Play Integrity
  / App Attest token. Promotes `evidence_grade` to `DeviceAttested`.
  Optional.
- [face-affect.md](face-affect.md) — push derived face-affect values
  from your own camera + landmark pipeline. Optional.

Features that work the same way on every platform and are
straightforward from the spec — `startCapture`, `evaluate`, drain
queue, projections — are documented in
[../technical-specification.md §3 + §4](../technical-specification.md).

## Permissions

`getPermissions()` (available after `initialize`) reports what the OS
has granted today. The SDK
**never requests permissions on your behalf** — your app prompts when
it judges the moment is right, then calls `initialize`. See
[../permissions.md](../permissions.md) for the per-capability mapping
across iOS + Android.

## Forwarders + posture

The default is `forwarders: []` — nothing transmits, nothing is
queued. Adding a forwarder is the only way data leaves the device.
Pick a projection to control what's in the envelope; see
[../technical-specification.md §4.2 + §4.3](../technical-specification.md).

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
