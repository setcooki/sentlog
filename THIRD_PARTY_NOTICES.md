# Third-Party Notices

Sentlog ships under MIT (see [LICENSE](LICENSE)) and depends on
several open-source projects. This file lists the third-party
software and data Sentlog redistributes or builds on, with the
required attributions and licence texts.

When a third-party artefact is *bundled* in the SDK (e.g. a model
weight file shipped in the AAR / XCFramework), this file carries its
licence. When a third-party artefact is only a *build-time
dependency*, this file carries attribution.

---

## AASIST-L pretrained weights (bundled)

The Sentlog liveness module bundles the official AASIST-L pretrained
checkpoint from NAVER Corp's open-source release.

- **Source:** [`clovaai/aasist`](https://github.com/clovaai/aasist),
  file `models/weights/AASIST-L.pth` (~426 KB, 85K parameters).
- **Paper:** Jung, Heo, Tak, Shim, Chung, Lee, Yu, Evans.
  *AASIST: Audio Anti-Spoofing Using Integrated Spectro-Temporal
  Graph Attention Networks.* ICASSP 2022.
  [arXiv:2110.01200](https://arxiv.org/abs/2110.01200).
- **Pinned commit SHA:** *(recorded in `models/liveness/SOURCE.md`
  when the weights are integrated in Phase 1.)*
- **Licence:** MIT.

```
MIT License

Copyright (c) 2021-present NAVER Corp.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

**Note on `clovaai/aasist`'s `NOTICE` file:** that file flags the
min-tDCF metrics code (`evaluation.py`) as CC-BY-NC-SA 4.0
(non-commercial). Sentlog does **not** import that path —
only the architecture (`models/AASIST.py`) and the weight blob
(`models/weights/AASIST-L.pth`), both under MIT, cross into Sentlog.

---

## AASIST-L liveness model (bundled when conversion has run)

When the conversion in `tools/aasist-conversion/` has been run, the
SDK bundles the converted `aasist_l.onnx` (~340 KB) derived from the
upstream MIT-licensed `AASIST-L.pth` weights.

- **Source:** [`clovaai/aasist`](https://github.com/clovaai/aasist),
  file `models/weights/AASIST-L.pth` and `models/AASIST.py`.
- **Pinned commit:** see `AASIST_COMMIT` in
  [`tools/aasist-conversion/convert-aasist-l.py`](tools/aasist-conversion/convert-aasist-l.py).
- **Conversion:** PyTorch → ONNX (opset 17) via the upstream
  architecture definition; smoke-tested with ORT.
- **Licence:** MIT (NAVER Corp; see
  [ADR 0004](docs/adr/0004-aasist-l-liveness-from-week-2.md) for the
  whole-repo licence assessment).

Same MIT notice as the Silero VAD entry above; see
[`models/liveness/SOURCE.md`](models/liveness/SOURCE.md) for the full
provenance of the converted artefact.

---

## ASVspoof 2019 LA — Training data

The AASIST-L weights bundled above were trained on the ASVspoof 2019
LA (Logical Access) dataset.

- **Source:** [Edinburgh DataShare DOI 10.7488/ds/2555](https://datashare.ed.ac.uk/handle/10283/3336)
- **Citation:** Wang, Yamagishi, Todisco, Delgado, Nautsch, Evans,
  Sahidullah, Vestman, Kinnunen, Lee, Juvela, Alku, Peng, Hwang, Tsao,
  Wang, Le Maguer, Becker, Henderson, Clark, Zhang, Wang, Wang, Liu,
  Liu, Wang, Xie. *ASVspoof 2019: A large-scale public database of
  synthesized, converted and replayed speech.* Computer Speech &
  Language, vol. 64, 2020.
- **Licence:** Open Data Commons Attribution Licence (ODC-BY 1.0).

ODC-BY 1.0 permits commercial use, modification, and redistribution
of the database and any "Produced Works" (such as a trained model)
provided that the database is attributed in any public use. This
notice constitutes that attribution.

---

## Silero VAD ONNX model (bundled)

The Sentlog Android AAR bundles the Silero VAD v5.1 ONNX model
at `android/sentlog/src/main/assets/silero_vad.onnx` (~2.3 MB).

- **Source:** [`snakers4/silero-vad`](https://github.com/snakers4/silero-vad),
  file `src/silero_vad/data/silero_vad.onnx` at tag `v5.1`.
- **Paper / model card:** Silero Team — *Silero VAD: pre-trained
  enterprise-grade voice activity detector*. See the upstream repo
  README for benchmark details.
- **Pinned tag:** v5.1.
- **Licence:** MIT.

```
MIT License

Copyright (c) 2024-present Silero Team

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The iOS SwiftPM package bundles the same model file when Silero is
wired up in a follow-up chunk.

---

## Other upstream dependencies

The following open-source projects are used at build or runtime by
Sentlog. Their licences are reproduced in the bundled artefacts where
required by the respective licence; this file carries attribution.

| Project | Use | Licence |
|---|---|---|
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | On-device ASR | MIT |
| [ggml](https://github.com/ggerganov/ggml) | ML kernels backing whisper.cpp | MIT |
| [Silero VAD](https://github.com/snakers4/silero-vad) | Voice activity detection | MIT |
| [tweetnacl](https://tweetnacl.cr.yp.to/) | Ed25519 signing for the VC 2.0 envelope's DataIntegrityProof (Phase 4b). Vendored at `shared/external/tweetnacl/`; see `shared/external/tweetnacl/VERSION.md`. | Public domain |
| [MediaPipe Face Mesh](https://github.com/google/mediapipe) | Face landmark detection (Android, affect mode) | Apache-2.0 |
| Apple Vision framework | Face landmark detection (iOS, affect mode) | Apple system framework |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing and serialisation in the C++ core | MIT |
| [GoogleTest](https://github.com/google/googletest) | Test framework (build-time only; not in shipping artefacts) | BSD-3-Clause |
| [ONNX Runtime Mobile](https://github.com/microsoft/onnxruntime) | On-device ONNX inference (Silero VAD; AASIST-L in a follow-up chunk) | MIT |

When the SDK build system integrates each of these in the phases
that own them, the bundled binaries carry the respective licence
texts and attribution notices as required.

## Components considered and replaced

Two components from earlier iterations of the rollout plan were
dropped in favour of homegrown MIT implementations to keep the
SDK's single-binary distribution story clean:

- **Chromaprint (LGPL-2.1+)** — perceptual audio fingerprint.
  Static-linking LGPL into an MIT SDK puts a relinking obligation
  on every integrator. Replaced by `sentlog-pcp-v1` (a Goertzel
  temporal-delta hash; `shared/src/internal/fingerprint.{h,cpp}`).
- **openSMILE (GPL or commercial)** — eGeMAPS prosody features.
  Dual GPL / commercial licence is fundamentally incompatible with
  MIT distribution. Replaced by `sentlog-prosody-v1` (autocorrelation
  F0 + RMS/ZCR statistics, inspired by the eGeMAPS spec;
  `shared/src/internal/prosody.{h,cpp}`). The eGeMAPS feature *set*
  is a published academic spec, so implementing it ourselves is
  unencumbered; the openSMILE *implementation* is what's GPL.

---

## How to report a missing attribution

If you spot a third-party component used by Sentlog that should be
attributed here and isn't, please open an issue or email
`set@cooki.me` with the subject `[sentlog-attribution]`.
