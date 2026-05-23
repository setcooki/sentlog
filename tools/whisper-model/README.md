# whisper.cpp model download

The Sentlog Android binding loads `whisper-tiny.bin` from app assets
at startCapture and runs ASR over the captured audio before
`evaluate()` returns. The model file is **not** committed — it's
~30 MB and we don't want to inflate the repo, so each platform binding
expects an operator to drop it in before building a release.

The runtime gracefully no-ops when the asset is absent — the C++
pipeline then emits the placeholder transcript with
`model_versions.asr = "placeholder-not-loaded"`. The build never
depends on the model being present.

## When to run

- Initial bring-up of the Phase 2 ASR pipeline.
- When the upstream whisper.cpp pin in
  [`android/sentlog/src/main/cpp/CMakeLists.txt`](../../android/sentlog/src/main/cpp/CMakeLists.txt)
  is bumped and the new release publishes incompatible model files.

## How to run

```bash
cd tools/whisper-model
./download-tiny.sh
```

The script:

1. Downloads `ggml-tiny.bin` from Hugging Face
   (`ggerganov/whisper.cpp` repo, the upstream-recommended quantised
   tiny model — Q5_1 in name, but the official `ggml-tiny.bin` is the
   FP16 reference; we use it because the variable-context (ACFT) tail
   per Phase 2 of the rollout plan is more important than the storage
   savings at this size).
2. Validates the SHA-256 against the value pinned in the script.
3. Copies into both platform asset directories
   (`android/sentlog/src/main/assets/whisper-tiny.bin`,
   `ios/Sources/Sentlog/Resources/whisper-tiny.bin`).

The bytes are identical across platforms — whisper.cpp's ggml format
is endian/architecture-neutral.

## What the SDK does with the file

- **Android:** [`WhisperRunner.kt`](../../android/sentlog/src/main/kotlin/me/cooki/sentlog/internal/WhisperRunner.kt)
  copies the asset to the app cache directory at first use
  (whisper.cpp's loader needs a real filesystem path because it
  `mmap`s the file) and feeds the captured audio into
  `whisper_full`. The result is JSON-shaped for
  `sentlog_push_transcript`.
- **iOS:** lands in chunk 2c.

## Why not bundle it

A 30 MB binary in the AAR adds startup cost (extract on first app
launch) and balloons cold-start CI. Operators bundle it at the same
point they'd bundle the AASIST-L model — typically as part of a
release build pipeline, not the dev loop.

If you ever want a single-file SDK that ships with whisper, replace
the asset-load step in `WhisperRunner.kt` with a network fetch
backed by the SDK's existing OTA infrastructure (see Phase 7 in
[`docs/rollout-plan.md`](../../docs/rollout-plan.md)).
