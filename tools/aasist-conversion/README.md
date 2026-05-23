# AASIST-L → ONNX conversion

One-shot script that converts the upstream `AASIST-L.pth` PyTorch
checkpoint into an ONNX file the SDK's platform bindings load at
runtime.

## When to run

- Initial bring-up of the AASIST-L liveness pipeline (Phase 1e3b).
- When the upstream `clovaai/aasist` commit pin in
  `convert-aasist-l.py` is bumped.
- When the ONNX opset version target is changed.

Otherwise the `aasist_l.onnx` artefact committed under
`models/liveness/` (and copied into the platform asset directories)
is the source of truth — no need to re-run.

## How to run

Requires Python 3.10+ and roughly 1.5 GB of free disk (the bulk is
PyTorch's wheel).

```bash
cd tools/aasist-conversion
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python convert-aasist-l.py
```

The script:

1. Clones `clovaai/aasist` at the pinned commit into a temp directory.
2. Loads `AASIST-L.pth` against the upstream architecture definition.
3. Traces and exports to ONNX with a dynamic-length audio input.
4. Smoke-tests the exported file via ORT (CPU provider).
5. Writes `aasist_l.onnx` to `models/liveness/`.
6. Copies into both platform asset directories
   (`android/sentlog/src/main/assets/`, `ios/Sources/Sentlog/Resources/`).

Output is `~340 KB` (≈ same order as the source `.pth`).

## What the SDK does with the file

- **Android:** `AasistLivenessRunner.kt` loads it from
  `AssetManager.open("aasist_l.onnx")` at startCapture, runs inference
  during evaluate, pushes the result via `sentlog_push_liveness_result`.
- **iOS:** `AasistLivenessRunner.swift` loads via
  `Bundle.module.url(forResource: "aasist_l", withExtension: "onnx")`,
  same lifecycle.

If the file is missing (e.g. checked out before the conversion was
run), the SDK falls back to the placeholder liveness result
(`model_versions.liveness = "placeholder-not-loaded"`). This is by
design — the build doesn't depend on the model being present.

## Provenance

The script pins the upstream `clovaai/aasist` commit; the conversion
process records that SHA in `models/liveness/SOURCE.md` so the
artefact's provenance is reproducible. See
[ADR 0004](../../docs/adr/0004-aasist-l-liveness-from-week-2.md)
for the upstream licensing assessment (MIT, both code and weights).
