# Sentlog — React Native sample

A copy-pasteable starting point for using
[`@sentlog/react-native`](../../rn/) inside an existing React Native
app. This directory does NOT ship a full RN project scaffold (the
generated boilerplate is multi-MB and ages quickly with each RN
release); copy `App.tsx` into a fresh `npx react-native init` app
and follow the integration steps below.

## Install

```bash
# Inside your RN app:
npm install @sentlog/react-native

# iOS only:
cd ios && pod install && cd ..
```

The Sentlog native SDK (Kotlin AAR + Swift SPM target) must also
be on your build's classpath. Until we publish to Maven Central /
SwiftPM mirror in a Phase 7 follow-up, integrators wire the
underlying SDK via a local file dep or a private repo. See the
`rn/android/build.gradle` and `rn/ios/SentlogRn.podspec` for the
exact coordinates each platform expects.

## Permissions

Per [`docs/permissions.md`](../../docs/permissions.md):

- **iOS Info.plist:** add `NSMicrophoneUsageDescription` (always)
  + `NSCameraUsageDescription` (if `useVideoAffect=true`) +
  `NSMotionUsageDescription` (recommended if `useMotionSensors=
  true`).
- **Android `AndroidManifest.xml`:** the SDK's library manifest
  already declares `RECORD_AUDIO` and `CAMERA`. The integrator's
  app is still responsible for requesting these via the runtime
  permission UX before calling `Sentlog.startCapture()`.

## Use

See `App.tsx` in this directory. The flow is:

1. `await Sentlog.initialize(config)`.
2. (Optional) `await Sentlog.setSigningKey(base64SecretKey,
   verificationMethod)` if you want signed envelopes.
3. (Optional) `await Sentlog.setEnvelopeDispatcher((name, kind,
   sinkJson, envelopeJson) => { /* deliver via your HTTP client */ })`
   if you've configured non-local-queue forwarders.
4. `await Sentlog.startCapture()` — make sure the user has
   accepted the microphone prompt first.
5. After a few seconds of speech: `const result = await
   Sentlog.evaluate()`.
6. Inspect `result` — fields are typed against
   [`SentlogResult`](../../shared/types/sentlog.ts).

## What this sample shows

- Initialize + startCapture + evaluate, with a happy-path render
  of the result.
- Subscribing to telemetry events (`onTelemetry`) for diagnostics.
- Drain-queue example for local-queue forwarders.
- Permission-denied recovery loop.

It does NOT show: video-affect UX (still in Phase 5.5), TSA
timestamping (still in Phase 4.5), OTA updates (Phase 7).
