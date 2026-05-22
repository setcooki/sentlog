// SentlogConfig parsing and validation.
//
// The integrator passes a JSON config string at create time. This
// translation unit parses it, validates it against the schema defined
// in docs/technical-specification.md §3.3, and returns either a
// populated Config struct or a typed error indicating the offending
// field. The SDK never silently accepts a malformed config.

#ifndef SENTLOG_INTERNAL_CONFIG_H
#define SENTLOG_INTERNAL_CONFIG_H

#include <optional>
#include <string>
#include <vector>

#include "sentlog/sentlog.h"

namespace sentlog::internal {

struct Forwarder {
    std::string name;
    std::string projection;       // enum, validated against the allowed set
    float       sample_rate{1.0f};
    // sink is opaque to the core in Phase 1 — the platform binding
    // realises the sink and the C++ core hands off projected payloads
    // to it via the C ABI's CallbackSink path. Other sinks land in
    // Phase 4.
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

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_CONFIG_H
