# OTA model updates

The SDK ships with a baseline model bundle and supports out-of-band
updates via a signed JSON manifest. Updates are **explicit** — the
SDK never auto-updates in the background. Your app calls
`checkForUpdates()` when it judges the moment is right (app launch
on Wi-Fi, settings screen, etc.).

The wire format and verification rules are documented in
[../technical-specification.md §5.1](../technical-specification.md).
This page covers the integrator-facing flow.

## Default behaviour

`Sentlog.checkForUpdates()` fetches the published Sentlog manifest
URL over HTTPS and verifies the Ed25519 signature against two
rotatable pinned public keys (active + standby) baked into the
binding. The fetch uses:

- iOS: `URLSession.shared`
- Android: `java.net.HttpURLConnection`
- React Native / Capacitor: integrators do the fetch themselves with
  the JS-side `fetch()` and pass the body to the bytes-form overload
  (see below).

Guardrails on the fetch:

- HTTPS-only (rejected with `INVALID_CONFIG` otherwise)
- 10-second timeout
- 64 KB response cap (real manifests are <2 KB)
- non-2xx HTTP status is a fetch error, not a verification error

```swift
// iOS
do {
    let result = try await Sentlog.checkForUpdates()
    if result.updateFound {
        // result.newVersion is set — fetch and apply the bundle
        // through your own update flow.
    }
} catch {
    // fetch failed or verification failed; inspect `error`
}
```

```kotlin
// Android
try {
    val result = Sentlog.checkForUpdates()
    if (result.updateFound) {
        // ... same flow as iOS
    }
} catch (t: Throwable) {
    // ...
}
```

## Self-published manifest (custom URL + keys)

If you ship your own model improvements and want the SDK to consume
them in place of the published Sentlog manifest, override
`manifest_url` **and** `manifest_pubkeys` together in `SentlogConfig`.
Mixed-source configuration (custom URL with default keys, or vice
versa) is rejected at `initialize`:

```kotlin
val cfg = SentlogConfig(
    issuer = "did:example:integrator",
    manifestUrl = "https://updates.example.com/sentlog/manifest.json",
    manifestPubkeys = listOf(
        "<base64-of-active-pubkey-32-bytes>",
        "<base64-of-standby-pubkey-32-bytes>",
    ),
)
Sentlog.initialize(cfg, context)
val result = Sentlog.checkForUpdates()
```

Pubkeys are 32 raw bytes, base64-encoded (standard alphabet or URL
alphabet — either works). Rotate by deploying a new manifest signed
with the standby key, then in the next app release swap the slot
order in `manifestPubkeys`.

## Pre-fetched bytes overload

When you want full control of the fetch (custom HTTP client, request
caching, integration tests with a fake manifest), use the
`manifestJson` overload:

```swift
let bytes = try await myCustomFetch(...)
let json = String(data: bytes, encoding: .utf8)!
let result = try await Sentlog.checkForUpdates(manifestJson: json)
```

```kotlin
val json: String = myCustomFetch(...)
val result = Sentlog.checkForUpdates(json)
```

React Native + Capacitor bridges expose **only** this overload — the
JS-side `fetch()` is typically what apps want anyway, and avoids
crossing the native bridge twice for the same bytes.

## UpdateResult

```json
{
  "checked_at_ms":    1735689600000,
  "update_found":     true,
  "previous_version": "0.0.5",
  "new_version":      "0.0.6",
  "applied":          false,
  "error":            null
}
```

`applied` is **always false** today — the C++ core verifies the
manifest and returns the new version, but does not download or swap
the bundle. The integrator's app owns the actual bundle apply step
(your existing model-management code, OTA framework, etc.). The
bundle download itself is verified via `bundle_sha256` +
`bundle_size`, which the manifest carries; verify these before
swapping.

## What `update_found` means

`update_found` is `true` only when the signed `manifest_version` is
strictly newer than the loaded version, compared as
`MAJOR.MINOR.PATCH[-prerelease]`. An older, equal or unparseable version
reports `false`, so replaying an old signed manifest can't offer a
downgrade to the loaded version.

When it's `true`, the result also carries the verified `bundle_url`,
`bundle_sha256`, `bundle_size` and `issued_at_ms`. The SDK doesn't download
or apply bundles. If you do, check the hash and size before using the
bundle, and persist the highest applied version across launches.

## Verification failures

When the manifest signature doesn't verify (tampered, wrong key,
unknown key_id, schema mismatch), the SDK:

1. Returns an `UpdateResult` with `updateFound: false`, `applied:
   false`, `error: "<reason>"`. Reason is one of:
   - `invalid_json`
   - `missing_fields`
   - `unknown_key_id`
   - `bad_signature`
   - `unsupported_schema_version`
2. Throws `IllegalStateException` (Kotlin) / `SentlogError` (Swift)
   carrying the same reason in the message — so callers using
   try/catch see the failure typed.
3. Emits a `ManifestVerifyFailed` telemetry event with `reason` +
   `attempted_version` (the parsed version, or empty if parse
   failed).

There is no automatic retry. If the failure is a transient HTTP
issue, the integrator's app retries at the next opportunity.
