// Internal SDK state.
//
// One State instance per sentlog_handle_t the C ABI exposes. Owns the
// validated config, the audio ring buffer, the state machine (init →
// idle → capturing → evaluating → idle), and the telemetry callback
// the integrator wired up.
//
// All public methods are documented for the thread they expect to be
// called on. The state machine transition functions are NOT
// thread-safe; the C ABI serialises lifecycle calls. The audio push
// path (push_audio) is callable from the producer (real-time) thread
// in parallel with everything else.

#ifndef SENTLOG_INTERNAL_STATE_H
#define SENTLOG_INTERNAL_STATE_H

#include <atomic>
#include <cstddef>
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

// 30 seconds of 16 kHz audio = 480 000 samples. Round up to 524 288
// (2^19) for the power-of-two ring buffer requirement.
constexpr size_t kRingBufferCapacitySamples = 524288;

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

    /// Default: UNKNOWN. The platform binding pushes the real route
    /// at startCapture; integrators that never push see audio_source
    /// = 0.0 in the result.
    std::atomic<int> audio_route_{
        static_cast<int>(SENTLOG_AUDIO_ROUTE_UNKNOWN)};

    mutable std::mutex telemetry_mutex_;
};

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_STATE_H
