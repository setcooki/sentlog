# Sentlog — Capacitor sample

A copy-pasteable starting point for using
[`@sentlog/capacitor`](../../capacitor/) inside an existing
Ionic / Svelte / vanilla-TS Capacitor app. This directory does not
ship a full Capacitor project scaffold (different framework
preferences, generated boilerplate ages quickly); copy the example
snippets into your own page.

## Install

```bash
# Inside your Capacitor app:
npm install @sentlog/capacitor
npx cap sync
```

The Sentlog native SDK (Kotlin AAR + Swift SPM target) must also be
on your build's classpath. Until we publish to Maven Central /
SwiftPM mirror in a Phase 7 follow-up, integrators wire the
underlying SDK via a local file dep or a private repo. See
`capacitor/android/build.gradle` and
`capacitor/ios/SentlogCapacitor.podspec` for the dependency
coordinates each platform expects.

## Permissions

Per [`docs/permissions.md`](../../docs/permissions.md):

- **iOS Info.plist:** `NSMicrophoneUsageDescription` (always)
  + `NSCameraUsageDescription` (if `useVideoAffect=true`) +
  `NSMotionUsageDescription` (recommended if `useMotionSensors=
  true`).
- **Android `AndroidManifest.xml`:** the plugin's library manifest
  declares `RECORD_AUDIO`. Apps using video affect declare `CAMERA`
  themselves. The integrator's app is still responsible for the runtime
  permission UX.

## Use

See `page.ts` in this directory. The flow mirrors the React Native
sample: `initialize` → optional `setSigningKey` /
`setEnvelopeDispatcher` → `startCapture` → `evaluate`.

## What this sample shows

- Initialize + startCapture + evaluate, with a happy-path result
  print.
- Subscribing to telemetry events (`onTelemetry`).
- Drain-queue example for local-queue forwarders.
- Permission-denied error reporting.

It does NOT show: video-affect UX (Phase 5.5), TSA timestamping
(Phase 4.5), OTA updates (Phase 7).
