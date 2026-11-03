# @sentlog/react-native

React Native bridge for the Sentlog SDK. Pre-v1.0: the API may still change.

## Install status

- **npm:** not published yet (version `0.0.0`). Use a local checkout.
- **Android:** needs the `me.cooki.sentlog:sentlog` AAR on your app's
  classpath ([`android/build.gradle`](android/build.gradle) asks for version `0.0.0`).
  It isn't on Maven Central yet, so build it with
  `gradle -p android :sentlog:assembleDebug` and serve it from a local
  Maven repo.
- **iOS:** needs React Native 0.75+ and an iOS 16 deployment target
  (`platform :ios, '16.0'` in your Podfile). Use the default static linking:
  on React Native 0.85, `USE_FRAMEWORKS=dynamic` fails with `no such module
  'React'`. You can ignore the static-linking warning from `pod install`.
  [`SentlogRn.podspec`](SentlogRn.podspec) pulls
  the Swift package from this repo's `main` branch through React Native's
  `spm_dependency`, because there's no `Sentlog` pod and no tagged release yet.

## What works

The TypeScript surface builds and type-checks. The Android code is
compile-checked against the AAR by
[`tools/bridge-compile/`](../tools/bridge-compile/) (see `run.sh`). CI
doesn't build the iOS bridge.

The bridge has no networking of its own. For `http` and `webhook` sinks,
deliver envelopes from your `setEnvelopeDispatcher` handler.

## API

See [`src/types.ts`](src/types.ts): the `SentlogBridge` interface and all
types. That file is generated from `shared/types/sentlog.ts`; edit the
source and run `tools/sync-types.sh`.

JS exposes only `checkForUpdates(manifestJson)`, so fetch the manifest
yourself. `setHardwareSigner`, `setAudioEncoder`, `setTsaProvider` and the
bundled native helpers aren't bridged.

Sample snippets: [`../tools/sample-react-native/`](../tools/sample-react-native/).
