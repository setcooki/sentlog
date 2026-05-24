# Vendored whisper.cpp

| Field | Value |
|---|---|
| Upstream | https://github.com/ggerganov/whisper.cpp |
| Tag | v1.7.1 |
| Commit SHA | ebca09a3d1033417b0c630bbbe607b0f185b1488 |
| Source license | MIT (see `LICENSE`) |
| Vendored on | 2026-05-24 |

## Why vendored

The upstream `Package.swift` at v1.7.1 ships a `.process("ggml-metal.metal")`
declaration at the package root, but the file actually lives at
`ggml/src/ggml-metal.metal`. The mismatch makes Xcode 15 fail with
`undeclared identifier 'SWIFTPM_MODULE_BUNDLE'` when the Metal
backend tries to look up its shader bundle. Upstream removed
`Package.swift` entirely in v1.7.5+ (switched to systemLibrary mode,
which requires a brew/pkg-config install), so there's no upstream
tag to point at that works with SwiftPM as-is.

Vendoring lets us write a corrected `Package.swift` while pinning to
the same v1.7.1 source the Android `FetchContent` declaration in
[`android/sentlog/src/main/cpp/CMakeLists.txt`](../../../android/sentlog/src/main/cpp/CMakeLists.txt)
points at, so both platforms run identical bytes per ADR 0009.

## What's vendored

Only the C/C++/Metal files SwiftPM actually compiles. Other parts
of upstream — the Python bindings, examples, samples, model
download scripts, CUDA/SYCL/Vulkan backends — are skipped. The
`bindings/`, `examples/`, `models/`, `scripts/` etc. dirs from
upstream are *not* present in this vendor.

## Bumping the pin

1. Download the new tag tarball:

   ```bash
   curl -L https://github.com/ggerganov/whisper.cpp/archive/refs/tags/<tag>.tar.gz | tar xz
   ```

2. Replace the contents under this directory with the same file set
   (see `Package.swift`'s `sources:` / `publicHeadersPath:` / private
   headers under `ggml/src/`). Keep this `Package.swift` and `VERSION.md`.

3. Update the Android `FetchContent` pin in
   [`android/sentlog/src/main/cpp/CMakeLists.txt`](../../../android/sentlog/src/main/cpp/CMakeLists.txt)
   to the matching tag so the two platforms stay in lockstep.

4. Update this file's Tag / Commit SHA / Vendored on fields.
