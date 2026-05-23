#include "internal/state.h"

#include <nlohmann/json.hpp>

#include "internal/pipeline.h"

namespace sentlog::internal {

namespace {
using nlohmann::json;
}

State::State(Config cfg,
             sentlog_telemetry_cb_t telemetry_cb,
             void* telemetry_userdata)
    : config_(std::move(cfg)),
      telemetry_cb_(telemetry_cb),
      telemetry_userdata_(telemetry_userdata) {}

sentlog_status_t State::start_capture() {
    int expected = static_cast<int>(LifecyclePhase::Idle);
    const int desired = static_cast<int>(LifecyclePhase::Capturing);
    if (!phase_.compare_exchange_strong(expected, desired,
                                        std::memory_order_acq_rel)) {
        // start_capture called from a non-idle phase — lifecycle violation.
        return SENTLOG_ERR_INVALID_STATE;
    }
    ring_.reset();
    return SENTLOG_OK;
}

sentlog_status_t State::cancel() {
    // cancel is idempotent — any phase returns to idle.
    phase_.store(static_cast<int>(LifecyclePhase::Idle),
                 std::memory_order_release);
    ring_.reset();
    return SENTLOG_OK;
}

sentlog_status_t State::evaluate(std::string* result_json_out) {
    if (result_json_out == nullptr) return SENTLOG_ERR_INTERNAL;

    // Move from Capturing → Evaluating. Lifecycle violation if we're
    // somewhere else. cancel() puts us back to Idle on failure paths.
    int expected = static_cast<int>(LifecyclePhase::Capturing);
    const int desired = static_cast<int>(LifecyclePhase::Evaluating);
    if (!phase_.compare_exchange_strong(expected, desired,
                                        std::memory_order_acq_rel)) {
        return SENTLOG_ERR_INVALID_STATE;
    }

    try {
        *result_json_out = Pipeline::run(config_, ring_);
    } catch (...) {
        phase_.store(static_cast<int>(LifecyclePhase::Idle),
                     std::memory_order_release);
        return SENTLOG_ERR_INTERNAL;
    }

    ring_.reset();
    phase_.store(static_cast<int>(LifecyclePhase::Idle),
                 std::memory_order_release);
    return SENTLOG_OK;
}

sentlog_status_t State::push_audio(const float* samples, size_t num_samples) {
    if (phase() != LifecyclePhase::Capturing) {
        // Pushing audio outside a capture is a lifecycle violation.
        // We silently drop rather than fault the audio thread; the
        // integrator can detect this via getCapabilities / lifecycle
        // checks before pushing.
        return SENTLOG_ERR_INVALID_STATE;
    }
    ring_.write(samples, num_samples);
    return SENTLOG_OK;
}

std::string State::permissions_json() const {
    // Phase 1: the C++ core has no platform-permission visibility.
    // Bindings will fill in real values via a future C ABI call when
    // platform code lands. For now we report all "prompt" — meaning
    // the binding hasn't told us anything.
    json j;
    j["microphone"] = "prompt";
    j["motion"]     = config_.use_motion_sensors ? "prompt" : "not_requested";
    j["camera"]     = config_.use_video_affect   ? "prompt" : "not_requested";
    return j.dump();
}

std::string State::capabilities_json() const {
    // Phase 1: the C++ core reports its own intrinsic capabilities.
    // Platform-dependent capabilities (face mesh runtime, device
    // integrity, etc.) get populated by the bindings in later phases.
    json j;
    j["audio_capture"]          = true;   // the SDK can accept audio frames
    j["motion_sensors"]         = false;  // platform-side; not wired yet
    j["video_face_mesh"]        = false;  // platform-side; not wired yet
    j["device_integrity_check"] = false;  // platform-side; not wired yet
    j["ota_updates"]            = true;   // checkForUpdates implemented in Phase 7
    j["local_queue"]            = true;
    j["runtime"]                = "native";
    return j.dump();
}

std::string State::model_versions_json() const {
    // Phase 1: model loading not yet wired. Return the schema-correct
    // shape with placeholder versions so bindings can be developed
    // against a real result.
    json j;
    j["liveness"] = "0.0.0-phase1";
    j["asr"]      = "0.0.0-phase1";
    j["prosody"]  = "0.0.0-phase1";
    j["manifest"] = "0.0.0-phase1";
    return j.dump();
}

void State::emit_telemetry(const std::string& event_json) noexcept {
    if (telemetry_cb_ == nullptr) return;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    telemetry_cb_(event_json.c_str(), telemetry_userdata_);
}

}  // namespace sentlog::internal
