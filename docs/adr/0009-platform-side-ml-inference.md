# ADR 0009 — Platform-side ML inference; C++ core orchestrates and scores

**Status:** Accepted
**Date:** 2026-05-23
**Supersedes (in part):** [ADR 0001 §3](0001-cpp-shared-core-with-c-abi.md)
implicitly assumed ML inference would live in the C++ shared core
alongside scoring. This ADR documents an explicit pivot: inference
runs on the platform side; the C++ core orchestrates and scores.

## Context

The Phase 1 plan calls for Silero VAD, AASIST-L liveness, and a
prosody probe (Phase 3), all running on ONNX Runtime. Two integration
shapes are possible:

1. **ORT inside the C++ shared core.** SentlogCore links against ORT
   directly. Inference happens inside the same translation units as
   scoring. One inference codepath across platforms.
2. **ORT inside the platform bindings.** Kotlin and Swift each load
   ORT (via the native platform packages), run inference, and push
   results across the C ABI into the C++ core, which orchestrates and
   scores. Two inference codepaths; one scoring codepath.

Approach (1) preserves the "same .o files everywhere → automatic
score parity" property of ADR 0001 most directly. But it has real
distribution problems:

- ONNX Runtime ships as platform packages (`onnxruntime-mobile` AAR
  for Android, `microsoft/onnxruntime-swift-package-manager` for iOS).
  Using them from a SHARED C++ core that has to compile both via
  Android NDK (CMake) and via SwiftPM means pulling the prebuilt
  binaries into both build systems and configuring linker paths per
  architecture (arm64-android, arm64-ios-device, arm64-ios-sim,
  x86_64-ios-sim, arm64-macos, x86_64-macos, plus any others). Each
  combination has its own gotchas (sysroot, mininum-deployment
  target, code signing, simulator vs device linkage).
- The ORT C++ API surface is non-trivial; using the C API directly is
  cleaner but more verbose.
- Static linking is preferred (smaller, no dynamic-loading surprises)
  but ORT static libs aren't always published per architecture.

Approach (2) eats those problems entirely: each platform binding
adopts ORT in the way its native ecosystem expects, and the C++ core
stays ORT-free. The cost is a split pipeline — inference on one side
of the FFI, scoring on the other.

## Decision

**ML inference runs in the platform bindings. The C++ shared core
orchestrates and scores.**

The integration shape:

- **Android:** Kotlin uses `com.microsoft.onnxruntime:onnxruntime-mobile`
  as a Gradle dependency. ONNX model files (Silero VAD, AASIST-L,
  prosody probe) ship as AAR assets.
- **iOS:** Swift uses `microsoft/onnxruntime-swift-package-manager` as
  a SwiftPM dep. ONNX model files ship as SwiftPM resources.
- **C++ shared core:** accepts per-frame and per-utterance inference
  results from the platform binding via new C ABI entries
  (`sentlog_push_vad_frame`, `sentlog_push_liveness_result`, etc.).
  Pipeline assembles results using pushed values when present; falls
  back to its built-in baseline (energy VAD, etc.) when not. Scoring
  arithmetic, fingerprint computation, envelope construction, and
  evidence assembly stay in C++.

## Why this still preserves score parity

Score parity was the central justification for the C++ shared core
in ADR 0001. The argument:

- The actual ORT inference call is a deterministic data pump — given
  the same model, the same input tensor, and the same ORT version,
  the output is bit-identical (modulo well-understood floating-point
  determinism caveats that affect both platforms identically).
- Both platforms ship the **same model file** (via the same OTA
  manifest) and the **same pinned ORT version** (locked in the
  Gradle / SwiftPM dependency declarations).
- The scoring math that combines inference outputs — thresholds,
  ensembles, evidence assembly, fingerprint — stays in C++ where
  the parity guarantee from ADR 0001 still applies.

So in practice the parity loss is bounded to the ORT inference call
itself, which is the same model in the same runtime on both
platforms. Drift would have to come from ORT itself producing
different outputs on different architectures — a possibility but a
small and bounded one. Diagnostic golden-fixture tests on each
platform catch it.

## Consequences

**Enabled:**

- ORT distribution per platform via the native package ecosystems
  (Maven Central on Android, SwiftPM on iOS). No vendored prebuilt
  binaries in the repo; no cross-architecture linker debugging.
- Each binding can use the platform-idiomatic ORT API surface
  (Kotlin's `OrtEnvironment`, Swift's `ORTEnv`).
- Smaller C++ binary (no ORT linked into SentlogCore).
- The C++ shared core still owns the parts that matter for parity:
  state, scoring, envelopes, fingerprinting.

**Constrained:**

- Two inference codepaths to maintain — one in Kotlin, one in Swift.
  Bounded: each is ~50–100 lines of model load + per-frame inference.
- Golden fixtures need to live on both platforms to verify parity.
- New C ABI entries (push_vad_frame, push_liveness_result) widen the
  ABI surface from ~11 functions toward ~15. Still narrow.

**Architectural follow-ups:**

- WebAssembly (Phase 9) follows the same pattern: ORT-WASM in the JS
  layer, results pushed across the same C ABI to a WASM-compiled C++
  core.
- If parity-drift ever materialises in production, the fallback is
  approach (1) — vendor ORT per platform and run inference in C++.
  That's a future option, not blocked by this decision.

## Open follow-up

- A cross-platform golden-fixture test that runs the same audio
  through Silero on both Android and iOS and asserts the speech
  probabilities match to within a small epsilon. Phase 8 deliverable
  alongside the per-attack-class benchmark publication.
