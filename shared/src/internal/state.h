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
#include <string>

#include "internal/config.h"
#include "internal/ring_buffer.h"
#include "sentlog/sentlog.h"

namespace sentlog::internal {

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

    // --- Audio push (real-time thread; safe in parallel with anything else) ---

    sentlog_status_t push_audio(const float* samples, size_t num_samples);

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

    mutable std::mutex telemetry_mutex_;
};

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_STATE_H
