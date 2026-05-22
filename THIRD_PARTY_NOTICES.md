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

## Other upstream dependencies

The following open-source projects are used at build or runtime by
Sentlog. Their licences are reproduced in the bundled artefacts where
required by the respective licence; this file carries attribution.

| Project | Use | Licence |
|---|---|---|
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | On-device ASR | MIT |
| [ggml](https://github.com/ggerganov/ggml) | ML kernels backing whisper.cpp | MIT |
| [Silero VAD](https://github.com/snakers4/silero-vad) | Voice activity detection | MIT |
| [openSMILE](https://github.com/audeering/opensmile) | eGeMAPS prosody features | BSD-style (audEERING) |
| [Chromaprint](https://github.com/acoustid/chromaprint) | Perceptual audio fingerprint | LGPL-2.1+ |
| [MediaPipe Face Mesh](https://github.com/google/mediapipe) | Face landmark detection (Android, affect mode) | Apache-2.0 |
| Apple Vision framework | Face landmark detection (iOS, affect mode) | Apple system framework |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing and serialisation in the C++ core | MIT |
| [GoogleTest](https://github.com/google/googletest) | Test framework (build-time only; not in shipping artefacts) | BSD-3-Clause |

When the SDK build system integrates each of these in the phases
that own them, the bundled binaries carry the respective licence
texts and attribution notices as required.

---

## How to report a missing attribution

If you spot a third-party component used by Sentlog that should be
attributed here and isn't, please open an issue or email
`set@cooki.me` with the subject `[sentlog-attribution]`.
