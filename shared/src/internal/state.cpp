#include "internal/state.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <random>

#include "internal/envelope.h"
#include "internal/pipeline.h"
#include "internal/signing.h"

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
    {
        std::lock_guard<std::mutex> lock(vad_mutex_);
        pushed_vad_frames_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(liveness_mutex_);
        pushed_liveness_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        pushed_transcript_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(motion_mutex_);
        pushed_motion_frames_.clear();
    }
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
        const auto pushed_vad        = snapshot_vad_frames();
        const auto pushed_liveness   = snapshot_liveness();
        const auto pushed_transcript = snapshot_transcript();
        const auto pushed_motion     = snapshot_motion_frames();
        const auto route             = audio_route();
        *result_json_out = Pipeline::run(config_, ring_,
                                         pushed_vad, pushed_liveness,
                                         pushed_transcript, pushed_motion,
                                         route);
        // Phase 4d: dispatch envelopes for each configured forwarder
        // BEFORE the per-state cleanup below. Failures inside
        // dispatch_envelopes are absorbed (continue per forwarder)
        // so a misbehaving sink doesn't take down evaluate.
        if (!config_.forwarders.empty()) {
            dispatch_envelopes(*result_json_out);
        }
    } catch (...) {
        phase_.store(static_cast<int>(LifecyclePhase::Idle),
                     std::memory_order_release);
        return SENTLOG_ERR_INTERNAL;
    }

    ring_.reset();
    {
        std::lock_guard<std::mutex> lock(vad_mutex_);
        pushed_vad_frames_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(liveness_mutex_);
        pushed_liveness_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(transcript_mutex_);
        pushed_transcript_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(motion_mutex_);
        pushed_motion_frames_.clear();
    }
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

sentlog_status_t State::push_vad_frame(size_t start_sample,
                                       size_t num_samples,
                                       float  probability) {
    if (phase() != LifecyclePhase::Capturing) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    if (probability < 0.0f) probability = 0.0f;
    if (probability > 1.0f) probability = 1.0f;
    std::lock_guard<std::mutex> lock(vad_mutex_);
    pushed_vad_frames_.push_back({start_sample, num_samples, probability});
    return SENTLOG_OK;
}

std::vector<PushedVadFrame> State::snapshot_vad_frames() const {
    std::lock_guard<std::mutex> lock(vad_mutex_);
    return pushed_vad_frames_;
}

sentlog_status_t State::push_liveness_result(float score,
                                             float replay_risk,
                                             float synthetic_risk) {
    if (phase() != LifecyclePhase::Capturing) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    auto clamp = [](float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    };
    std::lock_guard<std::mutex> lock(liveness_mutex_);
    pushed_liveness_ = PushedLiveness{
        clamp(score), clamp(replay_risk), clamp(synthetic_risk)
    };
    return SENTLOG_OK;
}

std::optional<PushedLiveness> State::snapshot_liveness() const {
    std::lock_guard<std::mutex> lock(liveness_mutex_);
    return pushed_liveness_;
}

sentlog_status_t State::push_transcript(const char* transcript_json) {
    if (phase() != LifecyclePhase::Capturing) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    if (transcript_json == nullptr) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }

    PushedTranscript parsed;
    try {
        const auto j = json::parse(transcript_json);
        if (!j.is_object()) return SENTLOG_ERR_INVALID_CONFIG;

        // text and language are required; an empty text is allowed
        // (e.g. silence) but the keys must exist.
        if (!j.contains("text")     || !j["text"].is_string())     return SENTLOG_ERR_INVALID_CONFIG;
        if (!j.contains("language") || !j["language"].is_string()) return SENTLOG_ERR_INVALID_CONFIG;
        if (!j.contains("confidence")) return SENTLOG_ERR_INVALID_CONFIG;

        parsed.text     = j["text"].get<std::string>();
        parsed.language = j["language"].get<std::string>();

        float conf = j["confidence"].get<float>();
        if (conf < 0.0f) conf = 0.0f;
        if (conf > 1.0f) conf = 1.0f;
        parsed.confidence = conf;

        if (j.contains("word_timings") && j["word_timings"].is_array()) {
            parsed.word_timings.reserve(j["word_timings"].size());
            for (const auto& wt : j["word_timings"]) {
                if (!wt.is_object()) return SENTLOG_ERR_INVALID_CONFIG;
                if (!wt.contains("word")     || !wt["word"].is_string())  return SENTLOG_ERR_INVALID_CONFIG;
                if (!wt.contains("start_ms") || !wt["start_ms"].is_number()) return SENTLOG_ERR_INVALID_CONFIG;
                if (!wt.contains("end_ms")   || !wt["end_ms"].is_number())   return SENTLOG_ERR_INVALID_CONFIG;
                parsed.word_timings.push_back(PushedWordTiming{
                    wt["word"].get<std::string>(),
                    wt["start_ms"].get<int32_t>(),
                    wt["end_ms"].get<int32_t>(),
                });
            }
        }
    } catch (const json::exception&) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }

    std::lock_guard<std::mutex> lock(transcript_mutex_);
    pushed_transcript_ = std::move(parsed);
    return SENTLOG_OK;
}

std::optional<PushedTranscript> State::snapshot_transcript() const {
    std::lock_guard<std::mutex> lock(transcript_mutex_);
    return pushed_transcript_;
}

namespace {
// 60 s at 100 Hz upper bound — typical phone IMU is 100 Hz; max
// supported capture is 60 s. Generous headroom; bytes are tiny.
constexpr size_t kMaxMotionFrames = 6000;
}  // namespace

sentlog_status_t State::push_motion_frame(int64_t timestamp_ns,
                                          float ax, float ay, float az,
                                          float gx, float gy, float gz) {
    if (phase() != LifecyclePhase::Capturing) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(motion_mutex_);
    if (pushed_motion_frames_.size() >= kMaxMotionFrames) {
        // Drop-newest: a runaway producer can't OOM us. Same posture
        // as the audio ring buffer's drop-newest on overflow.
        return SENTLOG_OK;
    }
    pushed_motion_frames_.push_back(
        PushedMotionFrame{timestamp_ns, ax, ay, az, gx, gy, gz});
    return SENTLOG_OK;
}

std::vector<PushedMotionFrame> State::snapshot_motion_frames() const {
    std::lock_guard<std::mutex> lock(motion_mutex_);
    return pushed_motion_frames_;
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
    // motion_sensors: true iff the integrator opted in via config.
    // The actual capture happens on the platform side (Android
    // SensorManager / iOS CoreMotion); the C++ core just accepts
    // pushed frames. Phase 3c+.
    j["motion_sensors"]         = config_.use_motion_sensors;
    j["video_face_mesh"]        = false;  // platform-side; not wired yet
    j["device_integrity_check"] = false;  // platform-side; not wired yet
    // ota_updates / local_queue stay false until their real impls
    // land (Phase 7 / Phase 4). The C ABI accepts the calls today
    // but the implementations are no-op stubs — reporting true here
    // would mislead integrators reading capabilities() at startup.
    j["ota_updates"]            = false;
    j["local_queue"]            = false;
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
    j["vad"]      = "0.0.0-phase1";
    j["manifest"] = "0.0.0-phase1";
    return j.dump();
}

void State::emit_telemetry(const std::string& event_json) noexcept {
    if (telemetry_cb_ == nullptr) return;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    telemetry_cb_(event_json.c_str(), telemetry_userdata_);
}

// --- Phase 4d: signing + dispatch --------------------------------

sentlog_status_t State::set_signing_key(const uint8_t* secret_key_bytes,
                                        size_t secret_key_len,
                                        const std::string& verification_method) {
    if (secret_key_bytes == nullptr ||
        secret_key_len != kEd25519SecretKeyBytes) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    std::lock_guard<std::mutex> lock(signing_mutex_);
    std::memcpy(signing_key_.data(), secret_key_bytes, kEd25519SecretKeyBytes);
    verification_method_ = verification_method;
    has_signing_key_     = true;
    return SENTLOG_OK;
}

bool State::has_signing_key() const noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    return has_signing_key_;
}

const std::array<uint8_t, 64>& State::signing_key() const noexcept {
    // Caller must hold signing_mutex_ for the duration of any read
    // that hands the key to a signer — set_signing_key can be
    // called at any time after construction, so back-to-back
    // set + concurrent evaluate would otherwise race on
    // signing_key_. dispatch_envelopes serialises by holding the
    // lock across both has_signing_key() and the actual sign call;
    // this getter is unguarded but only used inside that locked
    // region.
    return signing_key_;
}

const std::string& State::verification_method() const noexcept {
    return verification_method_;
}

void State::set_envelope_dispatcher(sentlog_envelope_dispatch_cb_t cb,
                                    void* userdata) noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    envelope_dispatcher_         = cb;
    envelope_dispatcher_userdata_ = userdata;
}

void State::dispatch_envelopes(const std::string& result_json) {
    // Snapshot the dispatcher + signing material under the lock so
    // we don't fire against half-cleared state after a concurrent
    // destroy or set_*. Copying ~96 bytes of crypto material is
    // cheap; the alternative is holding the lock across user code
    // (the cb) which is a deadlock invitation.
    sentlog_envelope_dispatch_cb_t cb        = nullptr;
    void*                          cb_data   = nullptr;
    bool                           sign      = false;
    std::array<uint8_t, 64>        snap_key{};
    std::string                    snap_vm;
    {
        std::lock_guard<std::mutex> lock(signing_mutex_);
        cb       = envelope_dispatcher_;
        cb_data  = envelope_dispatcher_userdata_;
        sign     = has_signing_key_;
        if (sign) {
            snap_key = signing_key_;
            snap_vm  = verification_method_;
        }
    }

    // Sampling RNG (Phase 4f). One per dispatch invocation —
    // re-seeded from random_device so two evaluates on the same
    // handle don't lock-step. For tests where deterministic skip
    // matters, integrators set sample_rate to 0.0 or 1.0.
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> uniform01{0.0f, 1.0f};

    auto emit_skip = [&](const std::string& fw_name, const char* reason) {
        // Use the spec's exhaustive TelemetryEvent union (§3.2):
        // ForwarderOutcome { forwarder_name, outcome, attempts }.
        // Skip events surface as outcome="dropped" attempts=0; the
        // per-reason granularity (sampled / no_dispatcher / etc.) is
        // not yet carried in the wire shape. A future spec bump can
        // add an optional `reason` field; today this keeps the
        // binding decoders strict-by-spec.
        (void)reason;
        json ev;
        ev["type"]           = "ForwarderOutcome";
        ev["forwarder_name"] = fw_name;
        ev["outcome"]        = "dropped";
        ev["attempts"]       = 0;
        emit_telemetry(ev.dump());
    };

    const std::string valid_from = iso8601_utc_now();
    for (const auto& fw : config_.forwarders) {
        // Sampling: skip the forwarder with probability
        // (1 - sample_rate). sample_rate == 1.0 always fires;
        // sample_rate == 0.0 never fires. The skip emits a
        // telemetry event so integrators can see the rate
        // empirically.
        if (fw.sample_rate < 1.0f) {
            const float r = uniform01(rng);
            if (r >= fw.sample_rate) {
                emit_skip(fw.name, "sampled");
                continue;
            }
        }

        Projection projection = Projection::Full;
        if (!parse_projection_name(fw.projection, &projection)) {
            emit_skip(fw.name, "unknown_projection");
            continue;
        }

        ProjectionSpec spec;
        spec.kind          = projection;
        spec.custom_fields = fw.custom_fields;  // empty for non-Custom

        std::string envelope_json;
        try {
            envelope_json = build_envelope_unsigned(
                config_, result_json, valid_from, spec);
            if (sign) {
                envelope_json = sign_envelope_eddsa_jcs(
                    envelope_json, snap_key, snap_vm, valid_from);
            }
        } catch (...) {
            // Envelope construction / signing threw — surface it so
            // the integrator notices, then continue with the next
            // forwarder so one bad envelope can't take down evaluate.
            emit_skip(fw.name, "envelope_build_failed");
            continue;
        }

        if (fw.sink_kind == "local_queue") {
            // Append the envelope as one NDJSON line so
            // sentlog_drain_queue can read it back line-by-line.
            // sink_json's "path" was validated at parse_config (4e);
            // file-open failures still emit a telemetry skip event.
            try {
                const auto sj = json::parse(fw.sink_json);
                const std::string path = sj["path"].get<std::string>();
                std::ofstream out(path, std::ios::app | std::ios::binary);
                if (!out) {
                    emit_skip(fw.name, "local_queue_open_failed");
                    continue;
                }
                out << envelope_json << "\n";
            } catch (...) {
                emit_skip(fw.name, "local_queue_open_failed");
            }
            continue;
        }

        if (cb != nullptr) {
            cb(fw.name.c_str(),
               fw.sink_kind.c_str(),
               fw.sink_json.c_str(),
               envelope_json.c_str(),
               cb_data);
        } else {
            // Without a registered dispatcher, non-local-queue sinks
            // can't be delivered. Tell the integrator rather than
            // silently dropping.
            emit_skip(fw.name, "no_dispatcher_registered");
        }
    }
}

}  // namespace sentlog::internal
