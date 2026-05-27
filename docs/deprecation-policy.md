# Deprecation Policy

This is the contract for what's stable, what changes between
versions, and how we signal change. Applies from v1.0 onward.

## Versioning

Sentlog uses semantic versioning at the *SDK* level. The version
string is exposed via `Sentlog.version` (Kotlin / Swift) and
`SENTLOG_VERSION_STRING` (C ABI).

- **Major** (e.g. `1.0.0` → `2.0.0`) — breaking changes to any
  surface listed under "Stable surface" below.
- **Minor** (e.g. `1.0.0` → `1.1.0`) — additive changes only. New
  config fields with defaults that preserve existing behaviour; new
  optional API methods; new `TelemetryEvent` types; new sink kinds.
  Integrators built against the old version continue to compile and
  run unchanged.
- **Patch** (e.g. `1.0.0` → `1.0.1`) — bug fixes, doc updates,
  internal refactors. No API surface change.

The model bundle (whisper, AASIST-L, Silero VAD weights, etc.) is
versioned independently — see `model_versions.manifest` in the
result and the OTA update flow ([integration/ota.md](integration/ota.md)).
Bumping a bundled model version does not bump the SDK version.

## Stable surface

The following are covered by semver:

- The C ABI in `shared/include/sentlog/sentlog.h` — every function
  signature, every typedef, every enum value, every status code.
- The Kotlin public API on `me.cooki.sentlog.Sentlog` and the
  serialisable shapes (`SentlogConfig`, `SentlogResult`, `Forwarder`,
  `Permissions`, etc.).
- The Swift public API on `Sentlog` and its Codable models.
- The TypeScript surface of `@sentlog/react-native` and
  `@sentlog/capacitor` — both the bridge methods and the type
  re-exports from `shared/types/sentlog.ts`.
- The W3C VC 2.0 envelope wire format (`@context`, `type`,
  `credentialSubject.*`, `proof.*`). Envelope-format minor bumps
  are additive (new optional subject fields); major bumps require a
  major SDK version.
- The `TelemetryEvent` union discriminator values and the field set
  of each variant.
- The signed-manifest wire format (see
  [technical-specification.md §5.1](technical-specification.md)).
  `schema_version` is bumped when the format changes incompatibly;
  the SDK accepts a documented range.

## Not stable

These are explicitly *not* covered by semver and may change in any
release:

- Anything under `internal/` (Kotlin / Swift / TypeScript packages).
- The C++ headers under `shared/src/internal/`. The narrow C ABI is
  the contract; the C++ implementation isn't.
- File paths inside the integrator's app sandbox that the SDK reads
  or writes (e.g. local-queue file location, OTA bundle cache
  directory) — use the integrator-provided paths in the config; the
  SDK does not pick file locations on the integrator's behalf today.
- The exact wording of error / log messages.

## Deprecation cycle

When we deprecate something on the stable surface:

1. **Deprecate** in version `X.Y.0` — the symbol is marked
   `@Deprecated` (Kotlin) / `@available(*, deprecated)` (Swift) /
   `@deprecated` JSDoc (TypeScript). The deprecation message names
   the replacement. The behaviour is unchanged.
2. **Continue to ship** for at least one minor version after
   deprecation (so deprecated in `1.2.0` means still functional in
   `1.3.x`).
3. **Remove** in the next major version, at the earliest. Removal
   is called out in the release notes.

Deprecations affecting the C ABI: the symbol stays exported for the
full deprecation window so existing binaries don't break.
Deprecations affecting the envelope or manifest wire format: the
parser tolerates both forms during the deprecation window; the
writer switches to the new form when the consumer signals support.

## Out-of-policy: pre-v1.0

Pre-v1.0 phases (the current state of the repository) make no
stability guarantees. Surfaces change between phases as documented
in [rollout-plan.md](rollout-plan.md); breaking changes are flagged
in commit messages but not subject to the deprecation cycle above.
v1.0 is the first release the policy applies to.

## Reporting a stability concern

If you find a surface that should be stable but isn't documented
here — or a change that broke an integration where this policy says
it shouldn't have — open an issue. Stability issues are treated as
higher-priority than feature work.
