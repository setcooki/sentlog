# Sentlog — Whitepaper

**A universal, device-first evidence and presence SDK.**

**Licence:** MIT — open-source from day one.
**Audience:** Anyone evaluating Sentlog as a building block — product
managers, platform engineers, integration architects, regulators, the
open-source community.

Companion documents:
- [Technical Specification](./technical-specification.md) — architectural decisions, API, contracts.
- [Rollout Plan](./rollout-plan.md) — phases, gates, risks, open items.

---

## 1. What Sentlog Is

Sentlog is a mobile SDK that captures a short voice interaction
(optionally augmented with motion and facial-affect signals), produces
a multi-dimensional presence assessment, and emits the result as a
signed, durable evidence envelope. It runs entirely on-device,
transmits nothing unless an integrator wires it up, and is inert by
default.

It is not a verdict-issuing system. It is the evidence-producing layer
on which integrators build verdicts.

Several positions need stating up front:

1. **Liveness is a risk signal, not an authenticator.**
   Cross-generator performance against modern voice cloning sits at
   60–80% clean and 55–70% over telephony. Sentlog publishes
   per-attack-class numbers, never a single headline accuracy.

2. **No SDK-owned cloud.** The SDK ships scoring; integrators name
   every destination. Composable forwarding is the only output
   channel.

3. **The engagement model is a small probe over pretrained features.**
   Whisper-encoder hidden-state linear probe (~50 KB) +
   `sentlog-prosody-v1` MLP (~200 KB), ensembled. Marginal compute
   is near-zero
   because whisper is already running for ASR.

4. **Transcript is a first-class output.** The pipeline runs ASR
   anyway; the transcript ships back to the integrator with timings
   and language detection.

5. **Evidence is the output shape.** Every result is wrapped in a
   W3C Verifiable Credentials 2.0 envelope with Ed25519 (or ECDSA-P256
   when hardware-backed) signatures, model versions, timestamps, and
   a perceptual audio fingerprint. Verifiable weeks or months later
   by a tool that has only the envelope and a public key.

---

## 2. Foundational Principle — A Pure Library, Inert by Default

Sentlog is a library, not a service. No daemon, no background process,
no autonomous behaviour, no SDK-owned backend, no opinion on
destinations. The integrator's app is the runtime. Sentlog runs when
called, returns when done, forwards if configured, and is otherwise
silent.

Concrete consequences:

- **All defaults are inert.** `forwarders: []`. No telemetry. No
  motion sensors. No video. No device integrity. Every capability
  is opt-in.
- **No autonomous behaviour anywhere.** OTA fetch is via an explicit
  `Sentlog.checkForUpdates()` call. Forwarders fire only after an
  integrator-initiated `evaluate()`. Telemetry only if a callback
  is provided.
- **No opinion on backends.** Sinks are primitives — HTTP, webhook,
  local queue, callback. They are not destinations. The integrator
  names every endpoint.
- **No SDK-side state survives the app** other than cached models.
- **No SDK-level user model.** No user IDs, no sessions in the auth
  sense, no accounts. `session_id` is opaque pass-through metadata
  supplied by the integrator.

---

## 3. Positioning — Universal, Device-First, Evidence and Presence

Four words; each carries design weight.

### Universal

Runs in every realistic mobile integrator context: native iOS, native
Android, Capacitor-wrapped PWAs, React Native apps. One shared C++
core; thin per-platform and per-framework bridges. Distributed as
Maven/SPM artefacts plus `@sentlog/react-native` and
`@sentlog/capacitor` npm packages.

Browser-only / WebAssembly support (`@sentlog/web`) is a deliberate
post-v1.0 direction — same C++ core compiled via Emscripten, same
architecture, additional runtime target.

### Device-first

Capture and inference happen on the device. Defaults are inert:
nothing transmits, nothing runs in the background, no cloud is
implied by integration. Composable forwarding ships the result
wherever the integrator names — and only if the integrator names it.

### Evidence

The SDK's output is not a verdict; it is a *durable, verifiable
artefact*. Every result carries:

- A W3C Verifiable Credentials 2.0 envelope with Ed25519 or ECDSA-P256
  `DataIntegrityProof`.
- Model versions for every component (liveness, ASR, prosody, affect,
  manifest).
- A timestamp; optionally an RFC 3161 TSA timestamp when the integrator
  configures a TSA endpoint.
- A perceptual audio fingerprint (`sentlog-pcp-v1`, a homegrown
  MIT-licensed Goertzel-based delta hash) for duplicate detection
  without exposing the audio.
- The transcript with word timings.

Hardware-backed signing (Secure Enclave / Android KeyStore) is
supported in v1.0; when active, the envelope carries an `EnclaveSigned`
evidence grade. The envelope is designed to be stored, replayed,
presented, and verified weeks or months later by a tool that has only
the envelope and a public key.

### Presence

The subject of the evidence. Presence is multi-dimensional:

- **Liveness** — is this a real human speaking now?
- **Engagement** — is the delivery genuine vs mechanical repetition?
- **Direction** — is the intent aligned with a stated target?
- **Presence (proper)** — is the device in a real environment, in a
  human hand, on real hardware?
- **Affect** (optional) — facial valence and arousal during the
  utterance.

Each dimension is exposed separately; the SDK does not collapse them
into a single number. Integrators compose their own verdict.

### What Sentlog is not

Sentlog is not a trust SDK, not an authentication SDK, not a fraud
SDK, not a biometric identity SDK. It is the *evidence-producing
layer* on which integrators build those things.

---

## 4. Open-Source Positioning

Sentlog ships under MIT. There is no IP defense in the SDK — every
model architecture used (AASIST-L, whisper, Silero VAD) is
publicly described, pretrained weights are public, libraries are
open-source. Two components are homegrown MIT replacements where the
canonical implementation's licence would compromise the SDK's
distribution story: the perceptual audio fingerprint
(`sentlog-pcp-v1` instead of LGPL Chromaprint) and the prosody
feature extractor (`sentlog-prosody-v1` instead of GPL openSMILE —
the eGeMAPS feature *set* is the published spec we implement, the
openSMILE *implementation* is what's GPL). Sentlog stands on these
shoulders and provides shoulders for others to stand on.

The labelled corpus (see §6 below) is a model-quality investment, not
an IP moat. It improves the engagement and affect heads as it grows
and may itself be released openly with proper consent management.

Differentiation lives in:

- **Integration ergonomics** — composable forwarding, inert defaults,
  the right primitives for hybrid mobile stacks.
- **Multi-modal trust-signal design** — audio source + motion
  coherence + optional face affect, in one composable pipeline.
- **Standards alignment** — VC 2.0 envelopes, RFC 3161 timestamps,
  Ed25519 / ECDSA-P256 signatures with hardware backing.
- **The gradually accumulating labelled corpus** — none of which is
  patentable nor secret, all of which are well-designed primitives
  that adjacent SDKs in this space do not ship.

Sentlog occupies a slot adjacent to other open-source vertical SDKs
that the industry standardises around — composable primitives that
larger systems depend on rather than commercial products competing
for end-customers.

---

## 5. Architecture at a Glance

```
                ┌─────────────────────────────────────────────────┐
                │  Integrator app                                  │
                │  (native / Capacitor PWA / React Native host)   │
                └────────────────┬────────────────────────────────┘
                                 │ idiomatic API (Kotlin / Swift / TS)
                ┌────────────────▼───────────────┐
                │  Platform binding              │  thick — most logic
                │  android/  Sentlog.kt          │  lives here:
                │  ios/      Sentlog.swift       │  capture orchestration,
                │  rn/       (npm)               │  telemetry,
                │  capacitor/ (npm)              │  config validation,
                │                                │  OTA fetch + verifier,
                │                                │  motion / route
                │                                │  capture, ML inference
                └────────────────┬───────────────┘
                                 │ C ABI (extern "C" — narrow)
                ┌────────────────▼───────────────────────────────┐
                │  shared/  C++17 core                            │  thin —
                │   ├── capture/    ring buffer, framing          │  only what
                │   ├── vad/        energy VAD fallback           │  must be
                │   ├── asr/        transcript intake             │  C++:
                │   ├── prosody/    sentlog-prosody-v1            │  orchestration,
                │   ├── liveness/   liveness result intake        │  scoring,
                │   ├── scoring/    score assembly                │  fingerprint,
                │   ├── fingerprint/ sentlog-pcp-v1 hash          │  audio
                │   └── config/     SentlogConfig validation      │  arithmetic
                └────────────────┬───────────────────────────────┘
                                 │
                ┌────────────────▼───────────────────────────────┐
                │  Platform runtimes                              │
                │  iOS + Android: ONNX Runtime (VAD, liveness)    │
                │                 whisper.cpp (ASR, CPU-only)     │
                │  Face mesh:     integrator-supplied (affect)    │
                └─────────────────────────────────────────────────┘

  Auxiliary inputs (multi-modal):
   - Audio input route → liveness
   - Accelerometer + gyroscope → presence (motion)
   - Front camera (opt-in) → affect (sentiment enrichment)
   - Device integrity attestation (opt-in) → presence (integrity)

  Output pipeline:
   SentlogResult ─► forwarders[0..n] ─► sinks (Http / Webhook / Queue / Callback)
                                        with per-sink projection + sampling;
                                        delivery and retry in the integrator's dispatcher
```

Detailed architectural decisions are in the [Technical
Specification](./technical-specification.md).

---

## 6. Multi-Modal Signals

The research is clear: multi-modal beats pure-audio defences. Sentlog
exposes four orthogonal signal channels in v1.0.

### 6.1 Audio input source (always on)

Built-in mic vs USB vs Bluetooth vs loopback. Device farms wire phones
to central audio sources; "USB audio in" during participation is a
near-certain fraud signal. Score: `presence.audio_source` ∈ [0,1].
Built-in mic = 1.0; USB/Bluetooth = 0.2; loopback/unknown = 0.0.
Always on — negligible cost, strong signal.

### 6.2 Motion coherence (opt-in)

Phones held by humans show micro-tremor and speech-coupled vibration.
Docked or rig-mounted phones are dead still. The SDK samples
accelerometer + gyroscope at ~100 Hz during recording, computes
tremor energy in the physiological band and cross-correlation between
the speech envelope and high-passed accelerometer energy, and emits
`presence.motion` ∈ [0,1] (or null if disabled).

### 6.3 Device integrity (opt-in)

Play Integrity / AppAttest / DeviceCheck. The SDK never enforces; it
reports `presence.integrity` ∈ [0,1] (or null). Integrators that care
about device assurance enforce it themselves.

Not implemented yet: `presence.integrity` is always null and
`use_device_integrity` has no effect. Today a token the integrator pushes
with `setDeviceAttestation` promotes `evidence_grade` to `DeviceAttested`.

### 6.4 Video affect — sentiment enrichment (opt-in)

Video is positioned as a *sentiment* signal, not an anti-spoof
signal. The integrator runs a lightweight face mesh (typically MediaPipe
on Android, Vision on iOS; neither is an SDK dependency), samples
landmarks at 10–15 Hz during the utterance, and feeds a small affect head (FAU-based, or distilled affect models
trained on RAVDESS / CREMA-D face channels). Output is `affect`
(valence, arousal, dominant FAU) plus a contribution to
`engagement.face_score`. Overall `engagement.score` ensembles voice
prosody and face affect.

Video frames never leave the device under any forwarder projection.
Only derived scalars appear in the result.

### 6.5 Lip-sync — reserved, not in v1.0

Lip-sync coherence is the strongest available anti-cloning signal. It
is deliberately deferred because the current threat model is dominated
by device farms and replay, not voice cloning at scale, and because
affect mode in v1.0 already captures the face-mesh data at 10–15 Hz —
adding lip-sync later is an additional analysis pass on existing
captures rather than new capture infrastructure. The cost to activate
remains low when the threat model demands it.

### 6.6 Out of scope for v1.0

- Network / VPN / geolocation coherence — server-side concern.
- Ambient light, proximity, battery/thermal patterns — telemetry-only;
  not score inputs.
- Multi-language script handling beyond Latin scripts. RTL and CJK
  transcripts work because whisper supports them, but no per-script
  display-side validation ships in v1.0; integrators rendering
  transcripts handle bidi and font selection themselves.

---

## 7. Data Strategy and Corpus

Sentlog is open-source. The corpus is therefore not an IP moat — it
is an investment in model quality. Better data improves the engagement
and affect heads.

### 7.1 Public datasets

| Dataset | Licence | Use |
|---|---|---|
| CREMA-D | ODbL — commercial permitted | Face affect head bootstrap |
| ASVspoof 2019 / 5 eval | Research, public | Liveness evaluation |
| MSP-Podcast | Research-only | Internal evaluation, not training |
| IEMOCAP, MELD, RAVDESS | Research-only / copyright | Not usable for commercial training |

The voice-side engagement head is a whisper-encoder linear probe, so
it relies on pretrained whisper features rather than emotion-labelled
audio corpora.

### 7.2 Self-labelled corpus

Built from samples integrators forward via configured forwarders, with
end-user consent obtained by the integrator. Labels:

- `liveness`: genuine / replay / TTS / cloned
- `engagement`: genuine / mechanical / read-aloud
- `direction`: aligned / unrelated / off-target
- `affect`: valence / arousal independently rated

Milestones:

| Labelled clips | Model state | Timeline |
|---|---|---|
| 0 → 10k | Bootstrap on CREMA-D + ASVspoof; engagement `"training"` | MVP through end of v1.0 |
| 10k → 50k | First reliable engagement model; `"trained"` | Months 5–7 |
| 50k → 200k | Multi-language; refined per-attack-class liveness | Months 7–12 |
| 200k+ | Public per-attack-class benchmarks published | Months 12–14 |

### 7.3 No public dataset for "genuine vs mechanical repetition"

The closest literature (read-vs-spontaneous, acted-vs-spontaneous)
validates the feature set (rhythm metrics, F0 dynamics, pause entropy)
but does not provide training labels. Sentlog will be the first
labelled corpus for this task. The corpus improves model quality but
is not protected.

---

## 8. Summary

Sentlog is a universal, device-first, evidence-and-presence SDK
released under MIT. The architecture is open, the dependencies are
open, the wire format aligns with W3C Verifiable Credentials, and the
SDK does nothing by default beyond returning a result to the caller.
Differentiation lives in the integration primitives, the multi-modal
signal design, and the labelled corpus that accrues over time.

For architectural decisions, API contracts, and security model, see
the [Technical Specification](./technical-specification.md). For
phases, gates, and execution risk, see the [Rollout
Plan](./rollout-plan.md).
