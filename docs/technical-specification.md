# Sentlog — Technical Specification

**Architecture, API, contracts, versioning.**

**Licence:** MIT.
**Audience:** Engineers integrating, contributing to, or auditing the
SDK. Verifier authors. Security researchers.

Companion documents:
- [Whitepaper](./whitepaper.md) — vision, principles, positioning, multi-modal rationale, data strategy.
- [Rollout Plan](./rollout-plan.md) — phases, benchmark gate, testing, risks.

---

## 1. Architectural Decisions

| # | Decision | Rationale |
|---|---|---|
| 1.1 | **C++17 shared core with hand-written C ABI; thin Kotlin (JNI) + Swift (C-interop) wrappers** | The C/C++ ML and audio dependencies the SDK wraps mean any other shared-core language pays the C++ toolchain tax plus its own. Score parity is structurally automatic when the same compiled object code runs on every platform. |
| 1.2 | **whisper.cpp tiny Q5_1 on both platforms; iOS uses the whisper.cpp Core ML build** | One quantisation pipeline, one set of golden fixtures. An Apple-platform-native ASR alternative is the documented iOS escape valve. |
| 1.3 | **Baseline model bundled; OTA via Ed25519-signed JSON manifest with two rotatable pinned keys; explicit `checkForUpdates()` API — never autonomous** | Industry standard. Apple §2.5.2 permits — model weights are data, not code. |
| 1.4 | **16 kHz mono PCM, 16-bit, real-time audio thread → C++ ring buffer** | No platform-specific resampling in shared core. |
| 1.5 | **Kotlin: `suspend fun evaluate(): SentlogResult`. Swift: `func evaluate() async throws -> SentlogResult`** | Native idioms per platform; cancellation is free. |
| 1.6 | **`Sentlog` brand. Cocoa-style prefixes: `SentlogResult`, `SentlogConfig`** | Idiomatic in Swift, fine in Kotlin. |
| 1.7 | **AASIST-L for liveness from week 2, not rule-based** | AASIST-L is the small variant of the AASIST graph-attention anti-spoofing architecture — ~85K parameters, real-time on CPU, ~1% EER on ASVspoof 2019-LA closed-set. Paper attribution and weight licence in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). |
| 1.8 | **Whisper-encoder linear probe (~50 KB) + `sentlog-prosody-v1` MLP head (~200 KB), ensembled** | Marginal compute is near-zero because whisper is already running for ASR; the encoder's hidden state is reused as a feature stream for the prosody probe. The MIT prosody extractor (`shared/src/internal/prosody.{h,cpp}`) replaces the rollout-plan's original openSMILE choice — openSMILE is GPL / commercial-dual which is incompatible with our MIT distribution; the eGeMAPS feature *set* is a published spec, the openSMILE *implementation* is what's GPL. |
| 1.9 | **Composable forwarding for all output destinations** | Fully decouples from any specific service. Integrator names every endpoint. |
| 1.10 | **Audio input source + motion coherence are first-class signals; device integrity optional** | Catches the highest-volume fraud class (device farms). |
| 1.11 | **Video used for affect / sentiment enrichment only; lip-sync as anti-spoof is reserved for future activation** | Augments engagement scoring. Lip-sync is cheap to add later because affect mode already captures face-mesh data. |
| 1.12 | **Audit transmission is one projection of composable forwarding, not a separate channel** | One configuration surface; same primitive supports analytics, fraud, audit, moderation. |
| 1.13 | **`Transcript` is a first-class field on `SentlogResult`** | Pipeline computes it anyway. |
| 1.14 | **All code AI-written. Minimise C++ surface area. Sanitizers in CI non-negotiable.** | See §5.6. |
| 1.15 | **MIT licence, open-source from v1.0** | Aligns with foundational dependencies (whisper.cpp MIT, Silero MIT, MediaPipe Apache 2.0). The perceptual audio fingerprint is a homegrown MIT implementation (`sentlog-pcp-v1`) — Chromaprint (LGPL) was considered and dropped because the LGPL static-linking obligations would burden the SDK's single-binary distribution story. |
| 1.16 | **React Native and Capacitor JS bridges ship with v1.0** as part of the SDK; not deferred to a later release | The first integrator class is expected to be JS-driven hybrid stacks (Ionic / Svelte / React-based PWAs wrapped in Capacitor, or native apps built with React Native). The SDK is unusable to them without bridges. Both bridges are thin wrappers over the same native API surface — distributed as `@sentlog/react-native` and `@sentlog/capacitor` npm packages. |
| 1.17 | **Types-only entry point published** (`@sentlog/types` or per-bridge `/types` subpath) | Integrator monorepos commonly enforce zero-runtime-dependency policies on their shared/types workspace packages. |
| 1.18 | **Raw [0,1] scores; no SDK-side calibration or capping** | `thresholds_met` is the only field that uses the threshold values from config. All score fields are unmodified probability outputs. Integrators apply their own calibration on top. |
| 1.19 | **`SentlogResult.extensions: Map<String, Any>`** — opaque integrator-owned passthrough field | Integrators stuff their own signals into the same forwarder pipeline without having to mirror Sentlog's transport. JSON-passthrough; the SDK never interprets the contents. |
| 1.20 | **Custom manifest URL + integrator-supplied signing keys** | `SentlogConfig.manifest_url` and `manifest_pubkeys` let integrators ship their own model improvements. Overriding either requires overriding both. |
| 1.21 | **Custom scoring weights** | `SentlogConfig.scoring_weights: Map<String, Float>?` accepts named weights for composite scores. Unrecognised keys rejected at init. |
| 1.22 | **The output is evidence, not a verdict** | Public framing centres on durable, verifiable evidence. The SDK does not claim "this is a real human" — it produces evidence the integrator interprets. |
| 1.23 | **`Presence.summary` and `evidence_grade` are first-class fields** | `Presence.summary` ensembles the populated `presence` sub-fields. `evidence_grade` declares the assurance level so downstream consumers can apply differential trust. |
| 1.24 | **Envelope wire format is locked with explicit `envelope_format` versioning** | The envelope is the durable evidence artefact, verified by tools that may have only the envelope and a public key. Wire-format guarantees in §6.4. Reference verifier ships in `tools/evidence-verifier/`. |
| 1.25 | **Envelope conforms to the W3C Verifiable Credentials 2.0 data model** | `@context`, `type`, `issuer`, `validFrom`, `credentialSubject`, `proof` follow VC 2.0. Proof uses `DataIntegrityProof` with `eddsa-rdfc-2022` or `ecdsa-rdfc-2019`. Integrator supplies `issuer` at init. |
| 1.26 | **Optional RFC 3161 TSA timestamp** | When the integrator sets `SentlogConfig.tsa_endpoint`, the SDK requests an authoritative timestamp over the envelope hash and embeds the response as `tsa_timestamp`. Default null preserves inert-by-default. |
| 1.27 | **Hardware-backed signing supported in v1.0** | iOS Secure Enclave (ECDSA-P256); Android KeyStore Ed25519 (API 33+) or ECDSA-P256 (API 31–32) with StrongBox. Opt-in via `use_hardware_signing`. When successful, `evidence_grade = EnclaveSigned`. |
| 1.28 | **`@sentlog/web` (browser WASM) is post-v1.0** | Same C++ core compiled via Emscripten, plus a TypeScript surface mirroring `@sentlog/capacitor`. Tracked as a defined post-MVP deliverable. |

---

## 2. Repository Layout

```
sentlog/
├── LICENSE                # MIT
├── README.md
├── SECURITY.md            # vulnerability disclosure policy
├── shared/                # C++17 core (narrow surface, heavily tested)
│   ├── include/sentlog/   # C ABI header — the only stable boundary
│   ├── src/
│   ├── tests/             # gtest + golden fixtures
│   └── CMakeLists.txt
├── android/               # AAR
│   ├── sentlog/           # Kotlin public API, JNI, OTA, sensors,
│   │                      # forwarder pipeline, face mesh
│   └── benchmarks/        # macrobenchmark module
├── ios/                   # XCFramework
│   ├── Sources/Sentlog/   # Swift public API, Core ML, OTA, sensors,
│   │                      # forwarder pipeline, Vision face mesh
│   ├── Sources/SentlogC/  # C interop shim
│   └── Tests/
├── rn/                    # React Native plugin (@sentlog/react-native)
│   ├── package.json
│   ├── src/               # TypeScript API
│   ├── android/           # NativeModule wrapping the AAR
│   └── ios/               # ObjC++ bridge wrapping the XCFramework
├── capacitor/             # Capacitor plugin (@sentlog/capacitor)
│   ├── package.json
│   ├── src/               # TypeScript API
│   ├── android/           # @CapacitorPlugin wrapping the AAR
│   └── ios/               # CAP_PLUGIN wrapping the XCFramework
├── models/
│   ├── prosody/
│   ├── liveness/          # AASIST-L training + ASVspoof eval
│   ├── affect/            # face affect head training
│   ├── whisper/           # Q5_1 quantisation + Core ML conversion
│   └── manifest/          # signed manifest tooling
├── tools/
│   ├── benchmark-harness/
│   ├── reference-consumer/         # minimal HTTP server (Python) for HttpSink
│   ├── evidence-verifier/          # single-file Python verifier: envelope + pubkey → report
│   ├── sample-capacitor/           # Svelte/Ionic + Capacitor sample app
│   └── sample-react-native/        # React Native sample app
└── docs/
    ├── whitepaper.md
    ├── technical-specification.md
    ├── rollout-plan.md
    ├── adr/
    └── benchmarks/
```

Integrator apps live in their own repositories.

The architecture diagram is in the [Whitepaper §5](./whitepaper.md#5-architecture-at-a-glance).

---

## 3. Public API Shape

### 3.1 Top-level API

```kotlin
// Kotlin
object Sentlog {
    fun initialize(config: SentlogConfig, onTelemetry: ((TelemetryEvent) -> Unit)? = null)
    fun destroy()
    fun startCapture()
    suspend fun evaluate(): SentlogResult
    fun cancel()
    fun getPermissions(): Permissions
    fun getCapabilities(): Capabilities
    suspend fun checkForUpdates(): UpdateResult
    fun drainQueue(path: String, drain: (String) -> Boolean): Int
    fun modelVersions(): ModelVersions
}
```

```swift
// Swift
public enum Sentlog {
    public static func initialize(config: SentlogConfig,
                                  onTelemetry: ((TelemetryEvent) -> Void)?)
    public static func destroy()
    public static func startCapture()
    public static func evaluate() async throws -> SentlogResult
    public static func cancel()
    public static func getPermissions() -> Permissions
    public static func getCapabilities() -> Capabilities
    public static func checkForUpdates() async throws -> UpdateResult
    public static func drainQueue(path: String, drain: (Envelope) -> Bool) -> Int
    public static func modelVersions() -> ModelVersions
}
```

`initialize` is idempotent: calling it again destroys the previous
handle. `destroy` is exposed explicitly for graceful teardown and test
isolation; called automatically by the next `initialize`.

`getPermissions()` returns the current permission state without
throwing. The SDK never requests permissions on the integrator's
behalf — the host app owns the prompt UX.

`getCapabilities()` returns a typed capabilities object describing
which optional modules are available in the current runtime
environment. Integrators in degraded contexts (a plain browser PWA
without a Capacitor native shell, an Android device without the right
sensors) read this to decide what features to expose rather than
catching exceptions from `evaluate()`.

### 3.2 Result type

```
SentlogResult {
    schema_version: String                // semver — bump on breaking shape change
    transcript:     Transcript
    liveness:       Liveness
    presence:       Presence
    direction:      Float
    confidence:     Float
    thresholds_met: Boolean
    engagement:     Engagement
    affect:         Affect?
    fingerprint:    AudioFingerprint
    model_versions: ModelVersions
    diagnostics:    Diagnostics
    evidence_grade: EvidenceGrade
    extensions:     Map<String, Any>      // integrator-owned passthrough; SDK never interprets
}

EvidenceGrade ∈ {
    DeviceUnattested      // default; no platform attestation
    DeviceAttested        // Play Integrity / AppAttest / DeviceCheck passed
    EnclaveSigned         // hardware-backed signing succeeded
}

Transcript {
    text:         String
    language:     String                  // detected ISO 639-1
    confidence:   Float
    word_timings: List<WordTiming>?
    target_match: Float?
}

WordTiming { word: String, start_ms: Int, end_ms: Int }

Liveness {
    score:          Float
    replay_risk:    Float
    synthetic_risk: Float
}

Presence {
    summary:      Float                   // ensemble of populated sub-fields
    audio_source: Float                   // always populated
    motion:       Float?                  // null if disabled
    integrity:    Float?                  // null if disabled
}

Engagement {
    score:        Float                   // ensemble
    voice_score:  Float
    face_score:   Float?                  // null if video disabled
    model_status: "placeholder" | "training" | "trained"
}

Affect {
    valence:       Float                  // -1.0 to 1.0
    arousal:       Float                  //  0.0 to 1.0
    dominant_fau:  List<String>?
}

AudioFingerprint {
    algo:  String                         // currently "sentlog-pcp-v1"
    bytes: ByteArray                      // base64; ~100 chars for 15 s
}

Diagnostics {
    failure_reason:    FailureReason?     // null on success
    snr_db:            Float?
    duration_ms:       Int
    audio_quality:     "quiet" | "moderate" | "noisy" | "very_noisy"
}

FailureReason ∈ {
    TooShort
    TooNoisy
    NoSpeechDetected
    LanguageMismatch
    Interrupted
    InternalError
}

UpdateResult {
    checked_at_ms:    Long
    update_found:     Boolean
    previous_version: String
    new_version:      String?
    applied:          Boolean
    error:            String?
}

ModelVersions {
    liveness:    String
    asr:         String
    prosody:     String
    vad:         String
    affect:      String?
    manifest:    String
}

Permissions {
    microphone:    "granted" | "denied" | "prompt"
    motion:        "granted" | "denied" | "prompt" | "not_requested"
    camera:        "granted" | "denied" | "prompt" | "not_requested"
}

Capabilities {
    audio_capture:          Boolean
    motion_sensors:         Boolean
    video_face_mesh:        Boolean
    device_integrity_check: Boolean
    ota_updates:            Boolean
    local_queue:            Boolean
    runtime:                "native" | "capacitor" | "react_native" | "web_fallback"
}

TelemetryEvent =
    | PipelineTiming            { stage: String, duration_ms: Int }
    | ScoreHistogram            { field: String, bucket: String }
    | ForwarderOutcome          { forwarder_name: String, outcome: "ok"|"retry"|"dropped", attempts: Int }
    | ManifestVerifyFailed      { reason: String, attempted_version: String }
    | ModelLoadError            { model: String, reason: String }
    | PermissionDenied          { permission: "microphone"|"motion"|"camera" }
    | CaptureAborted            { reason: String }
    | HardwareSigningUnavailable { reason: String, fell_back_to: "software_ed25519" }
    | TsaRequestFailed          { tsa_url: String, reason: String }

// The list above is exhaustive — no other event types are emitted.
// Every event carries: { type, timestamp_ms, session_id }. No audio,
// no transcript text, no video frames, no user identifier.
```

### 3.3 Config type

```
SentlogConfig {
    // thresholds
    liveness_threshold:    Float = 0.7
    engagement_threshold:  Float = 0.5
    direction_threshold:   Float = 0.6
    min_confidence:        Float = 0.6

    // pipeline
    challenge_enabled:     Boolean = true
    language_hint:         String = "en"
    min_duration_ms:       Int = 5000
    max_duration_ms:       Int = 60000
    target_phrase:         String? = null

    // multi-modal (all opt-in)
    use_motion_sensors:    Boolean = false
    use_video_affect:      Boolean = false
    use_device_integrity:  Boolean = false

    // scoring (advanced — defaults are the published ones)
    scoring_weights:       Map<String, Float>? = null
        // known keys: liveness.replay, liveness.synthetic, engagement.voice,
        //             engagement.face, presence.audio_source, presence.motion,
        //             presence.integrity. Unrecognised keys are rejected at init.

    // model OTA (advanced — defaults point at the published Sentlog manifest)
    manifest_url:          URL? = null
    manifest_pubkeys:      List<Ed25519PublicKey>? = null
        // overriding either requires overriding both.

    // session metadata
    session_id:               String? = null
    companion_evidence_refs:  List<String> = []

    // evidence envelope (Verifiable Credentials)
    issuer:                String                    // required when any forwarder is configured
    signing_key_id:        String? = null            // defaults to "<issuer>#sentlog-1"
    use_hardware_signing:        Boolean = false
    hardware_signing_required:   Boolean = false

    // optional RFC 3161 timestamp authority
    tsa_endpoint:    URL? = null
    tsa_timeout_ms:  Int = 5000
    tsa_required:    Boolean = false

    // integrator passthrough
    extensions_in:         Map<String, Any> = {}

    // output pipeline
    forwarders:            List<Forwarder> = []

    // observability
    on_telemetry:          ((TelemetryEvent) -> Unit)?
}
```

Every default leaves the SDK inert beyond returning a result to the
caller.

---

## 4. Composable Forwarding

### 4.1 Model

After every `evaluate()`, the SDK passes the `SentlogResult` through
any configured forwarders. Each declares: what to send (a *projection*),
where to send it (a *sink*), how often (sampling), and how to retry.

Forwarders run on the SDK's I/O thread after the result is returned to
the caller. They do not block `evaluate()`.

```
Forwarder {
    name:         String
    sink:         Sink
    projection:   Projection
    sample_rate:  Float = 1.0
    retry:        RetryPolicy = default
}

RetryPolicy {
    max_attempts:     Int = 3
    initial_delay_ms: Int = 1000
    max_delay_ms:     Int = 60000
    backoff:          "exponential" | "linear" = "exponential"
}
```

### 4.2 Sinks (v1.0)

```
Sink ∈ {
    HttpSink {
        url:              URL
        headers:          Map<String,String>?
        signing_key:      Ed25519PrivateKey?
        pin_certificates: List<Cert>?
    }

    WebhookSink {
        url:                URL
        hmac_secret:        String
        signature_header:   String = "X-Sentlog-Signature"
        signature_format:   "sha256=<hex>" | "<hex>" | "<base64>" = "sha256=<hex>"
        headers:            Map<String,String>?
    }

    LocalQueueSink {
        path:           String
        max_size_bytes: Int = 10_000_000
    }

    CallbackSink {
        callback: (Projected) -> Unit
    }
}
```

Future: `WebSocketSink`, `GrpcSink`.

### 4.3 Projections

```
Projection ∈ {
    Full                 // everything except raw audio clip
    Full+AudioClip       // everything + Opus-compressed clip
    ScoresOnly           // scores + transcript.text; no fingerprint/clip
    TranscriptOnly       // transcript + word_timings only
    EvidenceFingerprint  // scores + fingerprint; no transcript
    EvidenceClip         // scores + Opus clip; no transcript
    Custom { fields: List<String> }
}
```

The Opus clip is encoded on-demand only when a forwarder projects
`AudioClip` or `EvidenceClip`. The perceptual audio fingerprint
(currently `sentlog-pcp-v1` — a homegrown Goertzel-based temporal-
delta hash; see `shared/src/internal/fingerprint.{h,cpp}`) is
always computed (cheap) and attached to `SentlogResult`, but only
transmitted by forwarders that project it. Chromaprint was
considered and dropped: it's LGPL and would burden the SDK's
single-binary distribution story.

### 4.4 Envelope and signing

The envelope conforms to the W3C Verifiable Credentials 2.0 data
model:

```json
{
  "@context": [
    "https://www.w3.org/ns/credentials/v2",
    "https://sentlog.org/contexts/evidence/v1"
  ],
  "type": ["VerifiableCredential", "SentlogEvidence"],
  "issuer": "<integrator-supplied URI or DID>",
  "validFrom": "2026-05-22T12:34:56.789Z",
  "credentialSubject": {
    "id": "<session_id, or did:sentlog:session:<session_id>>",
    "sentlog_version":         "1.0.0",
    "envelope_format":         "sentlog-evidence/1",
    "model_versions":          { "liveness": "...", "asr": "...", "affect": "..." },
    "evidence_grade":          "DeviceUnattested",
    "companion_evidence_refs": ["<integrator opaque id>", "..."],
    "projection":              "EvidenceFingerprint",
    "payload":                 { ... }
  },
  "proof": {
    "type":               "DataIntegrityProof",
    "cryptosuite":        "eddsa-rdfc-2022",         // or "ecdsa-rdfc-2019" for hardware
    "created":            "2026-05-22T12:34:56.789Z",
    "verificationMethod": "<integrator-supplied key id>",
    "proofPurpose":       "assertionMethod",
    "proofValue":         "<base64url signature>"
  },
  "tsa_timestamp": {                                 // optional — present only when TSA configured
    "tsa_url":         "https://tsa.example.com",
    "timestamp_token": "<base64 RFC 3161 TimeStampToken>"
  }
}
```

VC-2.0-compliant out of the box. Evidence can flow into VC-aware
ecosystems (wallets, DID resolvers, presentation exchanges) without
re-wrapping. The `issuer` and `verificationMethod` are
integrator-supplied — Sentlog does not run a key infrastructure or
imply any trust hierarchy.

`session_id` is opaque pass-through metadata supplied by the
integrator and copied verbatim into every envelope. It is not an
SDK-managed session and is not persisted by the SDK.

The Ed25519/ECDSA-signed envelope is also a suitable primitive for
**on-chain attestation flows.** An integrator's oracle can hash the
envelope under a domain separator and write the digest on-chain.
Sentlog is chain-agnostic; the SDK ships the signing primitive, the
integrator chooses the chain.

### 4.5 Offline queue behaviour

Two equally valid offline patterns; integrators pick whichever fits
their existing sync architecture:

**Pattern A — `LocalQueueSink` (SDK owns persistence).** The SDK
writes projected envelopes to a file. The integrator drains the queue
on its own schedule:

```kotlin
Sentlog.drainQueue(path: String, drain: (Envelope) -> Boolean): Int
```

The closure returns `true` to delete the envelope from the queue,
`false` to leave it. No background drain runs in the SDK.

**Pattern B — `CallbackSink` (integrator owns persistence).** The SDK
hands the projected envelope to the integrator's closure synchronously;
the integrator persists it wherever its existing sync layer expects.
Best for integrators with mature offline-first architectures whose
sync layer treats Sentlog output as one queue among many.

Documentation must call out both patterns; neither is canonical —
integrator architecture decides.

---

## 5. Security and Privacy

### 5.1 Model integrity

OTA model files transit over HTTPS *and* are verified against an
Ed25519-signed manifest. Two pinned public keys ("active" + "standby"),
rotation by overlap.

### 5.2 Forwarder-driven privacy posture

No SDK-side default transmits anything. Posture is determined entirely
by the integrator's forwarder configuration:

| Integrator config | What leaves the device |
|---|---|
| `forwarders: []` | Nothing |
| `[HttpSink, EvidenceFingerprint, 15%]` | ~120 bytes 15% of the time — non-invertible perceptual hash + scores. No voice. |
| `[HttpSink, TranscriptOnly]` | Text content. Voice does not leave the device — but the text itself is potentially personal data. |
| `[HttpSink, Full]` | Scores + transcript + fingerprint, no audio clip |
| `[HttpSink, Full+AudioClip]` | Heavyweight — requires explicit DPA |

Face frames never leave the device under any projection.

### 5.3 Transcript and word timings as personal data

Any projection that includes the transcript transmits content. Under
GDPR this is potentially personal data — anything the user said. The
SDK does not gate this; the integrator opts in by configuring such a
forwarder. The integrator is the data controller and must have a DPA
with the destination.

`word_timings` does not by itself carry biometric content but carries
speech-rhythm information that, combined with text, could support
speaker profiling. Treat as personal data alongside the transcript.

### 5.4 GDPR positioning

Sentlog is a tool. The integrator's app is the data controller. The
backend the integrator names is a sub-processor. SDK documentation
states this explicitly.

### 5.5 Anti-tamper and hardware-backed signing

- Native code not obfuscated for v1.0.
- Optional app attestation via `use_device_integrity` (Play Integrity /
  AppAttest / DeviceCheck).
- Optional **hardware-backed signing** via `use_hardware_signing`:
  - **iOS:** signing key generated in the Secure Enclave on first run;
    private key never leaves the SE. Algorithm: ECDSA-P256. Proof
    cryptosuite: `ecdsa-rdfc-2019`.
  - **Android API 33+:** KeyStore-backed Ed25519 with
    `setIsStrongBoxBacked(true)` where the chipset supports it.
    Proof cryptosuite: `eddsa-rdfc-2022`.
  - **Android API 31–32:** KeyStore-backed ECDSA-P256 with StrongBox.
    Proof cryptosuite: `ecdsa-rdfc-2019`.
  - **Fallback:** when hardware-backed key creation fails (older
    devices, denied permission, hardware unavailable), the SDK falls
    back to a software Ed25519 key, emits a
    `HardwareSigningUnavailable` telemetry event, and the envelope's
    `evidence_grade` stays at `DeviceAttested` or `DeviceUnattested`
    rather than promoting to `EnclaveSigned`. Integrators that require
    enclave signing set `hardware_signing_required: true` to make
    failure abort `evaluate()`.

### 5.6 AI-coded code hardening

- ASan + UBSan on every C++ PR. MSan on nightly.
- Golden-fixture coverage (~50 clips) on every PR.
- Narrow C ABI: under ~20 functions, opaque handles only.
- ≥72h soak before each release.

### 5.7 Demographic bias acknowledgement

Every model the SDK depends on inherits the bias of its training
corpus:

- **AASIST-L** (liveness) — trained on ASVspoof challenge data,
  skewed toward studio-quality speech and a limited speaker set.
- **whisper** (ASR) — multilingual but heavily English-weighted;
  recognition varies by accent, age, and noise environment.
- **`sentlog-prosody-v1`** — a small MIT-licensed extractor over
  pitch / energy / voicing stats inspired by eGeMAPS. Heuristic
  voice_score; not yet trained on a corpus, and the eGeMAPS lineage
  it draws from is itself derived from European acted-emotion
  corpora, so prosody calibration is not culturally neutral.
- **MediaPipe Face Mesh** — face geometry estimation varies across
  skin tones and lighting conditions.

This is not unique to Sentlog — it is the state of the open ML stack
in 2026 — but the SDK's outputs cannot be presented as
demographically neutral. Two commitments:

1. Per-attack-class benchmarks published post-MVP will also break
   down by self-reported demographic segments where the labelled
   corpus supports it.
2. Integrators are documented as responsible for monitoring their own
   downstream score distributions across user segments and surfacing
   any disparate outcomes.

### 5.8 Security disclosure

`SECURITY.md` at the repo root documents the vulnerability disclosure
channel, supported versions window, and response SLA. An SDK making
anti-spoof claims will attract security research; the policy must
exist before public release readiness.

### 5.9 Integrator misuse and degraded-runtime defensive behaviour

The SDK validates configuration at `init()` and rejects malformed
configs with a clear, typed exception describing the offending field.
API calls in incorrect lifecycle order (e.g. `evaluate()` before
`init()`, `startCapture()` while another capture is active) raise a
typed exception immediately — the SDK never silently no-ops or
returns invented results.

Permission failures surface in `Diagnostics.failure_reason` rather
than the SDK requesting permissions on the integrator's behalf.
Integrators that want to render fallback UI before calling
`startCapture()` consult `Sentlog.getPermissions()` first.

Runtime degradation (missing sensor hardware, JS bridge loaded in a
context without the expected native shell, model files missing or
corrupt) surfaces via `Sentlog.getCapabilities()` as `false` on the
affected fields. The SDK never claims a capability it cannot deliver.

---

## 6. Versioning and Backwards-Compatibility Policy

### 6.1 Three independent version axes

| Axis | Where it lives | Semver rules |
|---|---|---|
| **SDK version** | Maven Central + SPM + npm packages (`1.x.y`) | Major bumps on C ABI changes or public API removals; minor on additive changes; patch for fixes. |
| **Result schema version** | `SentlogResult.schema_version` (`"1"`, `"2"`, …) | Bumped only when the shape of `SentlogResult` changes in a way integrators must adapt to. |
| **Model manifest version** | `ModelVersions.manifest` | Independent of SDK version. SDK declares the manifest-schema range it accepts. |
| **Envelope format version** | `envelope_format` field in the envelope (`"sentlog-evidence/1"`) | Bumped only when the envelope wire format changes incompatibly. |

### 6.2 What counts as breaking

**Breaking** (major bump on the affected axis):

- Removing or renaming any public API method, field, or enum case.
- Changing the semantic meaning of an existing score field.
- Changing default threshold values.
- Changing the wire format of envelope signatures.
- Reducing what `getCapabilities()` reports for an existing field.

**Non-breaking** (minor or patch):

- Adding optional fields to `SentlogResult`, `SentlogConfig`, or any
  type with optional/null support.
- Adding new sink types, projection types, or telemetry event types.
- Adding new capabilities to `getCapabilities()`.
- Performance improvements that don't change behaviour.
- Internal model improvements pushed via OTA (do not bump SDK version).

### 6.3 Deprecation policy

Deprecated public API is marked in code and docs, kept functional for
at least one major version, removed in the next major bump. Migration
notes accompany every major release.

### 6.4 Envelope wire-format stability

The signed envelope is the SDK's durable evidence artefact. It is
verified by tooling that often has only the envelope and a public key
— never the SDK source or the integrator's app. The envelope conforms
to the W3C Verifiable Credentials 2.0 data model, giving us a stable,
standards-aligned format that VC-aware ecosystems can verify directly
without Sentlog-specific tooling.

The envelope wire format is therefore a stricter contract than the
in-memory `SentlogResult` type:

- The set of *required* envelope fields (`@context`, `type`,
  `issuer`, `validFrom`, `credentialSubject`, `proof`, plus
  `envelope_format` inside `credentialSubject`) does not change
  within a major envelope-format version.
- New *optional* fields may appear in minor envelope-format bumps.
  Verifiers built against an older minor must ignore unknown fields.
- The `envelope_format` field carries the format version explicitly.
  A verifier can refuse to interpret envelopes from a format version
  it does not understand.
- Breaking changes (renaming required fields, changing signature
  scheme, changing field semantics) require an `envelope_format`
  major bump and a deliberate co-existence period during which the
  SDK can emit either format.

The reference verifier in `tools/evidence-verifier/` is the canonical
implementation of envelope verification and stays compatible with all
released envelope-format versions.
