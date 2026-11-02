// Internal SDK state.
//
// One State instance per sentlog_handle_t the C ABI exposes. Owns the
// validated config, the audio ring buffer, the state machine (init →
// idle → capturing → evaluating → idle), and the telemetry callback
// the integrator wired up.
//
// All public methods are documented for the thread they expect to be
// called on. The state machine transition functions are NOT
// thread-safe, and the C ABI takes no locks: the platform bindings
// hold their own lock around lifecycle calls, and direct C callers
// must serialise them. The audio push
// path (push_audio) is callable from the producer (real-time) thread
// in parallel with everything else.

#ifndef SENTLOG_INTERNAL_STATE_H
#define SENTLOG_INTERNAL_STATE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "internal/config.h"
#include "internal/ring_buffer.h"
#include "sentlog/sentlog.h"

namespace sentlog::internal {

/// One frame of VAD output pushed by the platform-side ONNX
/// inference (Silero). Stored in order of arrival.
struct PushedVadFrame {
    size_t start_sample;
    size_t num_samples;
    float  probability;
};

/// The utterance-level liveness scoring result pushed by the
/// platform-side ONNX inference (AASIST-L). At most one per
/// evaluation.
struct PushedLiveness {
    float score;
    float replay_risk;
    float synthetic_risk;
};

/// One aligned word from platform-side ASR. Times are relative to
/// capture start.
struct PushedWordTiming {
    std::string word;
    int32_t     start_ms;
    int32_t     end_ms;
};

/// The transcript pushed by platform-side ASR (whisper.cpp). At
/// most one per evaluation. target_match is intentionally absent —
/// it's derived in the pipeline from text + config.target_phrase.
struct PushedTranscript {
    std::string                   text;
    std::string                   language;
    float                         confidence;
    std::vector<PushedWordTiming> word_timings;  // empty = unaligned
};

/// One accelerometer + gyroscope sample pushed by the platform-side
/// motion sensor. Stored in order of arrival; the pipeline computes
/// presence.motion from the buffered series at evaluate time.
struct PushedMotionFrame {
    int64_t timestamp_ns;
    float ax, ay, az;
    float gx, gy, gz;
};

/// One face-affect inference result pushed by the platform-side
/// affect head. Raw landmarks never reach the C++ core (spec §5.3);
/// only the derived valence/arousal/FAU values.
struct PushedFaceFrame {
    int64_t     timestamp_ns;
    float       valence;       // [-1, 1]
    float       arousal;       // [0, 1]
    std::string dominant_fau;  // CSV, may be empty
};

// 2^20 samples at 16 kHz = 65.536 s, the power of two that covers the
// 60 s default max_duration_ms. config.cpp rejects anything longer.
constexpr size_t kRingBufferCapacitySamples = 1048576;

// One frame per 10 ms for the longest capture. Further frames are
// dropped, like kMaxFaceFrames.
constexpr size_t kMaxVadFrames = 6554;

enum class LifecyclePhase : int {
    Idle       = 0,
    Capturing  = 1,
    Evaluating = 2,
};

class State {
public:
    // Construct from a validated config. The telemetry callback may be
    // null. ownership of telemetry_userdata stays with the integrator
    // — we just pass it back to them.
    State(Config cfg,
          sentlog_telemetry_cb_t telemetry_cb,
          void* telemetry_userdata);

    ~State() = default;

    State(const State&) = delete;
    State& operator=(const State&) = delete;

    // --- Lifecycle (serialised by the C ABI; not thread-safe with itself) ---

    sentlog_status_t start_capture();
    sentlog_status_t cancel();

    /// Run the evaluation pipeline and return a JSON-encoded
    /// SentlogResult. NOT real-time-safe — pulls from the ring buffer,
    /// runs VAD, computes diagnostics, assembles the result. Returns
    /// SENTLOG_ERR_INVALID_STATE if called outside a capture.
    sentlog_status_t evaluate(std::string* result_json_out);

    // --- Audio push (real-time thread; safe in parallel with anything else) ---

    sentlog_status_t push_audio(const float* samples, size_t num_samples);

    /// Push one VAD probability frame from platform-side ONNX
    /// inference (Silero VAD). Safe in parallel with push_audio.
    sentlog_status_t push_vad_frame(size_t start_sample,
                                    size_t num_samples,
                                    float  probability);

    /// Snapshot the pushed VAD frames for pipeline consumption.
    /// Safe to call from the evaluation thread.
    std::vector<PushedVadFrame> snapshot_vad_frames() const;

    /// Push the utterance-level liveness result from platform-side
    /// ONNX inference (AASIST-L). At most one per evaluation; later
    /// pushes overwrite earlier ones. Must be called between
    /// startCapture and evaluate (typically immediately before).
    sentlog_status_t push_liveness_result(float score,
                                          float replay_risk,
                                          float synthetic_risk);

    /// Snapshot the pushed liveness result, or std::nullopt if none.
    std::optional<PushedLiveness> snapshot_liveness() const;

    /// Parse and store a transcript JSON. Validates shape inline;
    /// returns SENTLOG_ERR_INVALID_CONFIG on malformed JSON or
    /// missing required fields. Must be called between startCapture
    /// and evaluate.
    sentlog_status_t push_transcript(const char* transcript_json);

    /// Snapshot the pushed transcript, or std::nullopt if none.
    std::optional<PushedTranscript> snapshot_transcript() const;

    /// Push one motion sample from the platform-side IMU. Safe in
    /// parallel with push_audio. Caps the buffer at the per-evaluate
    /// limit so a runaway producer can't exhaust memory.
    sentlog_status_t push_motion_frame(int64_t timestamp_ns,
                                       float ax, float ay, float az,
                                       float gx, float gy, float gz);

    /// Snapshot the pushed motion frames for pipeline consumption.
    std::vector<PushedMotionFrame> snapshot_motion_frames() const;

    /// Push one face-affect inference result from the platform-side
    /// affect head (MediaPipe / Vision). Same buffering posture as
    /// motion: drop-newest on overflow, cleared on start_capture.
    /// `dominant_fau_csv` may be null/empty.
    sentlog_status_t push_face_frame(int64_t timestamp_ns,
                                     float valence,
                                     float arousal,
                                     const char* dominant_fau_csv);

    /// Snapshot the pushed face frames for pipeline consumption.
    std::vector<PushedFaceFrame> snapshot_face_frames() const;

    // --- Phase 4d: signing + forwarder dispatch -------------------

    /// Register an Ed25519 secret key (64 bytes, RFC 8032
    /// seed||pubkey) the SDK uses to sign envelopes. The
    /// `verification_method` is copied into proof.verificationMethod.
    /// Returns INVALID_CONFIG if the length isn't 64.
    sentlog_status_t set_signing_key(const uint8_t* secret_key_bytes,
                                     size_t secret_key_len,
                                     const std::string& verification_method);

    /// True if a signing key was registered. Pipeline-side
    /// dispatcher checks this before signing the envelope.
    bool has_signing_key() const noexcept;

    /// Internal accessors for the forwarder dispatcher.
    const std::array<uint8_t, 64>& signing_key() const noexcept;
    const std::string& verification_method() const noexcept;

    /// Register the envelope dispatcher invoked once per non-
    /// local-queue forwarder.
    void set_envelope_dispatcher(sentlog_envelope_dispatch_cb_t cb,
                                 void* userdata) noexcept;

    /// Register (or clear, when cb == nullptr) the hardware signer.
    /// Returns INVALID_CONFIG if cryptosuite is unrecognised when
    /// cb != nullptr.
    sentlog_status_t set_hardware_signer(
        sentlog_hw_sign_cb_t cb,
        void* userdata,
        const std::string& cryptosuite,
        const std::string& verification_method) noexcept;

    /// Register (or clear, when cb == nullptr) the audio encoder
    /// used to populate `audio_clip.bytes` on AudioClip-bearing
    /// projections. `codec_name` is written into the envelope.
    void set_audio_encoder(sentlog_audio_encoder_cb_t cb,
                           void* userdata,
                           const std::string& codec_name) noexcept;

    /// Register (or clear, when cb == nullptr) the RFC 3161 TSA
    /// provider. The SDK calls this after envelope signing when
    /// SentlogConfig.tsa_endpoint is set.
    void set_tsa_provider(sentlog_tsa_provider_cb_t cb,
                          void* userdata) noexcept;

    /// Push (or clear, when token_bytes is empty) the most recent
    /// device-attestation token from Play Integrity / App Attest.
    /// Recorded as credentialSubject.device_attestation on every
    /// dispatched envelope; promotes evidence_grade to
    /// "DeviceAttested" (unless already EnclaveSigned).
    void set_device_attestation(const std::vector<uint8_t>& token_bytes,
                                const std::string& attestation_type) noexcept;

    /// Internal accessors for the dispatch loop.
    sentlog_audio_encoder_cb_t audio_encoder_cb() const noexcept;
    void*                      audio_encoder_userdata() const noexcept;
    std::string                audio_encoder_codec() const;

    /// Dispatch envelopes to the registered dispatcher (and write
    /// local-queue forwarders to file). Called from State::evaluate
    /// after the result JSON is assembled.
    ///
    /// `audio_snapshot` is the captured PCM (32-bit float, 16 kHz
    /// mono) snapshotted by evaluate BEFORE Pipeline::run drained
    /// the ring buffer. Used by the Phase 4g audio-encoder path on
    /// AudioClip-bearing projections; pass empty when there's no
    /// audio (silent evaluate or no encoder configured).
    void dispatch_envelopes(const std::string& result_json,
                            const std::vector<float>& audio_snapshot);

    // --- Phase 7: OTA manifest verification -----------------------

    /// Verify a fetched manifest envelope against the configured
    /// pubkeys (config override or built-in placeholders) and emit
    /// a JSON UpdateResult into *update_json_out. The returned
    /// status is OK iff verification succeeded; an error status is
    /// paired with an UpdateResult whose `error` field names the
    /// failure mode. Never blocks; never fetches.
    sentlog_status_t check_for_updates(const uint8_t* manifest_bytes,
                                       size_t manifest_len,
                                       std::string* update_json_out);

    /// Non-destructive copy of the audio ring buffer contents into
    /// `out`. Returns the number of samples written. Safe to call
    /// concurrently with push_audio.
    size_t snapshot_audio(float* out, size_t capacity) const noexcept {
        return ring_.peek(out, capacity);
    }

    /// Record the current audio input route. Pushed by the platform
    /// binding at startCapture and on route changes. Safe to call any
    /// time after construction.
    void set_audio_route(sentlog_audio_route_t route) noexcept {
        audio_route_.store(static_cast<int>(route),
                           std::memory_order_release);
    }
    sentlog_audio_route_t audio_route() const noexcept {
        return static_cast<sentlog_audio_route_t>(
            audio_route_.load(std::memory_order_acquire));
    }

    // --- Read-only state queries (any thread) ---

    LifecyclePhase phase() const noexcept {
        return static_cast<LifecyclePhase>(phase_.load(std::memory_order_acquire));
    }

    // Get the JSON representation of permission state. In Phase 1
    // returns the platform's default reading; bindings override per
    // platform in later phases.
    std::string permissions_json() const;

    // Get the JSON representation of runtime capabilities.
    std::string capabilities_json() const;

    // Get the JSON representation of currently loaded model versions.
    std::string model_versions_json() const;

    // For tests and diagnostics.
    const Config& config() const noexcept { return config_; }
    size_t audio_available() const noexcept { return ring_.available(); }
    size_t audio_overflow_count() const noexcept { return ring_.overflow_count(); }

private:
    void emit_telemetry(const std::string& event_json) noexcept;

    // Phase 7f: extract top-level scores from the pipeline result JSON
    // and emit one PipelineTiming + one ScoreHistogram per top score
    // (liveness.score, presence.summary, engagement.score). Bucketed
    // to 0.1 width so the integrator can aggregate without seeing the
    // raw scores. Telemetry remains opt-in (telemetry_cb_ may be null).
    void emit_pipeline_telemetry(const std::string& result_json,
                                 int64_t pipeline_ms) noexcept;

    Config config_;
    sentlog_telemetry_cb_t telemetry_cb_;
    void*                  telemetry_userdata_;

    std::atomic<int> phase_{static_cast<int>(LifecyclePhase::Idle)};

    RingBuffer ring_{kRingBufferCapacitySamples};

    mutable std::mutex vad_mutex_;
    std::vector<PushedVadFrame> pushed_vad_frames_;

    mutable std::mutex liveness_mutex_;
    std::optional<PushedLiveness> pushed_liveness_;

    mutable std::mutex transcript_mutex_;
    std::optional<PushedTranscript> pushed_transcript_;

    mutable std::mutex motion_mutex_;
    std::vector<PushedMotionFrame> pushed_motion_frames_;

    mutable std::mutex face_mutex_;
    std::vector<PushedFaceFrame> pushed_face_frames_;

    // Phase 4d signing + dispatch state. Lifetimes are tied to the
    // sentlog_handle; the integrator clears these by destroy().
    mutable std::mutex signing_mutex_;
    bool                                         has_signing_key_{false};
    std::array<uint8_t, 64>                      signing_key_{};
    std::string                                  verification_method_;
    sentlog_envelope_dispatch_cb_t               envelope_dispatcher_{nullptr};
    void*                                        envelope_dispatcher_userdata_{nullptr};

    // Phase 7c hardware signer. When non-null, used in preference to
    // the software signing_key_ for envelope proof generation.
    // cryptosuite_ is one of "ecdsa-jcs-2019" / "eddsa-jcs-2022".
    sentlog_hw_sign_cb_t                         hw_signer_{nullptr};
    void*                                        hw_signer_userdata_{nullptr};
    std::string                                  hw_cryptosuite_;
    std::string                                  hw_verification_method_;

    // Phase 4g audio encoder. When non-null + a projection includes
    // audio_clip, dispatch_envelopes snapshots the ring buffer and
    // calls this for the encoded bytes. codec_name is recorded in
    // the envelope's audio_clip.codec field.
    sentlog_audio_encoder_cb_t                   audio_encoder_cb_{nullptr};
    void*                                        audio_encoder_userdata_{nullptr};
    std::string                                  audio_encoder_codec_;

    // Phase 4h TSA provider. Engaged when config.tsa_endpoint is set.
    sentlog_tsa_provider_cb_t                    tsa_provider_cb_{nullptr};
    void*                                        tsa_provider_userdata_{nullptr};

    // Phase 5e device attestation. Integrator-pushed token + type;
    // attached to credentialSubject.device_attestation on each
    // dispatched envelope. Empty token = no attestation.
    std::vector<uint8_t>                         device_attestation_token_;
    std::string                                  device_attestation_type_;

    /// Default: UNKNOWN. The platform binding pushes the real route
    /// at startCapture; integrators that never push see audio_source
    /// = 0.0 in the result.
    std::atomic<int> audio_route_{
        static_cast<int>(SENTLOG_AUDIO_ROUTE_UNKNOWN)};

    mutable std::mutex telemetry_mutex_;
};

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_STATE_H
