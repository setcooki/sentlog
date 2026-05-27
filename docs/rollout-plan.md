# Sentlog — Rollout Plan

**16-week MVP to v1.0 internal release.**

**Licence:** MIT.
**Audience:** Engineering execution — the team building Sentlog.
Tracks phases, gates, risks, and the open items that need closing
before or during the rollout.

Companion documents:
- [Whitepaper](./whitepaper.md) — vision, principles, positioning.
- [Technical Specification](./technical-specification.md) — architectural decisions, API, contracts.

---

## 1. Phased Rollout

### Phase 0 — Foundations (Week 0)

- Architectural decisions ratified as ADRs in `docs/adr/`.
- Repo structure created. CI green with ASan + UBSan jobs.
- C ABI header drafted at `shared/include/sentlog/sentlog.h`. Generated
  Kotlin and Swift signatures reviewed by one engineer per integrator.
- **AASIST-L pretrained-weight licence verified.** Resolved GREEN —
  `clovaai/aasist` repo and its in-tree `AASIST-L.pth` weight file
  are MIT-licensed; ASVspoof 2019-LA training data is ODC-BY 1.0.
  Attributions go in `THIRD_PARTY_NOTICES.md`. See ADR 0004.
- LICENSE file (MIT) and SECURITY.md committed to repo root.
- Labelling budget for the corpus confirmed by product.

Exit: empty SDK builds and publishes a snapshot artefact from CI.

### Phase 1 — Liveness path + Android benchmark gate (Weeks 1–2)

Sub-chunked for shippable increments:

- **1a**: shared C++ core skeleton (ring buffer, state machine, config
  validation, C ABI implementation, gtest harness).
- **1b**: Android binding — Kotlin public API, JNI bridge, externalNativeBuild
  pulling in the shared core, AudioRecord capture at 16 kHz mono PCM_FLOAT.
- **1c**: iOS binding — Swift package depending on shared (local SwiftPM
  dep), AVAudioEngine capture with format conversion, XCTest coverage.
- **1d**: VAD interface + reference energy-based implementation,
  evaluation pipeline orchestrator that drains the ring buffer,
  populated diagnostics (duration, SNR, audio_quality), schema-complete
  `SentlogResult` with placeholder ML scores. **Silero ONNX upgrade
  deferred — shares the ORT integration with chunk 1e (AASIST-L).**
- **1e**: ONNX Runtime integration — Silero VAD swap-in, AASIST-L
  liveness, model-file distribution per platform. Further split into:
  - **1e1**: architecture (ADR 0009 — platform-side inference, C++
    receives pushed results), Android Silero wired in.
  - **1e2**: iOS Silero wired in.
  - **1e3a**: liveness push C ABI + pipeline integration (`sentlog_push_liveness_result`,
    pipeline uses pushed liveness when present, otherwise placeholder).
  - **1e3b**: AASIST-L architecture + conversion script (architecture
    + Python conversion script + Kotlin/Swift `AasistLivenessRunner`
    classes + `sentlog_snapshot_audio` C ABI + wiring through both
    `Sentlog.evaluate()` flows). The platform runners gracefully no-op
    when the bundled `.onnx` is missing, so the build doesn't depend
    on the model file being present. Final piece: someone runs
    `tools/aasist-conversion/convert-aasist-l.py` to generate
    `aasist_l.onnx` and commit it.
- **1f**: Audio input route detection wired through the C ABI.
- **1g**: **Benchmark gate (§2) executed.** Harness:
  `android/benchmarks/` module (androidx.benchmark.junit4) with
  `SentlogPipelineBenchmark` running the warm/cold/peak-memory tests
  over a synthetic 15 s workload per §2.2. Threshold checker:
  `tools/benchmark/check-thresholds.py` reads the JSON output and
  emits per-device verdicts. Operator runbook:
  `docs/benchmarks/README.md`. Results recorded in
  `docs/benchmarks/wk2.md`.

Exit: `Sentlog.evaluate()` returns a populated `SentlogResult` with
real `liveness`, real `presence.audio_source`, and the rest of the
fields either real or marked as placeholder via
`engagement.model_status`. Benchmark gate is **pass** or formal
architecture review triggered.

### Phase 2 — Transcription + transcript exposure + direction (Weeks 3–4)

- whisper.cpp tiny Q5_1 with variable-length (ACFT-style) encoder
  context — the largest single performance lever for mid-range
  Android.
- whisper.cpp ships CPU-only on both platforms in this phase. iOS
  Core ML acceleration (whisper.cpp's encoder-as-CoreML path) and
  Android Vulkan/GPU acceleration are tracked follow-ups that land
  after the Phase 2 benchmark gate confirms the CPU baseline meets
  thresholds. Android still uses LiteRT for prosody/affect heads in
  later phases — that's unrelated to ASR.
- `Transcript` populated in `SentlogResult` (text, language,
  confidence, word_timings). Direction scoring uses the same transcript.
- Language gating: non-English clips return reduced confidence with a
  diagnostic.

Exit: `SentlogResult.transcript.text` populated reliably; direction
scores correctly against `target_phrase`.

### Phase 3 — Multi-modal trust + prosody features (Weeks 5–6)

- Motion-coherence module: accelerometer + gyroscope buffering, tremor
  + speech-coupled-motion analysis, emits `presence.motion`.
- Prosody features: `sentlog-prosody-v1` (homegrown MIT C++,
  inspired by the eGeMAPS feature set) — openSMILE was the original
  call but its GPL / commercial dual-licence is incompatible with
  the SDK's MIT distribution, same fork-in-the-road we took for
  Chromaprint. Plus whisper-encoder hidden-state probe (deferred to
  a follow-up; the prosody-only path is what Phase 3d ships).
- `engagement` returns ensemble of voice signals; surfaced as
  `engagement.voice_score` with overall `engagement.score`;
  `engagement_model_status: "training"`.
- Perceptual audio fingerprint (`sentlog-pcp-v1` — homegrown MIT
  Goertzel-based delta hash; Chromaprint was dropped over LGPL
  static-link burden), always computed.
- **Configurable scoring weights** wired through composite-score
  assembly. Named-key validation at `init()` (unknown keys rejected).
  Defaults documented in `docs/scoring-weights.md`.

Exit: voice-side `SentlogResult` fully populated (transcript + scores
+ presence + fingerprint). Custom-weights override exercised by an
integration test.

### Phase 4 — Composable forwarding + VC evidence envelope (Weeks 7–8)

- Four sink types: `HttpSink`, `WebhookSink`, `LocalQueueSink`,
  `CallbackSink`.
- Seven projections: `Full`, `Full+AudioClip`, `ScoresOnly`,
  `TranscriptOnly`, `EvidenceFingerprint`, `EvidenceClip`, `Custom`.
- Retry/backoff per forwarder; sampling per forwarder.
- Opus encoding (32 kbps mono) for clip projections, on-demand only.
- **VC 2.0 envelope construction** per the spec. Envelope built with
  `@context`, `type`, `issuer`, `credentialSubject`, `proof` populated
  for every forwarder.
- Software Ed25519 `DataIntegrityProof` (`eddsa-jcs-2022`) for
  `HttpSink`; HMAC-SHA256 for `WebhookSink` with configurable header
  name and signature format. Hardware-backed proof variants land in
  Phase 7.
- Optional **RFC 3161 TSA hook**: when `tsa_endpoint` is set, the SDK
  obtains a timestamp over the envelope hash and embeds it as
  `tsa_timestamp`. `tsa_required=true` makes TSA failure abort
  `evaluate()`; default `false` ships without TSA on failure and emits
  a telemetry event.
- `Sentlog.drainQueue()` helper for `LocalQueueSink` consumers.
- `SentlogResult.extensions` carried through every projection that
  includes a `Full` view.

Exit: all four sink types round-trip through the reference consumer
at `tools/reference-consumer/`. End-to-end forwarder integration test
on both platforms. Envelope verifies cleanly against a generic
VC-2.0-aware verifier.

**Deferred to Phase 4.5 (post-Phase 5)**:

- **4g: Opus encoding** for the `EvidenceClip` and `Full+AudioClip`
  projections. The projections currently emit a documented
  `audio_clip` placeholder (`{"codec":"opus","bytes":""}`); the
  wire shape is stable, vendoring libopus + filling the bytes is a
  pure addition. Integrators who don't need clip projections are
  unaffected.
- **4h: RFC 3161 TSA hook**. The `tsa_endpoint` config field
  parses but `tsa_timestamp` isn't constructed yet. The
  `SENTLOG_ERR_TSA` status is reserved. The default `tsa_required:
  false` preserves inert-by-default, so the absence is silent;
  setting `tsa_required: true` would error on the next dispatch
  rather than silently mis-attest (a future chunk needs to wire
  this).

The remaining Phase 4 deliverables (envelope construction +
projections + signing + sink fanout + LocalQueue + drainQueue +
sampling + reference verifier) are all shipped and CI-green. The
two deferred items add bytes/timestamps inside the same envelope
shape; they don't change the public ABI surface.

**Doc-debt carried forward** (also surfaced by the Phase 5
closing audit):

- Spec §4.2 declares the abstract `Sink ∈ { … }` typed schemas
  but never shows the concrete on-wire JSON shape the integrator
  writes (`{"sink": {"kind": "http", "url": "…", "headers":
  {…}}}`). Worth a one-paragraph addendum with one JSON example
  per `sink_kind`.
- The LocalQueue wire format (one envelope per `\n`-terminated
  NDJSON line, sidecar `.lock` file, tmp+rename atomic drain) is
  only documented inline in `c_api.cpp` / `state.cpp`. Spec §4.5
  should call this out explicitly so external tooling can read
  queue files without source diving.

### Phase 5 — Video affect + permissions (Weeks 9–10)

- Video affect module: MediaPipe Face Mesh on Android, Vision face
  landmarks on iOS. 10–15 Hz sampling during utterance. Small affect
  head: valence, arousal, dominant FAU.
- `affect` field on `SentlogResult`; contributes
  `engagement.face_score`.
- iOS privacy manifest (`PrivacyInfo.xcprivacy`): microphone, motion,
  camera (when affect enabled).
- Android runtime permissions documented for integrators.
- Optional device-integrity hooks (Play Integrity / AppAttest).

Exit: video-affect path complete behind `use_video_affect` flag;
voice-only path remains a first-class citizen with no permission
inflation.

**Shipped in Phase 5**:

- C ABI surface: `sentlog_push_face_frame(handle, ts_ns, valence,
  arousal, dominant_fau_csv)` (`shared/include/sentlog/sentlog.h`).
- Pipeline aggregation: mean valence / arousal, dominant FAU on
  ≥25 % of frames, derived `affect` block emitted only when
  `use_video_affect == true` AND frames were pushed
  (`shared/src/internal/pipeline.cpp`).
- `engagement.face_score` contributes to the engagement ensemble
  via the existing `scoring_weights.engagement.face` weight
  (default 0.5 per `docs/scoring-weights.md`).
- Privacy enforcement: raw landmarks structurally cannot reach
  the envelope — only the derived push values cross the C ABI
  (spec §5.3). 6 pipeline tests cover absent-frame, present-frame,
  clamp, gate-off-suppression, lifecycle errors.
- Permissions: iOS `PrivacyInfo.xcprivacy` documents
  microphone / motion / camera; Android manifest declares
  `RECORD_AUDIO` + `CAMERA` with `required=false`; integrator
  checklist at [`docs/permissions.md`](permissions.md).

**Deferred to Phase 5.5 (post-Phase 5)**:

- **5b: Android MediaPipe Face Mesh + CameraX capture**. The C
  ABI is in place; what's left is platform-side wiring — vendor
  the MediaPipe tasks-vision AAR, drive a 10-15 Hz camera frame
  loop, push derived values via `sentlog_push_face_frame`. Needs
  the affect-head MLP that maps 468 landmarks → `(valence,
  arousal, FAU)`, which in turn needs training data and a
  pretrained checkpoint.
- **5c: iOS Vision face landmark capture**. Same shape: Vision
  framework face-landmark detection at 10-15 Hz, derived push
  through the same C ABI. Affect head same as 5b.
- **5e: Device-integrity hooks** (Play Integrity / AppAttest /
  DeviceCheck). Surfaces as `evidence_grade: "DeviceAttested"`
  on the envelope when the platform attestation succeeds; same
  inert-by-default pattern as the rest of the SDK.

The deferred items don't change the C ABI surface — they wire
real sensor data into the existing `sentlog_push_face_frame` path
and surface attestation status as an existing enum value.

### Phase 6 — React Native + Capacitor bridges (Weeks 11–12)

The first integrator class is expected to be JS-driven hybrid stacks
(Capacitor-wrapped PWAs and React Native apps); bridges ship with v1.0,
not after it.

- `@sentlog/react-native` npm package. TypeScript surface mirrors the
  native API. Android: Kotlin `ReactContextBaseJavaModule` (TurboModule
  where supported). iOS: Swift class exposing `@objc` async methods.
  Promises map to JS Promises. Events via `RCTEventEmitter`.
- `@sentlog/capacitor` npm package. TypeScript surface mirrors the
  native API. Android: Kotlin `@CapacitorPlugin`. iOS: Swift
  `CAP_PLUGIN`-annotated class. Each native method takes a
  `PluginCall`. Events via `notifyListeners`.
- Shared TypeScript types between the two packages — types live in the
  native SDK as the source of truth; both bridges import a generated
  `.d.ts` so they cannot drift.
- Binary fields (fingerprint bytes, signed envelopes) base64-encoded
  across the bridge.
- Sample apps for each: a minimal RN screen and a minimal Svelte/Ionic
  + Capacitor page that call `Sentlog.evaluate()` and display the
  result. Both ship in `tools/sample-react-native/` and
  `tools/sample-capacitor/`.

Exit: both npm packages publishable as `0.x` prereleases; sample apps
run on real devices on both platforms; type parity with the native
`SentlogResult` verified by snapshot tests.

### Phase 7 — OTA + hardware-backed signing + hardening + soak + v1.0 (Weeks 13–16)

Four weeks to absorb hardware-backed signing and the evidence-verifier
tool. Soak runs in parallel through weeks 14–16.

- OTA model update via signed manifest with two rotatable pinned keys.
  Exposed as `Sentlog.checkForUpdates() -> UpdateResult`. `evaluate()`
  never blocks on OTA.
- **Custom manifest URL + integrator-supplied signing keys** wired
  through. SDK defaults remain the Sentlog-published manifest URL and
  Sentlog public keys; integrators override `manifest_url` *and*
  `manifest_pubkeys` together. Mixed-source configuration is rejected
  at init with a typed error. Verification logic is the same
  Ed25519/SHA-256 path used for the Sentlog-published manifest.
- **Hardware-backed signing**: iOS Secure Enclave ECDSA-P256, Android
  KeyStore Ed25519 (API 33+) or ECDSA-P256 (API 31–32) with StrongBox
  where available. Opt-in via `use_hardware_signing`;
  `hardware_signing_required=true` enforces it. Successful enclave
  signing promotes `evidence_grade` to `EnclaveSigned`. Software
  fallback emits a `HardwareSigningUnavailable` telemetry event.
- `on_telemetry` callback wired through every pipeline stage.
- Two-week soak across both integrator apps for crash-free session
  rate. The SDK is available to integrators at the start of Phase 6;
  integrators' integration work runs on their own timeline and is not
  on the SDK team's critical path.
- Integrator-facing documentation in `docs/integration/`.
- Release notes, upgrade guide, deprecation policy.
- **`tools/evidence-verifier/`** ships as a single-file Python tool
  with no SDK dependency. Takes an envelope + public key, returns a
  structured verification report. The reference verifier is the
  canonical implementation of the wire-format contract.

Exit: v1.0 internal release at end of week 16 (~end of month 4).

**Shipped in Phase 7**:

- 7a: OTA manifest parse + Ed25519 verify primitive in `shared/src/
  internal/ota.{h,cpp}`. Wire format documented in spec §5.1.
- 7b-i: C ABI `sentlog_check_for_updates(handle, bytes, len, cb, ud)`
  + `SentlogConfig.manifest_url` / `manifest_pubkeys` with paired-
  override validation (mixed source rejected at init).
- 7b-ii: Platform HTTPS fetch via stdlib (`URLSession` on iOS,
  `HttpURLConnection` on Android) wired into the no-arg
  `Sentlog.checkForUpdates()`. HTTPS-only, 10 s timeout, 64 KB cap.
- 7c-i: Hardware-signer architecture — `sentlog_set_hardware_signer`
  C ABI + signer-callback pattern, `evidence_grade` promotion to
  `EnclaveSigned` on hw success, `HardwareSigningUnavailable`
  telemetry on fallback, drop-on-required.
- 7c-ii: iOS Swift `setHardwareSigner` public API with the
  Unmanaged.passRetained box pattern.
- 7d-stub: Android Kotlin `setHardwareSigner` + JNI trampoline with
  exception-safe ByteArray round-trip.
- 7f: `on_telemetry` sweep — `PipelineTiming`, `ScoreHistogram`
  (0.1-width buckets for the three top-level scores),
  `ForwarderOutcome=ok`, `CaptureAborted`, `ManifestVerifyFailed`
  events fire from the appropriate emission points.
- 7g: Integrator-facing documentation in `docs/integration/`
  (README, hardware-signing, ota, telemetry) +
  `docs/deprecation-policy.md`.

**Deferred to Phase 7.5 (post-Phase 7)**:

- **Real Secure Enclave + KeyStore signers**. The hardware-signer
  architecture ships in 7c, exposed via `setHardwareSigner` on both
  platforms — integrators wanting hw signing today supply their own
  SE / KeyStore-backed closure (full worked examples in
  `docs/integration/hardware-signing.md`). A SDK-bundled
  `SecureEnclaveSigner` (iOS) / `KeyStoreSigner` (Android) helper
  that takes care of key creation + biometric prompts + StrongBox
  fallback lands in 7.5 without changing the API.
- **≥72 h soak** across both integrator apps. Real-device-dependent;
  belongs alongside the integrator-side rollout, not the SDK team's
  critical path.
- **v1.0 cut + release notes + upgrade guide**. Pending the soak
  + helper-signer work; upgrade guide is a stub until there's a prior
  released version to upgrade from.
- `tools/evidence-verifier/` — already shipped in Phase 4i; listed
  here in the rollout plan but not re-deferred.

**Shipped in Phase 7.5 (partial — sub-chunks 7.5-i + 7.5-ii)**:

- **7.5-i**: `SecureEnclaveSigner` Swift helper
  (`ios/Sources/Sentlog/SecureEnclaveSigner.swift`). ECDSA-P256 key
  creation inside the Secure Enclave under an integrator-named
  keychain tag, keychain persistence + lookup, raw P-256 pubkey
  export, `did:key:` URI derivation (multicodec 0x1200 + compressed
  pubkey + base58btc, inline impl). Convenience overload:
  `Sentlog.setHardwareSigner(_ signer: SecureEnclaveSigner)`.
- **7.5-ii**: `KeyStoreSigner` Kotlin helper
  (`android/sentlog/src/main/kotlin/me/cooki/sentlog/KeyStoreSigner.kt`).
  ECDSA-P256 in AndroidKeyStore with transparent StrongBox fallback
  (handles `StrongBoxUnavailableException` + the Samsung early-API-28
  `ProviderException` edge case), same raw-pubkey + did:key surface
  as the iOS helper, exposes `strongBoxBacked: Boolean`.
  Convenience overload:
  `Sentlog.setHardwareSigner(signer: KeyStoreSigner)`.
- `docs/integration/hardware-signing.md` now leads with the two- /
  three-line helper usage on both platforms; the manual SE /
  KeyStore wiring stays as a "if you need biometric gating / custom
  rotation / Ed25519 on API 33+" fallback.

**Still pending in Phase 7.5**:

- **7.5-iv**: ≥72 h soak across both integrator apps; v1.0 cut
  + release notes + upgrade guide. Real-device + release work; not
  on the SDK team's CI-loop critical path.

### Phase 8 — Public release readiness (Months 4+)

Starts at the end of Phase 7. Specific items are gated by the corpus
milestones in the whitepaper.

- Per-attack-family published benchmarks (replay, known TTS,
  cross-family TTS, voice-cloning, voice-cloning-over-telephony).
  Requires ~50k+ labelled clips — gated to months 7–12.
- Multi-language support beyond English. Requires per-language
  evaluation set — gated to months 7–12.
- Public docs site, sample apps, conformance test suite.
- **Reserve item:** lip-sync anti-spoof. Cheap to activate because
  affect mode already captures face-mesh data at 10–15 Hz; lip-sync
  becomes an additional analysis pass on existing captures rather
  than new capture infrastructure.

### Phase 9 — `@sentlog/web` (browser WebAssembly) — Post-MVP

Same C++ core compiled to WASM via Emscripten. whisper.cpp has mature
WASM builds; `sentlog-prosody-v1` is pure C++ and compiles to WASM
transparently; AASIST-L can be ONNX-WASM. TypeScript surface mirrors
`@sentlog/capacitor`. Different
runtime — not in v1.0; tracked here as a defined post-MVP deliverable.
Estimated 3–4 weeks of work when scheduled.

---

## 2. The Android Benchmark Gate

### 2.1 Target hardware

- Samsung A14 5G (Mediatek Dimensity 700) — sub-$200 floor
- Pixel 6a (Tensor G1) — mid-range proxy
- Moto G Power 2024 (Snapdragon 6-class) — US carrier floor

### 2.2 Workload

- 15s clip, 16 kHz mono PCM, ≥30 dB SNR
- Full pipeline: VAD → whisper.cpp tiny Q5_1 (ACFT-style context) →
  eGeMAPS + whisper-probe prosody → AASIST-L liveness → score
  assembly → sentlog-pcp-v1 fingerprint
- 20 cold + 20 warm runs per device
- Video path benchmarked separately when affect mode is the target

### 2.3 Thresholds

| Metric | Pass | Review trigger |
|---|---|---|
| Warm pipeline p50 (no video) | ≤ 6 s | > 8 s |
| Warm pipeline p95 (no video) | ≤ 9 s | > 12 s |
| Warm pipeline p50 (with video) | ≤ 7 s | > 9 s |
| Cold model load | ≤ 4 s | > 8 s |
| Peak memory (no video) | ≤ 350 MB | > 450 MB |
| Peak memory (with video) | ≤ 420 MB | > 520 MB |

### 2.4 Escape valves (ranked)

1. ACFT-style variable-length encoder context — baseline in Phase 2.
2. GPU/Vulkan delegate via ggml where supported.
3. Reduce prosody head to `sentlog-prosody-v1` only on Android, drop whisper-probe.
4. Switch iOS to an Apple-platform-native ASR alternative (iOS-only).
5. Raise min Android API / SoC class (product decision).

### 2.5 OTA fetch behaviour

- `Sentlog.checkForUpdates()` is the *only* mechanism that fetches
  model updates. Integrator-triggered, returns asynchronously, never
  runs in the background.
- `evaluate()` does not initiate OTA. If a fresh model is required,
  the integrator calls `checkForUpdates()` first.
- Verification (Ed25519 signature, SHA-256 content hash) gates any
  on-disk swap. A failed verification leaves the previously known-good
  model in place and reports the failure via the telemetry callback if
  one is configured.

---

## 3. Testing and Observability

### 3.1 Testing

- gtest for scoring assembly, threshold logic, manifest verification,
  config validation, fingerprint generation, forwarder retry/queue
  logic.
- Golden fixtures (~50–80 audio/motion/face fixtures), expected score
  ranges, run on every PR.
- Platform smoke (`connectedAndroidTest`, XCTest) drives `evaluate()`
  against bundled fixtures.
- Macrobenchmark + Xcode Metrics nightly on three target devices.
- API stability snapshot tests for Kotlin and Swift surfaces.
- Forwarder pipeline tests: HTTP retries, queue overflow, sampling
  fidelity, projection correctness.
- Integrator misuse coverage: malformed configs, lifecycle violations,
  permission denials.
- Two-week soak before v1.0.

### 3.2 Observability

`on_telemetry: ((TelemetryEvent) -> Unit)?` is the only observability
hook. Events carry pipeline stage timings, model versions, score
histogram bucket, forwarder outcomes, error codes. No audio. No clip
ID. No user identifier. No transcription content. No video frames.

---

## 4. Risks

| Risk | Impact | Mitigation | Status |
|---|---|---|---|
| Android inference speed on mid-range | UX breaking if pipeline > 12s | Benchmark gate (§2); ACFT baseline | Unvalidated until Week 2 |
| Voice-cloning attacks at scale | Cross-generator liveness 55–80% | Multi-modal scoring; per-attack-class reporting; lip-sync deliberately deferred as cheap-to-activate reserve weapon | Partial — deliberate |
| AASIST-L pretrained weight licence | (Resolved — was: 4-week slip if non-permissive) | `clovaai/aasist` repo + weights MIT; ASVspoof 2019-LA training data ODC-BY 1.0. Attributions in `THIRD_PARTY_NOTICES.md`. | Resolved — see ADR 0004 |
| Upstream OSS dependency drift | whisper.cpp / Silero / MediaPipe stagnate or break | Pin to specific upstream versions; monthly health check; budget for in-house maintenance fork if any project goes silent for 6+ months | Tracking |
| Engagement model cold start | First 5–7 months scored against thin model | `engagement_model_status` field surfaces truth; integrators forbidden to display engagement until `"trained"` | Managed |
| Gaming at $0.01 scale | Fraudulent submissions reduce pool value | Multi-modal catches farms; fingerprint forwarding catches duplicates | Addressed |
| Self-labelled corpus pace | Engagement timeline depends on labelling throughput | Revised to Months 5–7; labelling budget Phase 0 question | Managed |
| Multi-language performance | Direction degrades on non-English | Per-language gating Phase 2; per-language eval pre-launch | Phase 2 |
| Face data permission drop-off | Video-affect opt-in costs install conversion | Video is opt-in per integrator; voice-only is first-class | Acknowledged |
| Model manifest key compromise | Attacker ships arbitrary model | Two rotatable pinned keys; rotation cycle in spec | Addressed |
| App store ML/camera policy | Apple/Google may flag bundled microphone+camera SDKs | Internal testing track in Phase 5–6; weights are data, not code (Apple §2.5.2) | Managed |
| AI-coded FFI safety | JNI/Swift bridging lifetime bugs not caught by unit tests | Narrow C ABI; ASan+UBSan in CI; ≥72h soak | Managed |
| Forwarder leak via misconfiguration | Integrator accidentally ships clips when only fingerprints intended | Strong type discrimination on `Projection`; `Full+AudioClip` is verbose and explicit | Managed |
| Transcript transmission as personal data | Integrator ships text via `TranscriptOnly` without realising it's personal data under GDPR | Documentation calls it out; integrator is data controller | Managed |
| Demographic bias inherited from upstream models | Scores may differ across accent, skin tone, age, noise environment | Acknowledged in spec; per-segment benchmark publication post-MVP | Acknowledged |

---

## 5. Open Items

### 5.1 Product decisions

1. **Target Android device list** (§2.1) — confirm the three devices.
2. **Engagement labelling budget** — clips/month rate determines
   whether the first reliable engagement model lands at month 5 or
   month 7.
3. **Lip-sync activation policy** — commit to a future activation
   window, or leave as reserve for if/when voice cloning becomes a
   felt production problem.

### 5.2 Phase 0 engineering gates

1. **C ABI header reviewed** by one engineer per integrator before
   anything depends on it.

(AASIST-L pretrained weight licence verified GREEN — see ADR 0004.
LICENSE committed at repo root.)

---

## 6. Summary

MVP target is 16 weeks to v1.0 internal release. The single
highest-risk technical item is the Week 1–2 Android benchmark gate
(§2). The single largest Phase 0 dependency is the AASIST-L
pretrained-weight licence (§5.2).

After v1.0, public release readiness items (multi-language,
per-attack-class benchmarks, conformance test suite) are gated by
corpus growth. `@sentlog/web` is a defined post-MVP deliverable.

For architecture, API, and contract details, see the [Technical
Specification](./technical-specification.md). For positioning and
multi-modal rationale, see the [Whitepaper](./whitepaper.md).
