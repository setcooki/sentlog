// SentlogConfig parsing and validation.
//
// The integrator passes a JSON config string at create time. This
// translation unit parses it, validates it against the schema defined
// in docs/technical-specification.md §3.3, and returns either a
// populated Config struct or a typed error indicating the offending
// field. The SDK never silently accepts a malformed config.

#ifndef SENTLOG_INTERNAL_CONFIG_H
#define SENTLOG_INTERNAL_CONFIG_H

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "internal/signing.h"
#include "sentlog/sentlog.h"

namespace sentlog::internal {

struct Forwarder {
    std::string name;
    std::string projection;       // enum, validated against the allowed set
    float       sample_rate{1.0f};
    // Only consumed when projection == "Custom" (Phase 4f). List of
    // dotted JSON paths the integrator wants in the projected
    // payload. Empty for non-Custom projections.
    std::vector<std::string> custom_fields;
    // sink is opaque to the core: shared/ doesn't run HTTP, the
    // platform binding realises http/webhook sinks via its
    // URLSession/OkHttp client. The C++ core dispatches local_queue
    // (file append) itself and routes everything else through the
    // integrator-supplied envelope dispatcher (Phase 4d).
    std::string sink_kind;        // "http" | "webhook" | "local_queue" | "callback"
    std::string sink_json;        // sink-specific config, opaque here
};

struct Config {
    // thresholds
    float liveness_threshold{0.7f};
    float engagement_threshold{0.5f};
    float direction_threshold{0.6f};
    float min_confidence{0.6f};

    // pipeline
    bool        challenge_enabled{true};
    std::string language_hint{"en"};
    int         min_duration_ms{5000};
    int         max_duration_ms{60000};
    std::optional<std::string> target_phrase;

    // multi-modal (opt-in)
    bool use_motion_sensors{false};
    bool use_video_affect{false};
    bool use_device_integrity{false};

    // Per-component ensemble weights — overrides for the published
    // defaults documented in docs/scoring-weights.md. Stored sparse:
    // only the keys the integrator overrode. Pipeline code reads via
    // weight_or_default(...) below. Unrecognised keys are rejected
    // at parse time (decision row 1.21 in the spec).
    std::map<std::string, float> scoring_weights;

    // evidence envelope (Verifiable Credentials)
    std::string issuer;                          // required if any forwarder is configured
    std::optional<std::string> signing_key_id;
    bool use_hardware_signing{false};
    bool hardware_signing_required{false};

    // optional RFC 3161 TSA
    std::optional<std::string> tsa_endpoint;
    int  tsa_timeout_ms{5000};
    bool tsa_required{false};

    // session
    std::optional<std::string> session_id;
    std::vector<std::string>   companion_evidence_refs;

    // forwarders
    std::vector<Forwarder> forwarders;

    // OTA model updates — spec decision row 1.20.
    //
    // Both default to absent: when both are unset, the platform
    // binding falls back to the published Sentlog manifest URL and
    // the Sentlog-pinned pubkeys baked into the binding.
    //
    // Overriding requires overriding BOTH together — mixed-source
    // configuration (custom URL with default keys, or custom keys
    // pointing at the Sentlog-published manifest) is rejected at
    // init. Validation enforces this in parse_config().
    //
    // `manifest_pubkeys` carries exactly two raw Ed25519 pubkeys
    // (32 bytes each): index 0 = "active", index 1 = "standby".
    // The JSON wire format uses base64(no-pad) strings; the parser
    // decodes them here.
    std::optional<std::string>                       manifest_url;
    std::optional<std::array<Ed25519PublicKey, 2>>   manifest_pubkeys;
};

// Parse and validate a JSON config string. On success returns OK and
// fills *out. On failure returns SENTLOG_ERR_INVALID_CONFIG and
// optionally populates *error_field with the dotted JSON path of the
// offending field (e.g. "scoring_weights.engagement.face").
sentlog_status_t parse_config(const std::string& json_text,
                              Config* out,
                              std::string* error_field);

// Serialise the config back to a JSON string. Used for telemetry and
// diagnostics; not part of the hot path.
std::string config_to_json(const Config& cfg);

// Allowed scoring_weights keys, in canonical order. The parser
// rejects any key not in this list. The pipeline reads its weight
// for `key` via weight_or_default(cfg, key) below, which falls back
// to the published default when the integrator hasn't overridden it.
extern const std::vector<std::string>& scoring_weight_keys();

// Look up a scoring weight. Returns the integrator override if
// present, otherwise the published default from
// docs/scoring-weights.md. Aborts (via assert) if `key` is not one
// of scoring_weight_keys() — callers must use named constants, not
// strings parsed from input.
float weight_or_default(const Config& cfg, const std::string& key);

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_CONFIG_H
