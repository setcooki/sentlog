# ADR 0008 — React Native and Capacitor JS bridges in v1.0

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.16](../technical-specification.md#1-architectural-decisions)

## Context

Earlier drafts treated JS bridges as post-v1.0 work. Once the first
integrator class — JS-driven hybrid stacks (Capacitor-wrapped PWAs,
React Native apps) — was characterised, that became untenable: the
SDK is *unusable* to those integrators without a bridge. Shipping a
"v1.0" they can't consume defeats the purpose.

## Decision

`@sentlog/react-native` and `@sentlog/capacitor` ship as part of v1.0.

Both are **thin wrappers** over the same native API surface (the
Kotlin and Swift bindings from ADR 0001). They are *not* a parallel
implementation — they are TypeScript surfaces over the existing
native bindings.

- **`@sentlog/react-native`:** Android: Kotlin `ReactContextBaseJavaModule`
  (TurboModule where supported). iOS: Swift class exposing `@objc`
  async methods. Promises map to JS Promises. Events
  (telemetry, `CallbackSink` forwarders) via `RCTEventEmitter`.
- **`@sentlog/capacitor`:** Android: Kotlin `@CapacitorPlugin`. iOS:
  Swift `CAP_PLUGIN`-annotated class. Each native method takes a
  `PluginCall`. Events via `notifyListeners`.

Shared TypeScript types live in the native SDK as the source of truth;
both bridges import a generated `.d.ts` so they cannot drift. Binary
fields (fingerprint bytes, signed envelope proof) are base64-encoded
across the bridge.

A types-only entry point is published (`@sentlog/types` or
per-bridge `/types` subpath) so monorepos with zero-runtime-dependency
policies on their shared/types workspace packages can import
`SentlogResult` / `Liveness` / `Engagement` without pulling native
code.

## Consequences

**Enabled:**

- JS-driven integrators can adopt Sentlog at v1.0, not at v1.x.
- The same `evaluate()` API shape, the same forwarders, the same
  envelope — across native, RN, and Capacitor.
- Capacitor PWAs running on Android and iOS get parity with native
  integrators (modulo the runtime they're embedded in).
- Reference sample apps in `tools/sample-capacitor/` and
  `tools/sample-react-native/` exercise the integration end-to-end.

**Constrained:**

- Phase 6 of the rollout plan is the two-week bridge build. Extends
  MVP by ~2 weeks versus a bridges-later schedule.
- Two npm packages to maintain, version, and publish alongside the
  native artefacts. Solved by lock-step versioning
  (`@sentlog/capacitor@1.2.0` ↔ native AAR 1.2.0).
- Browser-only / WebAssembly (`@sentlog/web`) remains post-MVP
  (Phase 9). Capacitor in plain-Chrome PWA mode without the native
  shell returns degraded `Capabilities` rather than crashing — see
  the integrator misuse / degraded-runtime handling in spec §5.9.
