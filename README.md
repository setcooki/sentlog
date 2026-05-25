# Sentlog

**A universal, device-first evidence and presence SDK.**

Captures a short voice interaction (optionally augmented with motion
and facial-affect signals), produces a multi-dimensional presence
assessment, and emits the result as a signed, W3C-Verifiable-Credentials
envelope. Runs entirely on-device. Inert by default. MIT.

## What it is

Sentlog is a mobile SDK for iOS and Android with first-class bridges
for React Native and Capacitor. It is not a trust SDK, not an
authentication SDK, not a fraud SDK — it is the *evidence-producing
layer* on which integrators build those things.

Four design pillars:

- **Universal** — native iOS, native Android, Capacitor-wrapped PWAs,
  React Native apps. One shared C++ core; thin per-platform and
  per-framework bridges. `@sentlog/web` (browser WASM) is a defined
  post-MVP deliverable.
- **Device-first** — capture and inference happen on the device.
  Defaults are inert: nothing transmits, nothing runs in the
  background, no cloud is implied by integration.
- **Evidence** — every result is wrapped in a W3C Verifiable
  Credentials 2.0 envelope with Ed25519 or ECDSA-P256 proofs (the
  latter via Secure Enclave / Android KeyStore when available),
  model versions, timestamps, and a homegrown MIT perceptual audio
  fingerprint (`sentlog-pcp-v1`).
  Optional RFC 3161 TSA timestamps when an integrator configures a
  TSA endpoint.
- **Presence** — the subject of the evidence. Liveness, engagement,
  direction, presence proper, and optionally facial affect — each
  exposed separately so integrators compose their own verdicts.

What the SDK ships, in one call:

```kotlin
val result: SentlogResult = Sentlog.evaluate()
// → transcript, liveness, engagement, direction, presence, affect,
//   confidence, fingerprint, model versions, diagnostics, evidence_grade
```

What leaves the device: nothing, unless the integrator configures a
forwarder. Forwarders are pluggable sinks (HTTP, webhook, local queue,
callback) with per-sink projections (full result, scores only,
transcript only, fingerprint, clip, custom). The SDK has no opinion
on destinations.

## Status

**Pre-Phase-0.** The repository currently contains only the planning
documents. No source code, build system, models, or sample apps exist
yet. Phase 0 (foundations) and the 16-week MVP to v1.0 are documented
in [docs/rollout-plan.md](docs/rollout-plan.md).

## Documentation

Three documents cover what Sentlog is, how it works, and how it ships:

- **[docs/whitepaper.md](docs/whitepaper.md)** — vision, foundational
  principles, positioning, multi-modal signal rationale, data strategy.
  Start here if you're evaluating Sentlog as a building block.
- **[docs/technical-specification.md](docs/technical-specification.md)** —
  architectural decisions, repository layout, the full API surface,
  composable forwarding, security model, versioning policy. Start here
  if you're integrating or contributing.
- **[docs/rollout-plan.md](docs/rollout-plan.md)** — 16-week MVP phases,
  Android benchmark gate, testing and observability, risk register,
  open items. Start here if you're executing.

[CLAUDE.md](CLAUDE.md) covers conventions for AI-coded contributions.

## Planned repository structure

Full layout in
[docs/technical-specification.md §2](docs/technical-specification.md#2-repository-layout).
Summary:

```
sentlog/
├── LICENSE                # MIT
├── README.md              # this file
├── SECURITY.md            # vulnerability disclosure policy
├── CLAUDE.md              # AI-coded contribution guidelines
├── shared/                # C++17 core — narrow C ABI, heavily tested
├── android/               # AAR (Kotlin + JNI)
├── ios/                   # XCFramework (Swift + C interop)
├── rn/                    # @sentlog/react-native
├── capacitor/             # @sentlog/capacitor
├── models/                # training + quantisation pipelines (not bundled)
├── tools/
│   ├── benchmark-harness/
│   ├── reference-consumer/   # HTTP server for HttpSink testing
│   ├── evidence-verifier/    # single-file Python verifier
│   ├── sample-capacitor/     # Svelte/Ionic + Capacitor sample app
│   └── sample-react-native/  # React Native sample app
└── docs/
    ├── whitepaper.md
    ├── technical-specification.md
    └── rollout-plan.md
```

## Quick comparison

| Concern | Sentlog |
|---|---|
| Runtime | On-device only (iOS, Android, Capacitor, React Native; WASM post-MVP) |
| Output | W3C Verifiable Credentials 2.0 envelope (signed, durable) |
| Signing | Ed25519 (software) / ECDSA-P256 (Secure Enclave / KeyStore) |
| Timestamping | Device timestamp; optional RFC 3161 TSA |
| Networking | None by default. Pluggable forwarders, named by the integrator. |
| State | No user model, no sessions, no accounts. Opaque `session_id` passthrough. |
| Personal data | Voice and video never leave the device unless explicitly configured. Fingerprint forwarding ships ~120 bytes of non-invertible perceptual hash. |
| Licence | MIT |
| Foundational dependencies | whisper.cpp (MIT), Silero VAD (MIT), AASIST-L (MIT), MediaPipe Face Mesh (Apache 2.0). Audio fingerprint and prosody features are homegrown MIT — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). |

## Roadmap

16-week MVP to v1.0 internal release; details in
[docs/rollout-plan.md §1](docs/rollout-plan.md#1-phased-rollout).

| Phase | Weeks | Focus |
|---|---|---|
| 0 | 0 | Foundations — ADRs, repo scaffolding, AASIST-L licence verification |
| 1 | 1–2 | Liveness path + Android benchmark gate |
| 2 | 3–4 | Transcription, transcript exposure, direction scoring |
| 3 | 5–6 | Multi-modal trust (motion) + prosody features |
| 4 | 7–8 | Composable forwarding + W3C Verifiable Credentials envelope |
| 5 | 9–10 | Video affect (sentiment enrichment) + permissions |
| 6 | 11–12 | React Native + Capacitor bridges |
| 7 | 13–16 | OTA + hardware-backed signing + hardening + soak + v1.0 |
| 8 | M4+ | Public release readiness (gated by corpus milestones) |
| 9 | post-MVP | `@sentlog/web` (browser WebAssembly) |

## Status snapshot

- Documentation phase complete.
- Phase 0 not yet started.
- Open items for product and Phase 0 engineering gates listed in
  [docs/rollout-plan.md §5](docs/rollout-plan.md#5-open-items).

## Support

If Sentlog is useful to you and you'd like to support the work:

[☕ Buy me a coffee](https://www.buymeacoffee.com/setcooki)

## License

MIT. See [LICENSE](LICENSE).

Third-party attributions (upstream models, datasets, and libraries
the SDK builds on) are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
