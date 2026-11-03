# Permissions checklist

Sentlog never requests runtime permissions itself. The integrator's
app is responsible for the prompt UX. The SDK declares the necessary
permissions in its manifest / privacy manifest so that the
consuming-app's build system merges them in correctly; if a required
permission is denied at runtime, `Sentlog.startCapture()` throws
(Kotlin: `SecurityException`, checked when a `context` was passed to
`initialize` / Swift: `SentlogError(status: SENTLOG_ERR_
PERMISSION_DENIED)`).

| Capability | iOS | Android | When required |
|---|---|---|---|
| Voice pipeline | `NSMicrophoneUsageDescription` in `Info.plist` | `RECORD_AUDIO` runtime permission | Always |
| Motion coherence ([`presence.motion`](technical-specification.md)) | `NSMotionUsageDescription` (recommended, OS logs the access regardless) | none — CoreSensorManager is permission-free at the sample rates we use (≤200 Hz) | `SentlogConfig.useMotionSensors == true` |
| Video affect ([`affect`](technical-specification.md)) | `NSCameraUsageDescription` in `Info.plist` | `CAMERA` declared in your manifest (the SDK does not declare it) plus the runtime permission | `SentlogConfig.useVideoAffect == true` |
| Hardware-backed signing | none (Secure Enclave is non-prompting) | none (Android KeyStore StrongBox is non-prompting) | `SentlogConfig.useHardwareSigning == true` (Phase 7) |

## What does NOT need a permission

- Local-queue forwarders writing to app-private storage.
- HTTP / Webhook forwarders (network calls; no permission gate on
  modern iOS / Android).
- OTA model update checks (network).
- Reading the captured audio buffer for liveness / ASR / prosody —
  these are derived from the microphone-recorded data and are
  covered by the microphone permission.

## What about face frames?

When `useVideoAffect == true`, your app captures camera frames,
runs landmark detection and the affect head on-device (see
[integration/face-affect.md](integration/face-affect.md)), and pushes
only the derived `(valence, arousal, FAU)` values into the C++ core. The
frames never reach the envelope under any projection — spec §5.2 is
enforced structurally because the C ABI `sentlog_push_face_frame`
only accepts the derived values, not the raw landmarks.

The recommended UX is to request camera permission **only when the
integrator's app actually enables `useVideoAffect`**, not at SDK
initialization. A user denying camera while the SDK is on the
voice-only path doesn't degrade anything.

## Permission-denied flow

```kotlin
try {
    Sentlog.startCapture()
} catch (e: SecurityException) {
    // request the microphone runtime permission, then retry
}
```

```swift
do {
    try Sentlog.startCapture()
} catch let e as SentlogError where e.status == SENTLOG_ERR_PERMISSION_DENIED {
    // request the relevant runtime permission, then retry
}
```

`startCapture` checks only the microphone permission. Integrators that
need finer-grained UX can pre-check with `getPermissions()` or the
platform's permission APIs before calling it.
