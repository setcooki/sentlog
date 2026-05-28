/*
 * sentlog.h — Sentlog SDK C ABI
 *
 * Sentlog is a universal, device-first evidence and presence SDK. This
 * header is the only stable boundary between the C++ shared core and
 * every platform binding (Kotlin/JNI, Swift/C-interop, React Native,
 * Capacitor). Keep it narrow.
 *
 * Design constraints (see docs/technical-specification.md):
 *
 *   - The surface is around two dozen functions plus opaque handles.
 *     No exposed structs. AI-coded FFI bindings are only tractable
 *     at this size.
 *   - Result and config data crosses the boundary as JSON strings to
 *     keep the ABI flat. Binary fields inside the JSON (audio
 *     fingerprint, signed envelope proof) are base64-encoded by the
 *     core before they reach this layer.
 *   - All async operations are callback-based. Bindings translate
 *     them into native idioms: Kotlin coroutines, Swift async/await,
 *     JS Promises.
 *   - Heap-allocated strings returned by getter functions MUST be
 *     released with sentlog_string_free().
 *   - The library never requests permissions or touches the network
 *     except through callbacks the integrator wired up. See the
 *     "Inert by Default" principle in the whitepaper.
 *
 * Licence: MIT.
 */

#ifndef SENTLOG_H
#define SENTLOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENTLOG_VERSION_MAJOR  0
#define SENTLOG_VERSION_MINOR  1
#define SENTLOG_VERSION_PATCH  0
#define SENTLOG_VERSION_STRING "0.1.0"

/* --------------------------------------------------------------------
 * Error / status codes
 *
 * Every function that can fail returns sentlog_status_t. Functions that
 * cannot fail return void or a non-status value documented per function.
 * ----------------------------------------------------------------- */

typedef enum {
    SENTLOG_OK                     = 0,
    SENTLOG_ERR_INVALID_CONFIG     = 1,  /* config JSON malformed or fails validation */
    SENTLOG_ERR_INVALID_STATE      = 2,  /* lifecycle violation (e.g. evaluate before init) */
    SENTLOG_ERR_PERMISSION_DENIED  = 3,  /* required platform permission not granted */
    SENTLOG_ERR_CAPTURE_FAILED     = 4,  /* audio/video capture failed at the platform level */
    SENTLOG_ERR_MODEL_LOAD         = 5,  /* model file missing, corrupt, or version mismatch */
    SENTLOG_ERR_MANIFEST_VERIFY    = 6,  /* OTA manifest signature did not verify */
    SENTLOG_ERR_CANCELLED          = 7,  /* operation was cancelled by sentlog_cancel() */
    SENTLOG_ERR_HARDWARE_SIGNING   = 8,  /* hardware-backed signing required but unavailable */
    SENTLOG_ERR_TSA                = 9,  /* TSA required but TSA request failed */
    SENTLOG_ERR_INTERNAL           = 99  /* anything not covered above */
} sentlog_status_t;

/* --------------------------------------------------------------------
 * Opaque handle types
 *
 * Bindings never look inside these — they're just pointers. All field
 * access goes through getter functions that return JSON.
 * ----------------------------------------------------------------- */

typedef struct sentlog_handle sentlog_handle_t;

/* --------------------------------------------------------------------
 * Callback signatures
 *
 * All async results arrive here. Callbacks are invoked from the SDK's
 * I/O thread — bindings are responsible for marshalling back to the
 * platform's UI thread if needed.
 *
 * Strings passed to callbacks are owned by the SDK and valid only for
 * the duration of the call. If the binding needs them longer, copy.
 * ----------------------------------------------------------------- */

/* evaluate() completion. result_json is non-NULL only when status == OK. */
typedef void (*sentlog_result_cb_t)(
    sentlog_status_t status,
    const char* result_json,
    void* userdata);

/* checkForUpdates() completion. */
typedef void (*sentlog_update_cb_t)(
    sentlog_status_t status,
    const char* update_json,
    void* userdata);

/* Per-envelope callback during drainQueue(). Return 1 to delete the
 * envelope from the queue, 0 to leave it for a later drain. */
typedef int (*sentlog_drain_cb_t)(
    const char* envelope_json,
    size_t envelope_len,
    void* userdata);

/* Telemetry. Fires only if a callback was provided at sentlog_create().
 * event_json carries the TelemetryEvent shape defined in §3.2 of the
 * technical specification. No PII, no audio, no transcript, no frames. */
typedef void (*sentlog_telemetry_cb_t)(
    const char* event_json,
    void* userdata);

/* Per-envelope dispatch callback. Fires once per configured forwarder
 * after sentlog_evaluate completes, for any sink_kind the C++ core
 * doesn't itself dispatch (everything except "local_queue", which
 * the core writes to a file directly).
 *
 *   forwarder_name  The name from SentlogConfig.forwarders[i].name
 *                   — opaque to the SDK, integrator-supplied.
 *   sink_kind       "http" | "webhook" | "callback".
 *   sink_json       The sink-specific configuration as JSON
 *                   (URL, headers, HMAC params, etc.). Parse on the
 *                   binding side.
 *   envelope_json   The signed W3C VC 2.0 envelope, ready to ship.
 *                   Null-terminated. The integrator owns no memory;
 *                   the buffer is valid only for the duration of the
 *                   callback.
 *
 * The callback runs on the SDK's I/O thread (the same thread that
 * delivers sentlog_result_cb_t). Long-running work — HTTP POSTs,
 * etc. — should hand off to the binding's own scheduling. */
typedef void (*sentlog_envelope_dispatch_cb_t)(
    const char* forwarder_name,
    const char* sink_kind,
    const char* sink_json,
    const char* envelope_json,
    void* userdata);

/* RFC 3161 TSA-provider callback (Phase 4h). The platform binding
 * computes SHA-256 over the bytes the SDK hands it (the envelope's
 * proof.proofValue UTF-8 bytes), wraps the digest in a TimeStampReq,
 * POSTs to SentlogConfig.tsa_endpoint with Content-Type:
 * application/timestamp-query, and returns the DER-encoded
 * TimeStampResp bytes from the TSA.
 *
 * Native crypto + native HTTP both live on the binding side
 * (CryptoKit / URLSession on iOS, MessageDigest / HttpURLConnection
 * on Android) — the C++ core stays codec-free.
 *
 *   to_hash_bytes          Bytes to digest + timestamp (envelope's
 *                          proof.proofValue, base64url-encoded).
 *   to_hash_len            Length of to_hash_bytes.
 *   tsa_endpoint_url       The full TSA URL from SentlogConfig.
 *                          Passed through unchanged.
 *   timeout_ms             From SentlogConfig.tsa_timeout_ms.
 *   token_out_buf          DER-encoded TimeStampResp from the TSA.
 *                          Buffer sized 16 KiB; typical responses are
 *                          1–4 KiB.
 *   token_out_len_inout    In: capacity. Out: bytes written.
 *
 * Returns 0 on success, non-zero on fetch / TSA failure. On failure
 * the SDK emits a TsaRequestFailed telemetry event; the envelope
 * ships without the tsa_timestamp block — unless
 * SentlogConfig.tsa_required is true, in which case the forwarder
 * drops with a ForwarderOutcome=dropped telemetry. */
typedef int (*sentlog_tsa_provider_cb_t)(
    const uint8_t* to_hash_bytes,
    size_t         to_hash_len,
    const char*    tsa_endpoint_url,
    int            timeout_ms,
    uint8_t*       token_out_buf,
    size_t*        token_out_len_inout,
    void*          userdata);

/* Audio encoder callback (Phase 4g). The platform binding implements
 * this with the OS-native codec — AudioToolbox AVAudioConverter on
 * iOS, MediaCodec on Android — typically producing Opus bytes. The
 * SDK calls it once per evaluate when a configured forwarder's
 * projection includes an audio clip (Full+AudioClip or EvidenceClip),
 * snapshots the ring buffer as 32-bit float PCM at 16 kHz mono, and
 * attaches the encoded bytes to the envelope's `audio_clip` block.
 *
 *   pcm_samples_f32        Pointer to the captured PCM frame buffer
 *                          (32-bit float, mono, ring-buffer order).
 *   num_samples            Number of float samples in the buffer.
 *   sample_rate_hz         Hard-coded to 16000 today; future-proofed
 *                          by being passed explicitly so a future
 *                          variable-rate path doesn't need a new ABI.
 *   encoded_out_buf        Caller-provided buffer the cb writes the
 *                          encoded bytes into. Sized 256 KiB; far
 *                          larger than any 30s @ 32 kbps Opus
 *                          output (~ 120 KiB worst case).
 *   encoded_out_len_inout  In: capacity. Out: bytes written.
 *
 * Returns 0 on success, non-zero on encoding failure. On failure the
 * envelope's audio_clip falls back to an empty placeholder + a
 * documented note; the dispatch continues per ForwarderOutcome=ok. */
typedef int (*sentlog_audio_encoder_cb_t)(
    const float*  pcm_samples_f32,
    size_t        num_samples,
    int           sample_rate_hz,
    uint8_t*      encoded_out_buf,
    size_t*       encoded_out_len_inout,
    void*         userdata);

/* Hardware-backed signing callback (Phase 7c). The platform binding
 * implements this with whatever the OS exposes: Secure Enclave on
 * iOS (ECDSA-P256), KeyStore StrongBox on Android (Ed25519 on API
 * 33+, otherwise ECDSA-P256). The C++ core hands the canonical bytes
 * to sign and expects the raw signature back.
 *
 *   message_bytes        The JCS-canonicalised envelope bytes to sign.
 *   message_len          Length of message_bytes.
 *   sig_out_buf          Caller-provided buffer the cb writes the
 *                        raw signature into. ECDSA-P256 signatures
 *                        are ASN.1/DER and ≤72 bytes; Ed25519 are
 *                        fixed 64 bytes. Buffer is sized 128 bytes
 *                        to cover either.
 *   sig_out_len_inout    In: capacity of sig_out_buf. Out: bytes
 *                        actually written.
 *   userdata             Opaque pass-through.
 *
 * Returns 0 on success, non-zero on hardware-signing failure (key
 * unavailable, user-presence denied, etc.). On failure the C++ core
 * emits a HardwareSigningUnavailable telemetry event and falls back
 * to software signing — unless config.hardware_signing_required is
 * true, in which case envelope dispatch is skipped with a
 * ForwarderOutcome=dropped telemetry. */
typedef int (*sentlog_hw_sign_cb_t)(
    const uint8_t* message_bytes,
    size_t         message_len,
    uint8_t*       sig_out_buf,
    size_t*        sig_out_len_inout,
    void*          userdata);

/* --------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------- */

/* Get the SDK version as a static string. Always succeeds. */
const char* sentlog_version(void);

/* Create an SDK instance.
 *
 *   config_json         JSON matching the SentlogConfig schema in §3.3.
 *   telemetry_cb        Optional. May be NULL.
 *   telemetry_userdata  Opaque pointer passed back to telemetry_cb.
 *   status_out          Receives the creation status. Must be non-NULL.
 *
 * Returns a handle on success or NULL on failure (with *status_out set).
 * The handle must be released with sentlog_destroy(). */
sentlog_handle_t* sentlog_create(
    const char* config_json,
    sentlog_telemetry_cb_t telemetry_cb,
    void* telemetry_userdata,
    sentlog_status_t* status_out);

/* Destroy an SDK instance. Safe to call with NULL. */
void sentlog_destroy(sentlog_handle_t* handle);

/* --------------------------------------------------------------------
 * Capture and evaluation
 * ----------------------------------------------------------------- */

/* Begin audio capture (and motion/video capture if the relevant
 * use_* flags were set at create time). The platform binding is the
 * source of audio: it captures samples from the OS audio subsystem
 * and feeds them in via sentlog_push_audio_frame(). The C++ core
 * does not touch the audio device itself. */
sentlog_status_t sentlog_start_capture(sentlog_handle_t* handle);

/* Push one frame of captured audio into the SDK's ring buffer.
 *   samples      Pointer to PCM samples, mono, 32-bit float, [-1.0, 1.0].
 *   num_samples  Number of samples in this frame.
 *
 * Audio is expected at 16 kHz mono. Frame size is up to the binding;
 * the ring buffer accepts any size up to its capacity per call. This
 * call is on the hot path (~50 Hz at typical 20 ms frames) and is
 * non-blocking — it returns immediately and never allocates. If the
 * ring buffer is full the oldest samples are discarded silently and
 * the buffer's overflow counter increments (surfaced via telemetry). */
sentlog_status_t sentlog_push_audio_frame(
    sentlog_handle_t* handle,
    const float* samples,
    size_t num_samples);

/* Push a VAD speech-probability for a single frame, computed by the
 * platform binding's ONNX Runtime inference (Silero VAD). The
 * pipeline uses pushed probabilities when present; absent any pushes,
 * it falls back to the C++ core's built-in energy VAD.
 *
 * See ADR 0009 for the platform-side-inference architecture.
 *
 *   start_sample  First audio sample this probability covers (matches
 *                 the audio pushed via sentlog_push_audio_frame).
 *   num_samples   Number of audio samples this probability covers
 *                 (e.g. 480 for a 30 ms Silero frame at 16 kHz).
 *   probability   Speech probability in [0.0, 1.0].
 *
 * Real-time-safe: never allocates, never blocks. Safe to call from
 * the binding's inference thread while sentlog_push_audio_frame is
 * also being called from the audio thread. */
sentlog_status_t sentlog_push_vad_frame(
    sentlog_handle_t* handle,
    size_t start_sample,
    size_t num_samples,
    float probability);

/* Push the liveness scoring result from platform-side ONNX inference
 * (AASIST-L). Liveness is utterance-level, not per-frame — call once
 * per evaluate, after audio capture has stopped, before calling
 * sentlog_evaluate. The pipeline uses pushed liveness when present;
 * absent the push, the result carries a 0.5 placeholder with the
 * model_versions.liveness field reflecting that nothing real ran.
 *
 * See ADR 0009.
 *
 *   score           Overall liveness, [0.0, 1.0]. 1.0 = genuine.
 *   replay_risk     Replay-attack risk, [0.0, 1.0]. 1.0 = high risk.
 *   synthetic_risk  TTS/voice-clone risk, [0.0, 1.0]. 1.0 = high risk. */
sentlog_status_t sentlog_push_liveness_result(
    sentlog_handle_t* handle,
    float score,
    float replay_risk,
    float synthetic_risk);

/* Push one face-affect inference result from the platform-side
 * affect head (MediaPipe Face Mesh on Android, Vision face
 * landmarks on iOS). The pipeline aggregates frames across the
 * capture and surfaces the result as the `affect` block in
 * SentlogResult, plus a per-frame contribution to
 * engagement.face_score. Phase 5+.
 *
 * The platform binding runs the model at 10-15 Hz during the
 * utterance; raw landmark blobs never cross the C ABI (per spec
 * §5.3 "Face frames never leave the device under any projection")
 * — only the derived (valence, arousal, FAU) values are pushed.
 *
 *   timestamp_ns      Monotonic clock reading; relative differences
 *                     matter for ordering, not absolute values.
 *   valence           [-1.0, 1.0] negative=sad, positive=happy.
 *   arousal           [0.0, 1.0]  activation level.
 *   dominant_fau_csv  Optional CSV of Facial Action Unit names
 *                     (e.g. "AU12,AU06"). Pass NULL when the head
 *                     doesn't emit a confident FAU classification
 *                     for the frame.
 *
 * Real-time-safe: never allocates beyond a per-frame heap-string
 * copy of the CSV. Capped at 6000 frames per evaluate (60 s at
 * 100 Hz upper bound). Must be called between startCapture and
 * evaluate; lifecycle violation returns INVALID_STATE. */
sentlog_status_t sentlog_push_face_frame(
    sentlog_handle_t* handle,
    int64_t timestamp_ns,
    float valence,
    float arousal,
    const char* dominant_fau_csv);

/* Push one accelerometer + gyroscope sample from the platform-side
 * motion sensor (Android SensorManager / iOS CoreMotion). The
 * pipeline analyses the buffered samples at evaluate time and
 * surfaces a [0,1] result in `presence.motion`. Phase 3c+.
 *
 *   timestamp_ns  Monotonic clock reading; only relative differences
 *                 between samples matter to the pipeline.
 *   ax, ay, az    Accelerometer (m/s^2, includes gravity).
 *   gx, gy, gz    Gyroscope (rad/s).
 *
 * Call at the sensor's native rate (~100 Hz on most phones); the
 * core caps the per-evaluate buffer so a runaway producer won't
 * exhaust memory. Real-time-safe: never allocates, never blocks. */
sentlog_status_t sentlog_push_motion_frame(
    sentlog_handle_t* handle,
    int64_t timestamp_ns,
    float ax, float ay, float az,
    float gx, float gy, float gz);

/* Push the transcript from platform-side ASR (whisper.cpp). At most
 * one per evaluation; later pushes overwrite earlier ones. Must be
 * called between startCapture and evaluate.
 *
 * transcript_json shape:
 *   {
 *     "text":        "...",            // required, may be empty
 *     "language":    "en",             // ISO 639-1, required
 *     "confidence":  0.0..1.0,         // required
 *     "word_timings": [                // optional; omit when ASR
 *       { "word": "hello",             // doesn't surface alignments
 *         "start_ms": 100,
 *         "end_ms":   450 },
 *       ...
 *     ]
 *   }
 *
 * The pipeline does not compute target_match here — it derives that
 * later from the pushed text and config.target_phrase. Pushing a
 * target_match field is ignored.
 *
 * See ADR 0009. */
sentlog_status_t sentlog_push_transcript(
    sentlog_handle_t* handle,
    const char* transcript_json);

/* Audio input route the platform-side capture is using. The C++ core
 * cannot detect this itself — it only sees the float samples — so the
 * binding pushes the route via sentlog_set_audio_route. The pipeline
 * maps each route to a presence.audio_source probability per the
 * defaults in docs/whitepaper.md §6.1:
 *   - SENTLOG_AUDIO_ROUTE_BUILT_IN:   audio_source = 1.0
 *   - SENTLOG_AUDIO_ROUTE_USB:        audio_source = 0.2
 *   - SENTLOG_AUDIO_ROUTE_BLUETOOTH:  audio_source = 0.2
 *   - SENTLOG_AUDIO_ROUTE_LOOPBACK:   audio_source = 0.0
 *   - SENTLOG_AUDIO_ROUTE_UNKNOWN:    audio_source = 0.0
 * Integrators can override the per-route mapping via scoring_weights
 * (presence.audio_source.* keys; future, not v1.0). */
typedef enum {
    SENTLOG_AUDIO_ROUTE_UNKNOWN    = 0,
    SENTLOG_AUDIO_ROUTE_BUILT_IN   = 1,
    SENTLOG_AUDIO_ROUTE_USB        = 2,
    SENTLOG_AUDIO_ROUTE_BLUETOOTH  = 3,
    SENTLOG_AUDIO_ROUTE_LOOPBACK   = 4,
} sentlog_audio_route_t;

/* Set the audio input route the platform binding is using. Call at
 * start_capture and again if the route changes mid-capture (rare on
 * iOS; possible on Android when a headset is plugged in mid-session). */
sentlog_status_t sentlog_set_audio_route(
    sentlog_handle_t* handle,
    sentlog_audio_route_t route);

/* Non-destructively read the captured audio out of the ring buffer.
 * Used by the platform binding to run utterance-level inference
 * (e.g. AASIST-L liveness) without consuming the audio — the pipeline
 * still drains it at sentlog_evaluate time.
 *
 *   out_samples   Caller-supplied buffer. Must hold at least
 *                 out_capacity floats.
 *   out_capacity  Number of samples the buffer can hold.
 *   out_written   Out-param: actual number of samples written
 *                 (min of out_capacity and available samples).
 *
 * Safe to call from the binding's inference thread; the read is
 * lock-free and concurrent with sentlog_push_audio_frame on the
 * audio thread. */
sentlog_status_t sentlog_snapshot_audio(
    sentlog_handle_t* handle,
    float* out_samples,
    size_t out_capacity,
    size_t* out_written);

/* Stop capture and run evaluation. Result arrives via cb on the I/O thread. */
sentlog_status_t sentlog_evaluate(
    sentlog_handle_t* handle,
    sentlog_result_cb_t cb,
    void* userdata);

/* Cancel an in-progress capture or evaluation. If an evaluate() is
 * pending, its callback fires with SENTLOG_ERR_CANCELLED. */
void sentlog_cancel(sentlog_handle_t* handle);

/* --------------------------------------------------------------------
 * Read-only state queries
 *
 * All return heap-allocated JSON owned by the caller. Free with
 * sentlog_string_free(). Return NULL on internal failure.
 * ----------------------------------------------------------------- */

/* Permission state for microphone / motion / camera. Schema: §3.2. */
char* sentlog_get_permissions(sentlog_handle_t* handle);

/* Capability detection for the current runtime. Schema: §3.2. */
char* sentlog_get_capabilities(sentlog_handle_t* handle);

/* Versions of loaded models and the OTA manifest. Schema: §3.2. */
char* sentlog_model_versions(sentlog_handle_t* handle);

/* --------------------------------------------------------------------
 * Forwarder dispatch (Phase 4)
 * ----------------------------------------------------------------- */

/* Register the integrator-supplied Ed25519 signing key the SDK uses
 * to populate the W3C VC 2.0 envelope's `proof` block. The key is
 * the 64-byte RFC 8032 seed||pubkey concatenation; the binding
 * obtains this from libsodium / NaCl / a hardware-backed keystore.
 *
 *   verification_method  String written into proof.verificationMethod
 *                        (typically a `did:key:` URI the integrator's
 *                        verifier resolves to the public key).
 *
 * Without a registered key, envelopes are emitted unsigned (no proof
 * block). Hardware-backed signing lands in Phase 7 alongside
 * ecdsa-jcs-2019. */
sentlog_status_t sentlog_set_signing_key(
    sentlog_handle_t* handle,
    const uint8_t* secret_key_bytes,
    size_t          secret_key_len,
    const char*     verification_method);

/* Register the per-handle envelope dispatcher. Fires once per
 * configured forwarder after evaluate, EXCEPT for forwarders with
 * sink_kind = "local_queue" which the C++ core writes to a file
 * itself (Phase 4d2). Without a registered dispatcher, http /
 * webhook / callback forwarders are silently skipped — the binding
 * is responsible for the actual network or in-process delivery. */
sentlog_status_t sentlog_set_envelope_dispatcher(
    sentlog_handle_t* handle,
    sentlog_envelope_dispatch_cb_t cb,
    void* userdata);

/* Register the per-handle hardware-signer callback (Phase 7c). The
 * binding implements `cb` using the device's secure element —
 * Secure Enclave on iOS, KeyStore/StrongBox on Android — and the
 * SDK calls it to sign envelopes in place of the software Ed25519
 * key registered via sentlog_set_signing_key.
 *
 *   cryptosuite           The W3C Data Integrity cryptosuite the
 *                         signature realises. One of:
 *                           "ecdsa-jcs-2019"   (Secure Enclave;
 *                                              Android API 31-32)
 *                           "eddsa-jcs-2022"   (Android API 33+
 *                                              KeyStore Ed25519)
 *   verification_method   Same role as in sentlog_set_signing_key:
 *                         copied into proof.verificationMethod, the
 *                         verifier resolves it to the public key.
 *
 * Pass `cb = NULL` to clear a previously registered hw signer.
 *
 * Selection at dispatch time:
 *   - hw signer registered + use_hardware_signing=true   → hw signs;
 *     envelope.evidence_grade is promoted to "EnclaveSigned".
 *   - hw signer absent OR cb returns non-zero, AND
 *     hardware_signing_required=false                    → fall back
 *     to the software Ed25519 key (if any); a
 *     HardwareSigningUnavailable telemetry event fires.
 *   - hw signer absent OR cb returns non-zero, AND
 *     hardware_signing_required=true                     → the
 *     envelope is not dispatched; a ForwarderOutcome=dropped
 *     telemetry event fires per affected forwarder. */
sentlog_status_t sentlog_set_hardware_signer(
    sentlog_handle_t*    handle,
    sentlog_hw_sign_cb_t cb,
    void*                userdata,
    const char*          cryptosuite,
    const char*          verification_method);

/* Register (or clear, when cb == NULL) the per-handle audio encoder
 * used to populate `audio_clip.bytes` on AudioClip-bearing
 * projections. Without a registered encoder the envelope ships the
 * placeholder block (codec="opus", bytes="") + a note documenting
 * the unencoded fall-back. `codec_name` is recorded in the envelope
 * (`audio_clip.codec`); must match what the cb actually produces. */
sentlog_status_t sentlog_set_audio_encoder(
    sentlog_handle_t*          handle,
    sentlog_audio_encoder_cb_t cb,
    void*                      userdata,
    const char*                codec_name);

/* Register (or clear, when cb == NULL) the per-handle RFC 3161 TSA
 * provider. Engaged only when SentlogConfig.tsa_endpoint is set; the
 * `tsa_endpoint` URL is opaque to the C++ core — the binding owns
 * the HTTPS POST. Without a registered provider, the SDK falls back
 * to no-TSA mode (TsaRequestFailed telemetry on every evaluate the
 * config asked for TSA; ForwarderOutcome=dropped when tsa_required). */
sentlog_status_t sentlog_set_tsa_provider(
    sentlog_handle_t*         handle,
    sentlog_tsa_provider_cb_t cb,
    void*                     userdata);

/* Phase 5e: Push the most recent device-attestation token the
 * integrator obtained via Play Integrity (Android) / App Attest
 * (iOS). The SDK records the token + type as
 * credentialSubject.device_attestation on every dispatched envelope
 * and promotes evidence_grade to "DeviceAttested" (unless already
 * "EnclaveSigned", which is higher).
 *
 *   token_bytes / token_len   The opaque attestation token. The SDK
 *                             treats this as bytes — verifiers
 *                             validate against Google's / Apple's
 *                             attestation roots out of band.
 *   attestation_type          Free-form tag ("play_integrity",
 *                             "app_attest", "device_check", etc.).
 *                             Copied into the envelope.
 *
 * Pass token_bytes=NULL / token_len=0 to clear a previously-pushed
 * attestation (e.g. on user logout). Attestation lifetime is
 * integrator-managed — the SDK never times tokens out itself; if
 * yours expire, push a fresh one before the next evaluate. */
sentlog_status_t sentlog_set_device_attestation(
    sentlog_handle_t* handle,
    const uint8_t*    token_bytes,
    size_t            token_len,
    const char*       attestation_type);

/* --------------------------------------------------------------------
 * OTA
 * ----------------------------------------------------------------- */

/* Verify a fetched OTA manifest envelope and report the result.
 *
 * The HTTPS fetch lives on the platform binding (Kotlin/Swift) — the
 * C++ core is fed the manifest bytes here and verifies the Ed25519
 * signature against the two pinned pubkeys (active + standby), as
 * configured via `SentlogConfig.manifest_pubkeys` or — when absent —
 * the platform binding's compiled-in Sentlog defaults.
 *
 * Result is delivered through `cb`. On verification failure the
 * callback fires with status == SENTLOG_ERR_MANIFEST_VERIFY and an
 * update_json payload whose `error` field names the failure mode;
 * on success status == SENTLOG_OK and update_json reports whether the
 * manifest's version is newer than the loaded one. Bundle download +
 * apply is a separate concern handled by the platform binding when
 * `update_found` flips to true; the C++ core never blocks on it.
 *
 * Never autonomous — only fires when called. */
sentlog_status_t sentlog_check_for_updates(
    sentlog_handle_t* handle,
    const uint8_t*    manifest_bytes,
    size_t            manifest_len,
    sentlog_update_cb_t cb,
    void* userdata);

/* --------------------------------------------------------------------
 * Local queue (offline forwarding)
 * ----------------------------------------------------------------- */

/* Drain the local envelope queue at the given path. Each envelope is
 * delivered to cb; the callback decides whether to delete it (1) or
 * keep it (0). Returns the number of envelopes delivered. */
int sentlog_drain_queue(
    sentlog_handle_t* handle,
    const char* path,
    sentlog_drain_cb_t cb,
    void* userdata);

/* --------------------------------------------------------------------
 * Memory management
 * ----------------------------------------------------------------- */

/* Free a heap-allocated string returned by any sentlog_get_* or
 * sentlog_model_versions function. Safe to call with NULL. */
void sentlog_string_free(char* str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SENTLOG_H */
