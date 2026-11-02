# AASIST-L liveness model provenance

The `aasist_l.onnx` file in this directory is the SDK's bundled
liveness model. It is **not yet generated** — run the conversion
script described in
[`tools/aasist-conversion/README.md`](../../tools/aasist-conversion/README.md)
to produce it.

When generated, the file is also copied (by the conversion script)
into the platform asset directories:

- `android/sentlog/src/main/assets/aasist_l.onnx`
- `ios/Sources/Sentlog/Resources/aasist_l.onnx`

## Source

- **Upstream architecture + checkpoint:** [`clovaai/aasist`](https://github.com/clovaai/aasist)
- **Pinned commit:** see `AASIST_COMMIT` in
  [`tools/aasist-conversion/convert-aasist-l.py`](../../tools/aasist-conversion/convert-aasist-l.py).
- **Source file:** `models/weights/AASIST-L.pth` (~426 KB, ~85K params).
- **Licence:** MIT (NAVER Corp; see
  [ADR 0004](../../docs/adr/0004-aasist-l-liveness-from-week-2.md)
  for the licensing assessment and
  [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) for the
  attribution).

## ONNX export details

- **Target opset:** 17 (matches the ORT versions pinned on both
  platforms: `onnxruntime-android` 1.24.2 and
  `microsoft/onnxruntime-swift-package-manager` 1.24.2).
- **Inputs:** `audio` (float32, shape `[batch, samples]`, dynamic).
- **Outputs:** `embedding` (utterance embedding), `logits` (binary
  spoof / genuine classification).

## Liveness mapping

The SDK runner softmaxes the logits and emits:

- `score`          = genuine probability
- `replay_risk`    = spoof probability (AASIST-L doesn't distinguish
                     attack types; both fields carry the same value)
- `synthetic_risk` = spoof probability (same caveat)

If/when a separate replay vs synthetic detector ships, the runner
distributes the probability appropriately.

## Verification

The conversion script smoke-tests the exported ONNX with a single
zero-input inference via ORT to confirm the file is loadable. The
end-to-end SDK pipeline test on real audio runs in the Phase 1g
benchmark gate.
