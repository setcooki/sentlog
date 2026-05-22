# ADR 0001 — C++17 shared core with hand-written C ABI

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.1](../technical-specification.md#1-architectural-decisions)

## Context

Sentlog is a universal SDK across iOS, Android, React Native, and
Capacitor. The output is structured presence and evidence — and the
non-negotiable property is **score parity across every runtime**: the
same clip on the same model must produce the same scores on every
platform. If Android scores a clip at 0.72 and iOS scores it at 0.68,
the SDK's value proposition collapses.

Four shared-core options were considered:

1. **C++17 with a hand-written C ABI, wrapped by thin Kotlin (JNI) and
   Swift (C-interop) layers.** Industry consensus pattern.
2. **Kotlin Multiplatform (Kotlin/Native on iOS).** Ergonomic in Kotlin
   but does not remove the C++ layer (whisper.cpp / ggml are C++); ends
   up with both Kotlin and C++ in the shared layer.
3. **Rust + UniFFI.** Memory-safe, good FFI tooling, but wrapping
   whisper.cpp requires a C wrapper anyway — pays C++ toolchain cost
   plus a Rust toolchain cost.
4. **Two parallel native implementations.** Fastest to ship, but score
   parity is non-trivial to maintain over time and every interface
   change has to be done twice.

Empirical evidence from comparable on-device voice/ML SDKs is
unambiguous: Picovoice (Porcupine, Cobra, Rhino) uses an ANSI C core,
MediaPipe uses C++17, ONNX Runtime Mobile uses C++, Vosk uses C++,
DeepSpeech/Coqui STT uses C++, whisper.cpp itself uses C++. No
commercial on-device voice/ML SDK ships KMP as its compute core. Rust
+ UniFFI is proven in production for non-ML domains (Mozilla
Application Services, Matrix Rust SDK, Bitwarden) but has zero
on-device ML deployments.

## Decision

The shared core is **C++17** with a **hand-written C ABI** at
`shared/include/sentlog/sentlog.h`. Platform bindings are:

- **Android:** Kotlin + JNI wrapping the C ABI.
- **iOS:** Swift + C-interop (via `SentlogC` shim) wrapping the C ABI.
- **React Native and Capacitor:** native bridges layered on the
  Android and iOS bindings.

The C ABI is kept narrow — 8–12 functions plus opaque handles, no
exposed structs. Data crosses the boundary as JSON. This is partly to
keep AI-coded FFI tractable.

## Consequences

**Enabled:**

- Score parity is automatic: every platform invokes literally the same
  object code through the same C entry points.
- whisper.cpp, ggml, openSMILE, and AASIST-L (all C/C++) integrate
  without language hops.
- The same C++ core compiles to WebAssembly for a future
  `@sentlog/web` (Phase 9).
- Reference patterns from Picovoice / MediaPipe / ONNX Runtime Mobile
  can be copied directly.

**Constrained:**

- Build system is CMake + Gradle externalNativeBuild + Xcode
  integration — finicky but well-trodden.
- C++ memory safety is the team's problem. Mitigated by the narrow C
  ABI, AddressSanitizer + UndefinedBehaviorSanitizer on every PR,
  MemorySanitizer on nightly, ≥72 hour soak before release.
- AI-coded JNI / Swift bridging needs heavy test coverage (golden
  fixtures) and soak to catch lifetime bugs.

## Alternatives revisited

These remain viable for narrow future use cases, but the cost of
switching the shared core away from C++ outweighs any benefit:

- **Rust + UniFFI:** could be reconsidered if a future ML kernel we
  depend on becomes Rust-native and the C wrapping cost flips.
- **Kotlin Multiplatform:** could be useful for *non-compute* code
  (e.g. integration helpers, sample apps) but never the compute core.
