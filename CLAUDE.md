# CLAUDE.md — Sentlog

## What is Sentlog

A universal, device-first evidence and presence SDK for iOS and Android.
Captures a short voice interaction (optionally augmented with motion
and facial-affect signals), produces a multi-dimensional presence
assessment, and emits the result as a signed, W3C-Verifiable-Credentials
envelope. Inert by default — no SDK-owned cloud, no autonomous
behaviour. MIT, open-source from day one.

Documentation source of truth:
- [docs/whitepaper.md](docs/whitepaper.md) — vision, positioning, multi-modal rationale, data strategy.
- [docs/technical-specification.md](docs/technical-specification.md) — architectural decisions, API, contracts, security, versioning.
- [docs/rollout-plan.md](docs/rollout-plan.md) — 16-week MVP phases, benchmark gate, testing, risks, open items.

If a question is about *what Sentlog is or why*, read the whitepaper.
If it's about *how it works or what the API is*, read the technical
specification. If it's about *when something lands or what gates it*,
read the rollout plan.

## Status

**Pre-v1.0 — NOT production ready.** Phases 0 through 7.5
(architecture pass) have shipped. The shared C++ core, both
platform bindings (iOS + Android), both JS bridges (RN +
Capacitor), the OTA path, the hardware-signing pluggable
architecture (with SDK-bundled `SecureEnclaveSigner` +
`KeyStoreSigner` helpers), the device-attestation push API (with
SDK-bundled `AppAttestAttester` + `PlayIntegrityAttester`
helpers), the audio-encoder + TSA-provider callback patterns
(with SDK-bundled `OpusAudioEncoder` + `RfcTsaProvider` native
helpers), the face-affect scaffolding, telemetry, integration
docs, deprecation policy, and three Python tools (evidence-verifier,
reference-consumer, benchmark-harness) all exist and pass CI.

Still genuinely open before v1.0:
  - **≥72 h soak runs** across both integrator apps (real-device,
    not CI-loop work)
  - **v1.0 cut + release notes + upgrade guide**
  - **Real face-affect MLP** (placeholder ships; corpus-gated to
    months 7–12 per the rollout plan, with
    `model_versions.affect = "placeholder-derived"` so consumers
    can detect the stub)
  - **Phase 8 work**: per-attack-family benchmarks (50k+ corpus),
    multi-language, public docs site, conformance suite, lip-sync
    anti-spoof

Until v1.0 is cut, the public API surface and envelope wire format
may shift. Integrators tracking `main` accept that risk; the
deprecation cycle in `docs/deprecation-policy.md` only kicks in
from v1.0 onward.

When asked to make something: real code exists, real build commands
work. Don't scaffold from scratch — `git log` + the rollout plan +
the spec are the up-to-date sources. CI runs 11 jobs on every push
(shared ASan+UBSan, shared strict warnings, iOS SwiftPM, Android
AAR, RN + Capacitor lint+build, types parity, evidence-verifier,
reference-consumer, benchmark-harness selftest, docs presence). New
code is expected to keep CI green.

## Repository structure

The full layout is documented in
[docs/technical-specification.md §2](docs/technical-specification.md#2-repository-layout).
Summary:

```
sentlog/
├── LICENSE                # MIT
├── README.md
├── SECURITY.md
├── shared/                # C++17 core — narrow surface, heavily tested
├── android/               # AAR
├── Package.swift          # Swift package manifest
├── ios/                   # Swift binding
├── rn/                    # @sentlog/react-native bridge
├── capacitor/             # @sentlog/capacitor bridge
├── models/                # training + quantisation pipelines (not bundled in SDK)
├── tools/                 # benchmark harness, reference consumer, evidence verifier, samples
└── docs/
```

## Package boundaries

- **`shared/`** is C++17 only. Its public surface is a narrow C ABI in
  `shared/include/sentlog/sentlog.h` — around two dozen functions
  plus opaque handles. Nothing else crosses the boundary. Score assembly,
  fingerprint computation, scoring math, and ML kernel orchestration
  live here.
- **`android/` and `ios/`** are the thick bindings. Capture
  orchestration, OTA fetch, sensor handling, telemetry — all
  platform-native code. The goal is to keep the C++ surface small because all code is AI-written
  (see below).
- **`rn/` and `capacitor/`** wrap the platform bindings. Pure
  TypeScript surface; the bridges are thin native-to-JS shims that
  serialise types and forward calls.
- **`models/`** is a training/conversion workshop. Nothing in
  `models/` ships in the SDK binary — outputs go through the signed
  manifest (`models/manifest/`, which holds no manifests yet).
- **`tools/`** is integrator-facing tooling and samples. Independent
  of the SDK; treat as a separate concern.

## Stack

| Layer | Stack |
|---|---|
| Shared core | C++17, CMake, gtest |
| Android binding | Kotlin, JNI, Gradle, Macrobenchmark, ONNX Runtime |
| iOS binding | Swift, C interop, SPM, XCTest, ONNX Runtime |
| React Native bridge | TypeScript, Kotlin (NativeModule), Swift (`@objc`) |
| Capacitor bridge | TypeScript, Kotlin (`@CapacitorPlugin`), Swift (`CAP_PLUGIN`) |
| Evidence verifier | Python (single-file, stdlib + `cryptography`) |
| Models | whisper.cpp, AASIST-L, Silero VAD |
| Audio fingerprint | homegrown MIT `sentlog-pcp-v1` (Goertzel temporal-delta hash) — Chromaprint dropped over LGPL static-link burden |
| Prosody features | homegrown MIT `sentlog-prosody-v1` (F0 / energy / voicing-stat heuristic) — openSMILE dropped over GPL/commercial dual-licence incompatibility |
| Signing | Ed25519 (software), ECDSA-P256 (hardware-backed via Secure Enclave / KeyStore StrongBox) |
| Evidence format | W3C Verifiable Credentials 2.0 with `DataIntegrityProof` |
| Optional timestamping | RFC 3161 TSA |

## Commands

```bash
make setup            # list the toolchains the build needs
make build-shared     # build the C++ core (ASan + UBSan on)
make build-android    # build the AAR
make build-ios        # build the Swift package
make build-rn         # build the @sentlog/react-native package
make build-capacitor  # build the @sentlog/capacitor package
make test             # C++ core tests + bridge typecheck and types parity
make test-android     # Android unit tests
make test-ios         # iOS XCTest suite
make benchmark        # run the Android benchmarks (connected device needed)
make verify           # run the tools/evidence-verifier selftest
make lint             # typecheck both bridges + types parity
```

CI is defined in [.github/workflows/ci.yml](.github/workflows/ci.yml).

## Key design constraints

These come from the technical specification and are load-bearing.
Don't violate them without flagging.

1. **Inert by default.** No autonomous behaviour anywhere. No
   background tasks. No SDK-owned cloud. No defaults that do anything
   the integrator didn't ask for. `forwarders: []` is empty. OTA fetch
   is via explicit `checkForUpdates()`. The SDK runs when called,
   returns when done, and is silent otherwise.
2. **Narrow C ABI.** The C header in `shared/include/sentlog/` exposes
   around two dozen functions with opaque handles. No exposed structs.
   AI-coded FFI bindings are tractable only at this surface size.
3. **Raw [0,1] scores.** All score fields are unmodified probability
   outputs. No SDK-side calibration, no capping. `thresholds_met` is
   the only field that consumes the threshold values.
4. **Evidence, not verdicts.** The SDK's output is a signed envelope
   describing what was captured, not a verdict. The envelope conforms
   to W3C Verifiable Credentials 2.0; envelope wire format is stable
   across major envelope-format versions.
5. **No SDK-owned identity.** No user IDs, no sessions in the auth
   sense. `session_id` is opaque integrator pass-through metadata.
6. **No PII in telemetry.** The `TelemetryEvent` union is exhaustive
   (defined in `docs/technical-specification.md §3.2`). No audio, no
   transcript text, no video frames, no user identifier.
7. **Forwarders ship envelopes; integrators name destinations.** Sinks
   (HTTP, Webhook, LocalQueue, Callback) are primitives. The SDK has
   no opinion on backends. It writes LocalQueue itself; HTTP and webhook
   delivery, including retry, is the integrator's dispatcher.

## AI-coded engineering posture

All code is AI-written. This shapes several non-negotiables:

- **ASan + UBSan on every C++ PR.** MSan on nightly. No exceptions.
- **Golden-fixture coverage** (~50 audio/motion/face clips with
  expected score ranges) on every PR. Catches FFI bugs and platform
  drift no human is reading the code closely enough to spot.
- **Narrow C ABI** (constraint above) is partly to keep AI-coded FFI
  tractable.
- **≥72h soak** before each release. JNI ref leaks and Swift bridging
  lifetime bugs don't show up in unit tests.
- **Follow known-working project layouts** for C++/JNI/Swift-interop
  multi-platform SDKs rather than inventing one. The patterns in this
  space are well-trodden.
- **Always link types to where they're defined.** The full `SentlogResult`,
  `SentlogConfig`, `TelemetryEvent`, and forwarder types live in
  `docs/technical-specification.md §3` (API) and `§4` (forwarding).
  Don't paraphrase them in code comments — link.

## Behavioral guidelines

Rules for contributing to Sentlog, optimised for keeping AI-coded
changes small, accurate, and reviewable.

**Tradeoff:** these rules bias toward caution over speed. Trivial
tasks deserve trivial process — use judgment.

### 1. Slow down before writing code

The cheapest moment to catch a wrong direction is before any code
exists. Use it.

- If your interpretation of the task isn't the only valid one, say so
  and pick a path together — don't pick silently.
- If you're guessing at intent, ask. A two-sentence clarification
  beats a 200-line rewrite.
- If you see a simpler approach that fits the constraints, name it.
  Push back is welcome.
- Confusion is a signal, not a weakness. When something doesn't add
  up, stop and name what's missing.

### 2. Write less code

Speculative scaffolding compounds. Code that exists has to be read,
tested, kept consistent, and migrated forward. The bar isn't "could
this be useful one day" — it's "what's the smallest correct thing
right now."

- No features that weren't asked for.
- No abstractions until at least the second caller exists.
- No config toggles for behaviour that has one value today.
- No defensive checks for branches that genuinely can't execute.
- If a 200-line solution exists when a 50-line one would work, take
  the 50.

The reality check: would the harshest reviewer you've worked with
call this overcomplicated? If yes, simplify before they do.

### 3. Touch only what the change requires

Every line you change is a line someone has to review, regress-test,
and live with. Drive-by improvements blow up diffs and bury the
actual change.

- No tidying of adjacent code, comments, or formatting that isn't
  broken.
- No refactoring of working code while doing something else.
- Conform to the existing style even when it isn't your preferred
  style.
- Notice dead code, surface it in a comment — don't delete it without
  being asked.
- The only orphans you clean up are the ones your own change created.

The trace test: every modified line should be traceable to the
user's request. If it can't be, it shouldn't be in the diff.

### 4. Work toward verifiable goals

A task without an exit condition keeps drifting. Convert every
request into something you can prove is done.

- "Add validation" → write the test for the failure case, then make
  it pass.
- "Fix the bug" → write the reproduction first, then make it pass.
- "Refactor X" → record passing tests, refactor, confirm tests still
  pass.

For anything that takes more than one obvious step, lay out a
sequence with a verification per step:

```
1. <action>  →  verified by: <check>
2. <action>  →  verified by: <check>
3. <action>  →  verified by: <check>
```

Sharp success criteria let you make progress without checking in.
Fuzzy criteria force the user to evaluate every move.

---

**Signals these rules are working:** small focused diffs, fewer
wholesale rewrites, and clarifying questions land before the code
does — not after.

### 5. Respect document hierarchy

- The three docs in `docs/` are the source of truth. If something
  needs to change in *what the SDK does*, the docs change first or
  in the same PR.
- Decision rows in `docs/technical-specification.md §1` are
  load-bearing — don't contradict them in code.
- The rollout plan describes execution; code should not get ahead of
  its phase without coordination.

### 6. When in doubt (project-specific)

Read the docs first. They are deliberately self-contained.

If the docs don't answer the question, surface it — don't guess. The
project is small enough and early enough that the right answer is
usually to ask.
