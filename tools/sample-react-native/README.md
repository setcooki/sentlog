# Sentlog — React Native sample

A copy-pasteable starting point for using
[`@sentlog/react-native`](../../rn/) inside an existing React Native
app. This directory does NOT ship a full RN project scaffold (the
generated boilerplate is multi-MB and ages quickly with each RN
release); copy `App.tsx` into a fresh `npx react-native init` app
and follow the integration steps below.

## Install

The package isn't published to npm yet, and iOS install isn't possible
yet. See [`rn/README.md`](../../rn/README.md) for the current state and
what Android needs.

## Permissions

Per [`docs/permissions.md`](../../docs/permissions.md):

- **iOS Info.plist:** add `NSMicrophoneUsageDescription` (always)
  + `NSCameraUsageDescription` (if `useVideoAffect=true`) +
  `NSMotionUsageDescription` (recommended if `useMotionSensors=
  true`).
- **Android `AndroidManifest.xml`:** the SDK's library manifest
  already declares `RECORD_AUDIO`. Apps using video affect declare
  `CAMERA` themselves. The integrator's app is still responsible for requesting these via the runtime
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
- Surfacing a permission-denied error.

It does NOT show: drain queue, video-affect UX, TSA timestamping, OTA
updates.
