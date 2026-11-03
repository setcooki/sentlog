# Sentlog

**A universal, device-first evidence and presence SDK.**

Captures a short voice interaction (optionally augmented with motion
and facial-affect signals), produces a multi-dimensional presence
assessment, and emits the result as a signed, W3C-Verifiable-Credentials
envelope. Runs entirely on-device. Inert by default. MIT.

> ⚠️ **Status: pre-v1.0 — NOT production ready.** The architecture
> is in place and CI is green, but the SDK has not been through its
> ≥72 h on-device soak, has not been cut as v1.0, and the
> face-affect MLP is a placeholder pending the labelled corpus
> (gated to months 7–12 per `docs/rollout-plan.md`). The public API
> surface and envelope wire format may shift before v1.0; the
> deprecation cycle in [`docs/deprecation-policy.md`](docs/deprecation-policy.md)
> only kicks in from v1.0 onward. Use on `main` at your own risk.

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
transcript only, fingerprint, clip, custom). The SDK doesn't send
forwarder traffic itself: the integrator's dispatcher delivers HTTP and
webhook envelopes. The SDK has no opinion on destinations.

## Status

**Pre-v1.0.** Phases 0 through 7.5 (architecture pass) have shipped;
see the notice above for what that does and does not mean. What is
left before v1.0 is tracked in [docs/rollout-plan.md](docs/rollout-plan.md).

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

## Repository structure

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
├── ios/                   # Swift package (Swift + C interop)
├── rn/                    # @sentlog/react-native
├── capacitor/             # @sentlog/capacitor
├── models/                # training + quantisation pipelines (not bundled)
├── tools/
│   ├── benchmark-harness/
│   ├── reference-consumer/   # HTTP server for HttpSink testing
│   ├── evidence-verifier/    # single-file Python verifier
│   ├── sample-capacitor/     # Capacitor sample snippets (not a runnable app)
│   └── sample-react-native/  # React Native sample snippets (not a runnable app)
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
| Foundational dependencies | whisper.cpp (MIT), Silero VAD (MIT), AASIST-L (MIT), ONNX Runtime (MIT). Audio fingerprint and prosody features are homegrown MIT — see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). |

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

- Phases 0 through 7.5 shipped; ≥72 h soak and the v1.0 cut still open.
- Open items listed in
  [docs/rollout-plan.md §5](docs/rollout-plan.md#5-open-items).

## Support

If Sentlog is useful to you and you'd like to support the work:

[☕ Buy me a coffee](https://www.buymeacoffee.com/setcooki)

## License

MIT. See [LICENSE](LICENSE).

Third-party attributions (upstream models, datasets, and libraries
the SDK builds on) are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
