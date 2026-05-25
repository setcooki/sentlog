#include "internal/config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace sentlog::internal {

namespace {

using nlohmann::json;

constexpr float  kDefaultLivenessThreshold   = 0.7f;
constexpr float  kDefaultEngagementThreshold = 0.5f;
constexpr float  kDefaultDirectionThreshold  = 0.6f;
constexpr float  kDefaultMinConfidence       = 0.6f;
constexpr int    kDefaultMinDurationMs       = 5000;
constexpr int    kDefaultMaxDurationMs       = 60000;
constexpr int    kDefaultTsaTimeoutMs        = 5000;

const std::set<std::string> kAllowedProjections = {
    "Full", "Full+AudioClip", "ScoresOnly", "TranscriptOnly",
    "EvidenceFingerprint", "EvidenceClip", "Custom",
};

const std::set<std::string> kAllowedSinkKinds = {
    "http", "webhook", "local_queue", "callback",
};

// Helper: fail with the offending dotted path.
bool fail(std::string* error_field, const std::string& path) {
    if (error_field != nullptr) {
        *error_field = path;
    }
    return false;
}

bool read_threshold(const json& j, const std::string& key, float& out,
                    float default_value, std::string* error_field,
                    const std::string& path_prefix) {
    if (!j.contains(key)) {
        out = default_value;
        return true;
    }
    if (!j[key].is_number()) {
        return fail(error_field, path_prefix + key);
    }
    out = j[key].get<float>();
    if (out < 0.0f || out > 1.0f) {
        return fail(error_field, path_prefix + key);
    }
    return true;
}

bool read_bool(const json& j, const std::string& key, bool& out,
               bool default_value, std::string* error_field,
               const std::string& path_prefix) {
    if (!j.contains(key)) {
        out = default_value;
        return true;
    }
    if (!j[key].is_boolean()) {
        return fail(error_field, path_prefix + key);
    }
    out = j[key].get<bool>();
    return true;
}

bool read_int_positive(const json& j, const std::string& key, int& out,
                       int default_value, std::string* error_field,
                       const std::string& path_prefix) {
    if (!j.contains(key)) {
        out = default_value;
        return true;
    }
    if (!j[key].is_number_integer()) {
        return fail(error_field, path_prefix + key);
    }
    out = j[key].get<int>();
    if (out <= 0) {
        return fail(error_field, path_prefix + key);
    }
    return true;
}

bool read_string(const json& j, const std::string& key, std::string& out,
                 const std::string& default_value, std::string* error_field,
                 const std::string& path_prefix) {
    if (!j.contains(key)) {
        out = default_value;
        return true;
    }
    if (!j[key].is_string()) {
        return fail(error_field, path_prefix + key);
    }
    out = j[key].get<std::string>();
    return true;
}

bool read_optional_string(const json& j, const std::string& key,
                          std::optional<std::string>& out,
                          std::string* error_field,
                          const std::string& path_prefix) {
    if (!j.contains(key) || j[key].is_null()) {
        out.reset();
        return true;
    }
    if (!j[key].is_string()) {
        return fail(error_field, path_prefix + key);
    }
    out = j[key].get<std::string>();
    return true;
}

bool parse_forwarder(const json& j, Forwarder& out,
                     std::string* error_field, const std::string& path) {
    if (!j.is_object()) return fail(error_field, path);
    if (!j.contains("name") || !j["name"].is_string()) {
        return fail(error_field, path + ".name");
    }
    out.name = j["name"].get<std::string>();
    if (out.name.empty()) {
        return fail(error_field, path + ".name");
    }

    if (!j.contains("projection") || !j["projection"].is_string()) {
        return fail(error_field, path + ".projection");
    }
    out.projection = j["projection"].get<std::string>();
    if (kAllowedProjections.find(out.projection) == kAllowedProjections.end()) {
        return fail(error_field, path + ".projection");
    }

    // Phase 4f: Custom projection accepts a custom_fields array of
    // dotted paths into SentlogResult. Required for Custom; rejected
    // (loudly) for any other projection so typos surface at init.
    if (out.projection == "Custom") {
        if (!j.contains("custom_fields") || !j["custom_fields"].is_array()) {
            return fail(error_field, path + ".custom_fields");
        }
        for (size_t i = 0; i < j["custom_fields"].size(); ++i) {
            const auto& f = j["custom_fields"][i];
            if (!f.is_string() || f.get<std::string>().empty()) {
                if (error_field != nullptr) {
                    *error_field = path + ".custom_fields[" +
                                   std::to_string(i) + "]";
                }
                return false;
            }
            out.custom_fields.push_back(f.get<std::string>());
        }
        if (out.custom_fields.empty()) {
            return fail(error_field, path + ".custom_fields");
        }
    } else if (j.contains("custom_fields")) {
        return fail(error_field, path + ".custom_fields");
    }

    if (j.contains("sample_rate")) {
        if (!j["sample_rate"].is_number()) {
            return fail(error_field, path + ".sample_rate");
        }
        out.sample_rate = j["sample_rate"].get<float>();
        if (out.sample_rate < 0.0f || out.sample_rate > 1.0f) {
            return fail(error_field, path + ".sample_rate");
        }
    } else {
        out.sample_rate = 1.0f;
    }

    if (!j.contains("sink") || !j["sink"].is_object()) {
        return fail(error_field, path + ".sink");
    }
    const json& sink = j["sink"];
    if (!sink.contains("kind") || !sink["kind"].is_string()) {
        return fail(error_field, path + ".sink.kind");
    }
    out.sink_kind = sink["kind"].get<std::string>();
    if (kAllowedSinkKinds.find(out.sink_kind) == kAllowedSinkKinds.end()) {
        return fail(error_field, path + ".sink.kind");
    }

    // Phase 4e: per-sink_kind shape validation. The binding side is
    // what actually POSTs / writes, but we catch missing required
    // fields at init so the integrator gets a typed error rather
    // than a silent runtime no-op.
    if (out.sink_kind == "http" || out.sink_kind == "webhook") {
        if (!sink.contains("url") || !sink["url"].is_string() ||
            sink["url"].get<std::string>().empty()) {
            return fail(error_field, path + ".sink.url");
        }
        if (out.sink_kind == "webhook") {
            if (!sink.contains("hmac_secret") ||
                !sink["hmac_secret"].is_string() ||
                sink["hmac_secret"].get<std::string>().empty()) {
                return fail(error_field, path + ".sink.hmac_secret");
            }
        }
    } else if (out.sink_kind == "local_queue") {
        if (!sink.contains("path") || !sink["path"].is_string() ||
            sink["path"].get<std::string>().empty()) {
            return fail(error_field, path + ".sink.path");
        }
    }
    // callback: no required sink-side fields — the binding's
    // registered envelope dispatcher does the work.

    out.sink_json = sink.dump();
    return true;
}

}  // namespace

sentlog_status_t parse_config(const std::string& json_text,
                              Config* out,
                              std::string* error_field) {
    if (out == nullptr) return SENTLOG_ERR_INTERNAL;

    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::parse_error&) {
        if (error_field != nullptr) *error_field = "<root>";
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    if (!j.is_object()) {
        if (error_field != nullptr) *error_field = "<root>";
        return SENTLOG_ERR_INVALID_CONFIG;
    }

    Config cfg;
    const std::string root = "";

    bool ok = true;
    ok &= read_threshold(j, "liveness_threshold",   cfg.liveness_threshold,   kDefaultLivenessThreshold,   error_field, root);
    ok &= read_threshold(j, "engagement_threshold", cfg.engagement_threshold, kDefaultEngagementThreshold, error_field, root);
    ok &= read_threshold(j, "direction_threshold",  cfg.direction_threshold,  kDefaultDirectionThreshold,  error_field, root);
    ok &= read_threshold(j, "min_confidence",       cfg.min_confidence,       kDefaultMinConfidence,       error_field, root);

    ok &= read_bool(j, "challenge_enabled",    cfg.challenge_enabled,    true,  error_field, root);
    ok &= read_string(j, "language_hint",      cfg.language_hint,        "en",  error_field, root);
    ok &= read_int_positive(j, "min_duration_ms", cfg.min_duration_ms, kDefaultMinDurationMs, error_field, root);
    ok &= read_int_positive(j, "max_duration_ms", cfg.max_duration_ms, kDefaultMaxDurationMs, error_field, root);
    ok &= read_optional_string(j, "target_phrase", cfg.target_phrase, error_field, root);

    ok &= read_bool(j, "use_motion_sensors",   cfg.use_motion_sensors,   false, error_field, root);
    ok &= read_bool(j, "use_video_affect",     cfg.use_video_affect,     false, error_field, root);
    ok &= read_bool(j, "use_device_integrity", cfg.use_device_integrity, false, error_field, root);

    ok &= read_string(j, "issuer", cfg.issuer, "", error_field, root);
    ok &= read_optional_string(j, "signing_key_id", cfg.signing_key_id, error_field, root);
    ok &= read_bool(j, "use_hardware_signing",      cfg.use_hardware_signing,      false, error_field, root);
    ok &= read_bool(j, "hardware_signing_required", cfg.hardware_signing_required, false, error_field, root);

    ok &= read_optional_string(j, "tsa_endpoint", cfg.tsa_endpoint, error_field, root);
    ok &= read_int_positive(j, "tsa_timeout_ms", cfg.tsa_timeout_ms, kDefaultTsaTimeoutMs, error_field, root);
    ok &= read_bool(j, "tsa_required", cfg.tsa_required, false, error_field, root);

    ok &= read_optional_string(j, "session_id", cfg.session_id, error_field, root);

    if (j.contains("companion_evidence_refs")) {
        if (!j["companion_evidence_refs"].is_array()) {
            return fail(error_field, "companion_evidence_refs") ? SENTLOG_ERR_INVALID_CONFIG : SENTLOG_ERR_INVALID_CONFIG;
        }
        for (size_t i = 0; i < j["companion_evidence_refs"].size(); ++i) {
            const auto& item = j["companion_evidence_refs"][i];
            if (!item.is_string()) {
                if (error_field != nullptr) {
                    *error_field = "companion_evidence_refs[" + std::to_string(i) + "]";
                }
                return SENTLOG_ERR_INVALID_CONFIG;
            }
            cfg.companion_evidence_refs.push_back(item.get<std::string>());
        }
    }

    if (j.contains("scoring_weights")) {
        const auto& sw = j["scoring_weights"];
        if (!sw.is_object()) {
            if (error_field != nullptr) *error_field = "scoring_weights";
            return SENTLOG_ERR_INVALID_CONFIG;
        }
        const auto& allowed = scoring_weight_keys();
        for (auto it = sw.begin(); it != sw.end(); ++it) {
            const std::string& key = it.key();
            // Named-key validation per decision row 1.21 in the spec —
            // unrecognised keys are rejected so typos surface at init
            // rather than silently no-op'ing in scoring.
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
                if (error_field != nullptr) {
                    *error_field = "scoring_weights." + key;
                }
                return SENTLOG_ERR_INVALID_CONFIG;
            }
            if (!it.value().is_number()) {
                if (error_field != nullptr) {
                    *error_field = "scoring_weights." + key;
                }
                return SENTLOG_ERR_INVALID_CONFIG;
            }
            const float v = it.value().get<float>();
            if (!(v >= 0.0f) || v > 10.0f) {
                // Weights must be non-negative; cap at 10 to catch
                // accidental ×100 typos.
                if (error_field != nullptr) {
                    *error_field = "scoring_weights." + key;
                }
                return SENTLOG_ERR_INVALID_CONFIG;
            }
            cfg.scoring_weights.emplace(key, v);
        }
    }

    if (j.contains("forwarders")) {
        if (!j["forwarders"].is_array()) {
            if (error_field != nullptr) *error_field = "forwarders";
            return SENTLOG_ERR_INVALID_CONFIG;
        }
        for (size_t i = 0; i < j["forwarders"].size(); ++i) {
            Forwarder fw;
            const std::string path = "forwarders[" + std::to_string(i) + "]";
            if (!parse_forwarder(j["forwarders"][i], fw, error_field, path)) {
                return SENTLOG_ERR_INVALID_CONFIG;
            }
            cfg.forwarders.push_back(std::move(fw));
        }
    }

    if (!ok) return SENTLOG_ERR_INVALID_CONFIG;

    // Cross-field validation.
    if (cfg.min_duration_ms >= cfg.max_duration_ms) {
        if (error_field != nullptr) *error_field = "max_duration_ms";
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    if (!cfg.forwarders.empty() && cfg.issuer.empty()) {
        if (error_field != nullptr) *error_field = "issuer";
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    if (cfg.tsa_required && !cfg.tsa_endpoint.has_value()) {
        if (error_field != nullptr) *error_field = "tsa_endpoint";
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    if (cfg.hardware_signing_required && !cfg.use_hardware_signing) {
        if (error_field != nullptr) *error_field = "use_hardware_signing";
        return SENTLOG_ERR_INVALID_CONFIG;
    }

    *out = std::move(cfg);
    return SENTLOG_OK;
}

std::string config_to_json(const Config& cfg) {
    json j;
    j["liveness_threshold"]    = cfg.liveness_threshold;
    j["engagement_threshold"]  = cfg.engagement_threshold;
    j["direction_threshold"]   = cfg.direction_threshold;
    j["min_confidence"]        = cfg.min_confidence;
    j["challenge_enabled"]     = cfg.challenge_enabled;
    j["language_hint"]         = cfg.language_hint;
    j["min_duration_ms"]       = cfg.min_duration_ms;
    j["max_duration_ms"]       = cfg.max_duration_ms;
    if (cfg.target_phrase) j["target_phrase"] = *cfg.target_phrase;
    j["use_motion_sensors"]    = cfg.use_motion_sensors;
    j["use_video_affect"]      = cfg.use_video_affect;
    j["use_device_integrity"]  = cfg.use_device_integrity;
    j["issuer"]                = cfg.issuer;
    if (cfg.signing_key_id) j["signing_key_id"] = *cfg.signing_key_id;
    j["use_hardware_signing"]        = cfg.use_hardware_signing;
    j["hardware_signing_required"]   = cfg.hardware_signing_required;
    if (cfg.tsa_endpoint) j["tsa_endpoint"] = *cfg.tsa_endpoint;
    j["tsa_timeout_ms"] = cfg.tsa_timeout_ms;
    j["tsa_required"]   = cfg.tsa_required;
    if (cfg.session_id) j["session_id"] = *cfg.session_id;
    j["companion_evidence_refs"] = cfg.companion_evidence_refs;
    j["forwarders"] = json::array();
    for (const auto& fw : cfg.forwarders) {
        json fj;
        fj["name"]        = fw.name;
        fj["projection"]  = fw.projection;
        fj["sample_rate"] = fw.sample_rate;
        fj["sink"]        = json::parse(fw.sink_json);
        j["forwarders"].push_back(std::move(fj));
    }
    if (!cfg.scoring_weights.empty()) {
        json sw = json::object();
        for (const auto& [k, v] : cfg.scoring_weights) sw[k] = v;
        j["scoring_weights"] = sw;
    }
    return j.dump();
}

// Published default scoring weights. Sources for the choices:
//
//   liveness.replay / liveness.synthetic
//     Both 1.0 — AASIST-L's per-attack-class probabilities are
//     calibrated and surface as-is in the result. A future split
//     between replay and synthetic risk would land here.
//
//   engagement.voice
//     1.0 — the only signal source today (Phase 3+ adds prosody
//     + whisper-encoder probe; weight stays 1.0 because they're
//     ensembled inside the voice_score, not here).
//
//   engagement.face
//     0.5 — when video affect ships in Phase 5, weight it at half
//     voice initially. Face is a real signal but noisier across
//     lighting / camera angle than voice. Integrators with reliable
//     capture environments can crank to 1.0.
//
//   presence.audio_source
//     1.0 — the most direct signal (microphone vs USB vs loopback).
//
//   presence.motion
//     0.7 — motion coherence (Phase 3c) is informative but more
//     easily spoofed (record-and-shake) than the audio route, so
//     it gets less weight by default.
//
//   presence.integrity
//     1.0 — Play Integrity / DeviceCheck attestations, when
//     present, are very high-signal. Phase 7+.
//
// If you bump any of these, also update docs/scoring-weights.md so
// the published table doesn't drift from the implementation.
namespace {
const std::map<std::string, float>& kPublishedWeights() {
    static const std::map<std::string, float> kv = {
        {"liveness.replay",         1.0f},
        {"liveness.synthetic",      1.0f},
        {"engagement.voice",        1.0f},
        {"engagement.face",         0.5f},
        {"presence.audio_source",   1.0f},
        {"presence.motion",         0.7f},
        {"presence.integrity",      1.0f},
    };
    return kv;
}
}  // namespace

const std::vector<std::string>& scoring_weight_keys() {
    static const std::vector<std::string> keys = [] {
        std::vector<std::string> v;
        v.reserve(kPublishedWeights().size());
        for (const auto& [k, _] : kPublishedWeights()) v.push_back(k);
        return v;
    }();
    return keys;
}

float weight_or_default(const Config& cfg, const std::string& key) {
    const auto it = cfg.scoring_weights.find(key);
    if (it != cfg.scoring_weights.end()) return it->second;
    const auto& defaults = kPublishedWeights();
    const auto dit = defaults.find(key);
    // Internal callers must use a known key. We don't crash in
    // release — bad scoring weight on an unknown key returns 0 which
    // visibly excludes the term from the ensemble.
    return (dit != defaults.end()) ? dit->second : 0.0f;
}

}  // namespace sentlog::internal
