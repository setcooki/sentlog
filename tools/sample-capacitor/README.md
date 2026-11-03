# Sentlog — Capacitor sample

A copy-pasteable starting point for using
[`@sentlog/capacitor`](../../capacitor/) inside an existing
Ionic / Svelte / vanilla-TS Capacitor app. This directory does not
ship a full Capacitor project scaffold (different framework
preferences, generated boilerplate ages quickly); copy the example
snippets into your own page.

## Install

The package isn't published to npm yet, and iOS install isn't possible
yet. See [`capacitor/README.md`](../../capacitor/README.md) for the
current state and what Android needs.

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

It does NOT show: video-affect UX, TSA timestamping, OTA updates.
