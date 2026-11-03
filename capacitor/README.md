# @sentlog/capacitor

Capacitor plugin for the Sentlog SDK. Pre-v1.0: the API may still change.

## Install status

- **npm:** not published yet (version `0.0.0`). Use a local checkout.
- **Android:** needs the `me.cooki.sentlog:sentlog` AAR on your app's
  classpath ([`android/build.gradle`](android/build.gradle) asks for version `0.0.0`).
  It isn't on Maven Central yet, so build it with
  `gradle -p android :sentlog:assembleDebug` and serve it from a local
  Maven repo.
- **iOS:** install isn't possible yet. [`SentlogCapacitor.podspec`](ios/SentlogCapacitor.podspec) depends on a
  `Sentlog` pod, and that pod doesn't exist.

## What works

The TypeScript surface builds and type-checks. The Android code is
compile-checked against the AAR by
[`tools/bridge-compile/`](../tools/bridge-compile/) (see `run.sh`). The
iOS code exists but can't be installed, for the reason above.

The bridge has no networking of its own. For `http` and `webhook` sinks,
deliver envelopes from your `setEnvelopeDispatcher` handler.

## API

See [`src/types.ts`](src/types.ts): the `SentlogBridge` interface and all
types. That file is generated from `shared/types/sentlog.ts`; edit the
source and run `tools/sync-types.sh`.

JS exposes only `checkForUpdates(manifestJson)`, so fetch the manifest
yourself. `setHardwareSigner`, `setAudioEncoder`, `setTsaProvider` and the
bundled native helpers aren't bridged.

Sample snippets: [`../tools/sample-capacitor/`](../tools/sample-capacitor/).
