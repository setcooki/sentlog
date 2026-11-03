#include "internal/state.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

// POSIX flock so LocalQueueSink appends serialise against
// concurrent sentlog_drain_queue reads + rewrites. See the
// matching lock in c_api.cpp:sentlog_drain_queue.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>

#include "internal/envelope.h"
#include "internal/ota.h"
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
    {
        std::lock_guard<std::mutex> lock(face_mutex_);
        pushed_face_frames_.clear();
    }
    return SENTLOG_OK;
}

sentlog_status_t State::cancel() {
    // cancel is idempotent — any phase returns to idle. Emit the
    // CaptureAborted telemetry only when we actually moved out of an
    // active phase, so a defensive cancel-on-idle stays silent.
    const int prev_phase = phase_.exchange(
        static_cast<int>(LifecyclePhase::Idle),
        std::memory_order_acq_rel);
    ring_.reset();
    if (prev_phase != static_cast<int>(LifecyclePhase::Idle)) {
        json ev;
        ev["type"]   = "CaptureAborted";
        ev["reason"] = (prev_phase == static_cast<int>(LifecyclePhase::Capturing))
            ? "cancel_during_capture" : "cancel_during_evaluate";
        emit_telemetry(ev.dump());
    }
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
        const auto pushed_face       = snapshot_face_frames();
        const auto route             = audio_route();
        // Snapshot audio BEFORE Pipeline::run drains the ring (read()
        // is destructive). Cheap to do unconditionally — vector
        // allocation only matters when forwarders ask for a clip,
        // and 30 s @ 16 kHz = ~1.9 MB, allocated once per evaluate.
        std::vector<float> audio_snapshot(ring_.available());
        if (!audio_snapshot.empty()) {
            const size_t n = ring_.peek(audio_snapshot.data(),
                                        audio_snapshot.size());
            audio_snapshot.resize(n);
        }
        const auto t_pipeline_start  = std::chrono::steady_clock::now();
        *result_json_out = Pipeline::run(config_, ring_,
                                         pushed_vad, pushed_liveness,
                                         pushed_transcript, pushed_motion,
                                         pushed_face, route);
        const auto t_pipeline_end    = std::chrono::steady_clock::now();
        const int64_t pipeline_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t_pipeline_end - t_pipeline_start).count();
        // Phase 7f telemetry sweep: PipelineTiming for the evaluate
        // stage + ScoreHistogram for the three top-level scores.
        emit_pipeline_telemetry(*result_json_out, pipeline_ms);

        // Phase 4d: dispatch envelopes for each configured forwarder
        // BEFORE the per-state cleanup below. Failures inside
        // dispatch_envelopes are absorbed (continue per forwarder)
        // so a misbehaving sink doesn't take down evaluate.
        if (!config_.forwarders.empty()) {
            dispatch_envelopes(*result_json_out, audio_snapshot);
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
    {
        std::lock_guard<std::mutex> lock(face_mutex_);
        pushed_face_frames_.clear();
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
    // Enforce max_duration_ms. available() only grows during a capture
    // because nothing consumes the ring until evaluate.
    const size_t limit = static_cast<size_t>(config_.max_duration_ms) * 16;
    const size_t held  = ring_.available();
    ring_.write(samples, held < limit ? std::min(num_samples, limit - held) : 0);
    return SENTLOG_OK;
}

sentlog_status_t State::push_vad_frame(size_t start_sample,
                                       size_t num_samples,
                                       float  probability) {
    if (phase() != LifecyclePhase::Capturing) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    if (!(probability >= 0.0f)) probability = 0.0f;  // also catches NaN
    if (probability > 1.0f) probability = 1.0f;
    std::lock_guard<std::mutex> lock(vad_mutex_);
    if (pushed_vad_frames_.size() >= kMaxVadFrames) return SENTLOG_OK;
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
        if (!(v >= 0.0f)) return 0.0f;  // also catches NaN
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

namespace {
constexpr size_t kMaxFaceFrames = 6000;   // 60 s at 100 Hz upper bound
}  // namespace

sentlog_status_t State::push_face_frame(int64_t timestamp_ns,
                                        float valence,
                                        float arousal,
                                        const char* dominant_fau_csv) {
    if (phase() != LifecyclePhase::Capturing) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    auto clamp = [](float v, float lo, float hi) {
        if (!(v >= lo)) return lo;  // also catches NaN
        if (v > hi) return hi;
        return v;
    };
    std::lock_guard<std::mutex> lock(face_mutex_);
    if (pushed_face_frames_.size() >= kMaxFaceFrames) return SENTLOG_OK;
    pushed_face_frames_.push_back(PushedFaceFrame{
        timestamp_ns,
        clamp(valence, -1.0f, 1.0f),
        clamp(arousal,  0.0f, 1.0f),
        dominant_fau_csv ? std::string(dominant_fau_csv) : std::string(),
    });
    return SENTLOG_OK;
}

std::vector<PushedFaceFrame> State::snapshot_face_frames() const {
    std::lock_guard<std::mutex> lock(face_mutex_);
    return pushed_face_frames_;
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
    // Verification only works against configured keys until Sentlog
    // publishes its own (see the placeholder keys below). "native" is
    // the core's view; the JS bridges report their own runtime.
    j["ota_updates"]            = config_.manifest_pubkeys.has_value();
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
    j["vad"]      = "0.0.0-phase1";
    j["manifest"] = "0.0.0-phase1";
    return j.dump();
}

void State::emit_telemetry(const std::string& event_json) noexcept {
    if (telemetry_cb_ == nullptr) return;
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    telemetry_cb_(event_json.c_str(), telemetry_userdata_);
}

void State::emit_pipeline_telemetry(const std::string& result_json,
                                    int64_t pipeline_ms) noexcept {
    if (telemetry_cb_ == nullptr) return;
    try {
        // PipelineTiming — stage="evaluate" covers the whole
        // Pipeline::run wall-clock. Per-substage timings (vad,
        // transcript_assembly, prosody, fingerprint) can layer on top
        // in a future chunk; this is the one most integrators want.
        {
            json ev;
            ev["type"]        = "PipelineTiming";
            ev["stage"]       = "evaluate";
            ev["duration_ms"] = pipeline_ms;
            emit_telemetry(ev.dump());
        }

        // ScoreHistogram — one event per top-level score, bucketed to
        // 0.1 width. Bucket string is the lower bound: "0.0", "0.1",
        // ... "1.0". This gives integrators a histogram view without
        // ever surfacing the raw score value (no PII leak; the score
        // is non-PII but the discretisation here keeps telemetry
        // permanently coarse-grained).
        auto bucket_for = [](float v) -> std::string {
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            const int b = static_cast<int>(v * 10.0f);  // 0..10
            const float lo = static_cast<float>(b) / 10.0f;
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(lo));
            return std::string(buf);
        };
        auto emit_bucket = [&](const char* field, float v) {
            json ev;
            ev["type"]   = "ScoreHistogram";
            ev["field"]  = field;
            ev["bucket"] = bucket_for(v);
            emit_telemetry(ev.dump());
        };

        const auto r = json::parse(result_json);
        if (r.contains("liveness") && r["liveness"].contains("score") &&
            r["liveness"]["score"].is_number()) {
            emit_bucket("liveness.score",
                        r["liveness"]["score"].get<float>());
        }
        if (r.contains("presence") && r["presence"].contains("summary") &&
            r["presence"]["summary"].is_number()) {
            emit_bucket("presence.summary",
                        r["presence"]["summary"].get<float>());
        }
        if (r.contains("engagement") && r["engagement"].contains("score") &&
            r["engagement"]["score"].is_number()) {
            emit_bucket("engagement.score",
                        r["engagement"]["score"].get<float>());
        }
    } catch (...) {
        // Telemetry must never take down evaluate. Swallow any JSON
        // parse / lookup error silently.
    }
}

// --- Phase 7: OTA manifest verification --------------------------

namespace {

// Placeholder Sentlog-published manifest pubkeys baked into the build.
// Verification fails closed against these zero arrays because
// ed25519_verify_detached rejects small-order keys — Sentlog has not
// yet published its manifest signing infrastructure. Integrators that want to ship OTA today
// override `manifest_url` + `manifest_pubkeys` in SentlogConfig.
//
// When Sentlog publishes the v1.0 manifest infra, the real pubkeys
// (32 bytes each) replace these arrays here AND in the platform
// bindings' default-URL constants. The change is a coordinated
// commit, not an OTA itself.
constexpr Ed25519PublicKey kDefaultActivePubkey  = {};
constexpr Ed25519PublicKey kDefaultStandbyPubkey = {};

int64_t now_unix_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// Parses "MAJOR.MINOR.PATCH" with an optional "-prerelease". False if
// the text doesn't have that shape.
bool parse_version(const std::string& text,
                   std::array<uint64_t, 3>* numbers,
                   std::string* prerelease) {
    const size_t dash = text.find('-');
    const std::string core = text.substr(0, dash);
    *prerelease = dash == std::string::npos ? "" : text.substr(dash + 1);
    size_t pos = 0;
    for (size_t i = 0; i < numbers->size(); ++i) {
        const size_t end = core.find('.', pos);
        if ((i == 2) != (end == std::string::npos)) return false;
        const std::string part = core.substr(pos, end == std::string::npos ? end : end - pos);
        if (part.empty() || part.size() > 18 ||
            part.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        (*numbers)[i] = std::stoull(part);
        pos = end + 1;
    }
    return true;
}

// True only when `candidate` is strictly newer, so a replayed older
// manifest or an unparseable version is never offered as an update.
// Prereleases sort below their release, and otherwise by plain text.
bool is_newer_version(const std::string& candidate, const std::string& current) {
    std::array<uint64_t, 3> a{}, b{};
    std::string a_pre, b_pre;
    if (!parse_version(candidate, &a, &a_pre) || !parse_version(current, &b, &b_pre)) {
        return false;
    }
    if (a != b) return a > b;
    if (a_pre.empty() || b_pre.empty()) return a_pre.empty() && !b_pre.empty();
    return a_pre > b_pre;
}

const char* verify_error_string(ManifestVerifyStatus s) noexcept {
    switch (s) {
        case ManifestVerifyStatus::OK:                       return "ok";
        case ManifestVerifyStatus::InvalidJson:              return "invalid_json";
        case ManifestVerifyStatus::MissingFields:            return "missing_fields";
        case ManifestVerifyStatus::UnknownKeyId:             return "unknown_key_id";
        case ManifestVerifyStatus::BadSignature:             return "bad_signature";
        case ManifestVerifyStatus::UnsupportedSchemaVersion: return "unsupported_schema_version";
    }
    return "unknown";
}

}  // namespace

sentlog_status_t State::check_for_updates(const uint8_t* manifest_bytes,
                                          size_t manifest_len,
                                          std::string* update_json_out) {
    if (update_json_out == nullptr) return SENTLOG_ERR_INVALID_CONFIG;
    if (manifest_bytes == nullptr || manifest_len == 0) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }

    // Current loaded version mirrors what model_versions_json reports.
    // When real model loading lands, replace this with the loaded
    // manifest version stored on State; today it is the placeholder.
    const std::string previous_version = "0.0.0-phase1";

    const Ed25519PublicKey& active = config_.manifest_pubkeys.has_value()
        ? (*config_.manifest_pubkeys)[0] : kDefaultActivePubkey;
    const Ed25519PublicKey& standby = config_.manifest_pubkeys.has_value()
        ? (*config_.manifest_pubkeys)[1] : kDefaultStandbyPubkey;

    Manifest manifest;
    const auto vs = parse_and_verify_manifest(
        std::string_view(reinterpret_cast<const char*>(manifest_bytes),
                         manifest_len),
        active, standby, manifest);

    json out;
    out["checked_at_ms"]    = now_unix_ms();
    out["previous_version"] = previous_version;

    if (vs != ManifestVerifyStatus::OK) {
        const std::string err_str = verify_error_string(vs);
        out["update_found"] = false;
        out["applied"]      = false;
        out["error"]        = err_str;
        *update_json_out    = out.dump();
        // ManifestVerifyFailed telemetry per spec §3.2. attempted_version
        // is the claimed manifest_version, empty when the JSON shape
        // wasn't valid enough to extract it. It's unverified, so cap it.
        json ev;
        ev["type"]   = "ManifestVerifyFailed";
        ev["reason"] = err_str;
        ev["attempted_version"] = manifest.manifest_version.substr(0, 64);
        emit_telemetry(ev.dump(-1, ' ', false, json::error_handler_t::replace));
        return SENTLOG_ERR_MANIFEST_VERIFY;
    }

    const bool is_newer = is_newer_version(manifest.manifest_version, previous_version);
    out["update_found"] = is_newer;
    // `applied` stays false at this layer — the C++ core verifies the
    // manifest but does not own the bundle download or model swap.
    // The platform binding flips this when it has fetched and applied
    // the bundle (Phase 7.x bundle-apply work, not in 7b-i).
    out["applied"] = false;
    if (is_newer) {
        out["new_version"]   = manifest.manifest_version;
        out["bundle_url"]    = manifest.bundle_url;
        out["bundle_sha256"] = manifest.bundle_sha256;
        out["bundle_size"]   = manifest.bundle_size;
        out["issued_at_ms"]  = manifest.issued_at_ms;
    }
    *update_json_out = out.dump();
    return SENTLOG_OK;
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

void State::set_tsa_provider(sentlog_tsa_provider_cb_t cb,
                             void* userdata) noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    tsa_provider_cb_       = cb;
    tsa_provider_userdata_ = userdata;
}

void State::set_device_attestation(const std::vector<uint8_t>& token_bytes,
                                   const std::string& attestation_type) noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    device_attestation_token_ = token_bytes;
    device_attestation_type_  = token_bytes.empty()
        ? std::string() : attestation_type;
}

void State::set_audio_encoder(sentlog_audio_encoder_cb_t cb,
                              void* userdata,
                              const std::string& codec_name) noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    audio_encoder_cb_       = cb;
    audio_encoder_userdata_ = userdata;
    audio_encoder_codec_    = (cb != nullptr) ? codec_name : std::string();
}

sentlog_audio_encoder_cb_t State::audio_encoder_cb() const noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    return audio_encoder_cb_;
}
void* State::audio_encoder_userdata() const noexcept {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    return audio_encoder_userdata_;
}
std::string State::audio_encoder_codec() const {
    std::lock_guard<std::mutex> lock(signing_mutex_);
    return audio_encoder_codec_;
}

sentlog_status_t State::set_hardware_signer(
    sentlog_hw_sign_cb_t cb,
    void* userdata,
    const std::string& cryptosuite,
    const std::string& verification_method) noexcept {
    if (cb != nullptr) {
        if (cryptosuite != "ecdsa-jcs-2019" &&
            cryptosuite != "eddsa-jcs-2022") {
            return SENTLOG_ERR_INVALID_CONFIG;
        }
    }
    std::lock_guard<std::mutex> lock(signing_mutex_);
    hw_signer_              = cb;
    hw_signer_userdata_     = userdata;
    hw_cryptosuite_         = (cb != nullptr) ? cryptosuite : std::string();
    hw_verification_method_ = (cb != nullptr) ? verification_method : std::string();
    return SENTLOG_OK;
}

void State::dispatch_envelopes(const std::string& result_json,
                               const std::vector<float>& audio_snapshot) {
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
    sentlog_hw_sign_cb_t           hw_cb        = nullptr;
    void*                          hw_cb_data   = nullptr;
    std::string                    hw_cryptosuite;
    std::string                    hw_vm;
    {
        std::lock_guard<std::mutex> lock(signing_mutex_);
        cb       = envelope_dispatcher_;
        cb_data  = envelope_dispatcher_userdata_;
        sign     = has_signing_key_;
        if (sign) {
            snap_key = signing_key_;
            snap_vm  = verification_method_;
        }
        hw_cb          = hw_signer_;
        hw_cb_data     = hw_signer_userdata_;
        hw_cryptosuite = hw_cryptosuite_;
        hw_vm          = hw_verification_method_;
    }
    const bool try_hw = (hw_cb != nullptr) && config_.use_hardware_signing;

    // Phase 4g: pre-compute the encoded audio clip once if any
    // forwarder's projection asks for one AND an encoder is
    // registered. All forwarders in this dispatch share the same
    // ring-buffer audio, so encoding repeatedly per forwarder would
    // be pure waste.
    sentlog_audio_encoder_cb_t enc_cb       = nullptr;
    void*                      enc_userdata = nullptr;
    std::string                enc_codec;
    {
        std::lock_guard<std::mutex> lock(signing_mutex_);
        enc_cb       = audio_encoder_cb_;
        enc_userdata = audio_encoder_userdata_;
        enc_codec    = audio_encoder_codec_;
    }
    bool any_wants_clip = false;
    for (const auto& fw : config_.forwarders) {
        if (fw.projection == "Full+AudioClip" ||
            fw.projection == "EvidenceClip") {
            any_wants_clip = true; break;
        }
    }
    // Phase 5e: snapshot the device-attestation token (if any)
    // under the same lock as the rest of the signing material.
    DeviceAttestation device_att;
    const DeviceAttestation* device_att_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(signing_mutex_);
        if (!device_attestation_token_.empty()) {
            device_att.type = device_attestation_type_;
            device_att.token_b64 = base64_standard(
                device_attestation_token_.data(),
                device_attestation_token_.size());
            device_att_ptr = &device_att;
        }
    }

    EncodedAudioClip       encoded_clip;
    const EncodedAudioClip* encoded_clip_ptr = nullptr;
    if (any_wants_clip && enc_cb != nullptr && !audio_snapshot.empty()) {
        std::vector<uint8_t> encoded(256 * 1024);
        size_t out_len = encoded.size();
        const int rc = enc_cb(audio_snapshot.data(), audio_snapshot.size(),
                              16000, encoded.data(), &out_len, enc_userdata);
        if (rc == 0 && out_len > 0 && out_len <= encoded.size()) {
            encoded_clip.codec     = enc_codec;
            encoded_clip.bytes_b64 = base64_standard(encoded.data(), out_len);
            encoded_clip_ptr       = &encoded_clip;
        }
        // encoder failure → leave encoded_clip_ptr null →
        // placeholder ships. envelope build continues; the
        // forwarder still dispatches.
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
    auto emit_ok = [&](const std::string& fw_name) {
        // Phase 7f: emit ForwarderOutcome=ok on every successful
        // delivery (file write or dispatcher call). attempts=1 because
        // the SDK does not yet implement retry — the binding's HTTP /
        // webhook client owns retry on the wire.
        json ev;
        ev["type"]           = "ForwarderOutcome";
        ev["forwarder_name"] = fw_name;
        ev["outcome"]        = "ok";
        ev["attempts"]       = 1;
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

        // Phase 7c: hardware-backed signing takes priority when the
        // integrator opted in via use_hardware_signing + registered a
        // hw signer. On hw failure with hardware_signing_required=true
        // the envelope is dropped; otherwise we fall back to the
        // software Ed25519 key and emit HardwareSigningUnavailable.
        //
        // Evidence-grade promotion: on hw success the envelope's
        // subject.evidence_grade is rewritten to "EnclaveSigned"
        // BEFORE canonicalisation+signing, so the signature covers
        // the promoted value. On fallback we rebuild from result_json
        // with the original (DeviceUnattested) grade.
        std::string envelope_json;
        bool hw_signed = false;
        if (try_hw) {
            // Promote evidence_grade in a local result copy and feed
            // that into envelope build. Cheaper than mutating the
            // envelope post-build (no re-parse of the full result).
            try {
                auto promoted = json::parse(result_json);
                promoted["evidence_grade"] = "EnclaveSigned";
                const std::string promoted_str = promoted.dump();
                envelope_json = build_envelope_unsigned(
                    config_, promoted_str, valid_from, spec,
                    encoded_clip_ptr, device_att_ptr);

                auto signer = [&](std::string_view canonical)
                    -> std::vector<uint8_t> {
                    std::vector<uint8_t> sigbuf(128, 0);
                    size_t out_len = sigbuf.size();
                    const int rc = hw_cb(
                        reinterpret_cast<const uint8_t*>(canonical.data()),
                        canonical.size(),
                        sigbuf.data(), &out_len, hw_cb_data);
                    if (rc != 0 || out_len == 0 || out_len > sigbuf.size()) {
                        return {};
                    }
                    sigbuf.resize(out_len);
                    return sigbuf;
                };
                envelope_json = sign_envelope_via_hw_jcs(
                    envelope_json, signer, hw_cryptosuite, hw_vm, valid_from);
                hw_signed = true;
            } catch (...) {
                hw_signed = false;
            }
        }

        if (config_.use_hardware_signing && !hw_signed) {
            // Hardware signing failed or no signer is registered.
            // Required = drop; otherwise fall back to software signing
            // with a telemetry event so integrators can detect the
            // silent fallback.
            if (config_.hardware_signing_required) {
                emit_skip(fw.name, "hardware_signing_required_failed");
                continue;
            }
            json ev;
            ev["type"]         = "HardwareSigningUnavailable";
            ev["reason"]       = try_hw ? "signer_call_failed"
                                        : "signer_not_registered";
            ev["fell_back_to"] = sign ? "software_ed25519" : "none";
            emit_telemetry(ev.dump());
        }

        if (!hw_signed) {
            try {
                envelope_json = build_envelope_unsigned(
                    config_, result_json, valid_from, spec,
                    encoded_clip_ptr, device_att_ptr);
                if (sign) {
                    envelope_json = sign_envelope_eddsa_jcs(
                        envelope_json, snap_key, snap_vm, valid_from);
                }
            } catch (...) {
                emit_skip(fw.name, "envelope_build_failed");
                continue;
            }
        }

        // Phase 4h: TSA timestamping. Engaged when both tsa_endpoint
        // is configured AND a provider is registered. The provider
        // hashes the proof.proofValue + POSTs to the TSA + returns
        // the DER TimeStampResp bytes. We attach as a sidecar
        // `tsa_timestamp` field OUTSIDE the proof block — the
        // signature was over the envelope without TSA; verifiers
        // check proof first, then validate the TSA against the
        // proof's bytes. On failure: tsa_required = drop, else
        // continue + emit TsaRequestFailed.
        if (config_.tsa_endpoint.has_value()) {
            sentlog_tsa_provider_cb_t tsa_cb       = nullptr;
            void*                     tsa_userdata = nullptr;
            {
                std::lock_guard<std::mutex> lock(signing_mutex_);
                tsa_cb       = tsa_provider_cb_;
                tsa_userdata = tsa_provider_userdata_;
            }
            bool tsa_attached = false;
            std::string tsa_failure_reason;
            if (tsa_cb != nullptr) {
                try {
                    auto env = json::parse(envelope_json);
                    if (env.contains("proof") &&
                        env["proof"].contains("proofValue") &&
                        env["proof"]["proofValue"].is_string()) {
                        const std::string pv =
                            env["proof"]["proofValue"].get<std::string>();
                        std::vector<uint8_t> token(16 * 1024);
                        size_t token_len = token.size();
                        const int rc = tsa_cb(
                            reinterpret_cast<const uint8_t*>(pv.data()),
                            pv.size(),
                            config_.tsa_endpoint->c_str(),
                            config_.tsa_timeout_ms,
                            token.data(), &token_len, tsa_userdata);
                        if (rc == 0 && token_len > 0 &&
                            token_len <= token.size()) {
                            json ts;
                            ts["provider"] = *config_.tsa_endpoint;
                            ts["token_b64"] =
                                base64_standard(token.data(), token_len);
                            env["tsa_timestamp"] = std::move(ts);
                            envelope_json = env.dump();
                            tsa_attached = true;
                        } else {
                            tsa_failure_reason = "tsa_provider_returned_error";
                        }
                    } else {
                        tsa_failure_reason = "no_proof_to_stamp";
                    }
                } catch (...) {
                    tsa_failure_reason = "envelope_parse_failed";
                }
            } else {
                tsa_failure_reason = "no_tsa_provider_registered";
            }
            if (!tsa_attached) {
                json ev;
                ev["type"]    = "TsaRequestFailed";
                ev["tsa_url"] = *config_.tsa_endpoint;
                ev["reason"]  = tsa_failure_reason;
                emit_telemetry(ev.dump());
                if (config_.tsa_required) {
                    emit_skip(fw.name, "tsa_required_failed");
                    continue;
                }
            }
        }

        if (fw.sink_kind == "local_queue") {
            // Append the envelope as one NDJSON line so
            // sentlog_drain_queue can read it back line-by-line.
            // sink_json's "path" was validated at parse_config (4e);
            // file-open failures still emit a telemetry skip event.
            //
            // Sidecar flock (Phase 4 audit A1): we lock <path>.lock
            // — not <path> itself — so the lock's identity stays
            // stable across drain's tmp+rename. Locking the data
            // file directly would race: the writer's pre-acquired
            // fd would point at the unlinked old inode after drain
            // renamed a fresh file over it. drain takes the same
            // sidecar lock; see sentlog_drain_queue.
            try {
                const auto sj = json::parse(fw.sink_json);
                const std::string path = sj["path"].get<std::string>();
                const std::string lock_path = path + ".lock";
                const int lockfd = ::open(lock_path.c_str(),
                                          O_RDWR | O_CREAT, 0600);
                if (lockfd < 0) {
                    emit_skip(fw.name, "local_queue_open_failed");
                    continue;
                }
                if (::flock(lockfd, LOCK_EX) != 0) {
                    ::close(lockfd);
                    emit_skip(fw.name, "local_queue_open_failed");
                    continue;
                }
                // With the sidecar lock held, append to the data
                // file. std::ofstream is fine here — no other
                // process can rename while we're writing.
                bool wrote = false;
                {
                    std::ofstream out(path, std::ios::app | std::ios::binary);
                    if (out) {
                        out << envelope_json << "\n";
                        wrote = static_cast<bool>(out);
                    }
                }
                ::flock(lockfd, LOCK_UN);
                ::close(lockfd);
                if (wrote) emit_ok(fw.name);
                else       emit_skip(fw.name, "local_queue_open_failed");
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
            // The dispatcher is fire-and-forget from the C++ core's
            // perspective — the integrator's HTTP / webhook client
            // owns wire-level retry + status. We report "ok" once the
            // envelope crossed the FFI boundary; that's what the
            // shared core can verify.
            emit_ok(fw.name);
        } else {
            // Without a registered dispatcher, non-local-queue sinks
            // can't be delivered. Tell the integrator rather than
            // silently dropping.
            emit_skip(fw.name, "no_dispatcher_registered");
        }
    }
}

}  // namespace sentlog::internal
