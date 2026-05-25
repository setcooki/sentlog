#include "internal/config.h"

#include <gtest/gtest.h>

using sentlog::internal::Config;
using sentlog::internal::parse_config;
using sentlog::internal::scoring_weight_keys;
using sentlog::internal::weight_or_default;

namespace {

const char* kMinimalValidJson = R"({})";

const char* kFullValidJson = R"({
  "liveness_threshold": 0.75,
  "engagement_threshold": 0.5,
  "direction_threshold": 0.65,
  "min_confidence": 0.6,
  "challenge_enabled": true,
  "language_hint": "en",
  "min_duration_ms": 5000,
  "max_duration_ms": 60000,
  "target_phrase": "hello world",
  "use_motion_sensors": true,
  "use_video_affect": false,
  "use_device_integrity": false,
  "issuer": "did:example:integrator",
  "signing_key_id": "did:example:integrator#key-1",
  "use_hardware_signing": false,
  "hardware_signing_required": false,
  "tsa_endpoint": "https://tsa.example.com",
  "tsa_timeout_ms": 5000,
  "tsa_required": false,
  "session_id": "session-abc",
  "companion_evidence_refs": ["evidence-1", "evidence-2"],
  "forwarders": [
    {
      "name": "audit",
      "projection": "EvidenceFingerprint",
      "sample_rate": 0.15,
      "sink": { "kind": "http", "url": "https://example.com/audit" }
    }
  ]
})";

}  // namespace

TEST(ConfigTest, EmptyJsonAppliesAllDefaults) {
    Config cfg;
    std::string err;
    EXPECT_EQ(parse_config(kMinimalValidJson, &cfg, &err), SENTLOG_OK);
    EXPECT_FLOAT_EQ(cfg.liveness_threshold,   0.7f);
    EXPECT_FLOAT_EQ(cfg.engagement_threshold, 0.5f);
    EXPECT_FLOAT_EQ(cfg.direction_threshold,  0.6f);
    EXPECT_FLOAT_EQ(cfg.min_confidence,       0.6f);
    EXPECT_TRUE(cfg.challenge_enabled);
    EXPECT_EQ(cfg.language_hint, "en");
    EXPECT_EQ(cfg.min_duration_ms, 5000);
    EXPECT_EQ(cfg.max_duration_ms, 60000);
    EXPECT_FALSE(cfg.target_phrase.has_value());
    EXPECT_FALSE(cfg.use_motion_sensors);
    EXPECT_FALSE(cfg.use_video_affect);
    EXPECT_FALSE(cfg.use_device_integrity);
    EXPECT_TRUE(cfg.issuer.empty());
    EXPECT_TRUE(cfg.forwarders.empty());
}

TEST(ConfigTest, FullValidJsonParses) {
    Config cfg;
    std::string err;
    ASSERT_EQ(parse_config(kFullValidJson, &cfg, &err), SENTLOG_OK) << err;

    EXPECT_FLOAT_EQ(cfg.liveness_threshold, 0.75f);
    EXPECT_TRUE(cfg.use_motion_sensors);
    ASSERT_TRUE(cfg.target_phrase.has_value());
    EXPECT_EQ(*cfg.target_phrase, "hello world");
    EXPECT_EQ(cfg.issuer, "did:example:integrator");
    ASSERT_TRUE(cfg.session_id.has_value());
    EXPECT_EQ(*cfg.session_id, "session-abc");
    ASSERT_EQ(cfg.companion_evidence_refs.size(), 2u);
    EXPECT_EQ(cfg.companion_evidence_refs[0], "evidence-1");
    ASSERT_EQ(cfg.forwarders.size(), 1u);
    EXPECT_EQ(cfg.forwarders[0].name, "audit");
    EXPECT_EQ(cfg.forwarders[0].projection, "EvidenceFingerprint");
    EXPECT_FLOAT_EQ(cfg.forwarders[0].sample_rate, 0.15f);
    EXPECT_EQ(cfg.forwarders[0].sink_kind, "http");
}

TEST(ConfigTest, MalformedJsonRejected) {
    Config cfg;
    std::string err;
    EXPECT_EQ(parse_config("not json", &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "<root>");
}

TEST(ConfigTest, ThresholdOutOfRangeRejected) {
    Config cfg;
    std::string err;
    EXPECT_EQ(parse_config(R"({"liveness_threshold": 1.5})", &cfg, &err),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "liveness_threshold");
}

TEST(ConfigTest, NegativeDurationRejected) {
    Config cfg;
    std::string err;
    EXPECT_EQ(parse_config(R"({"min_duration_ms": -1})", &cfg, &err),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "min_duration_ms");
}

TEST(ConfigTest, MinGreaterThanMaxDurationRejected) {
    Config cfg;
    std::string err;
    EXPECT_EQ(parse_config(
                  R"({"min_duration_ms": 10000, "max_duration_ms": 5000})",
                  &cfg, &err),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "max_duration_ms");
}

TEST(ConfigTest, ForwarderWithoutIssuerRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({
        "forwarders": [
            {"name":"x","projection":"Full","sink":{"kind":"http","url":"https://x"}}
        ]
    })";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "issuer");
}

TEST(ConfigTest, UnknownProjectionRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({
        "issuer": "did:example:integrator",
        "forwarders": [
            {"name":"x","projection":"NotAProjection","sink":{"kind":"http"}}
        ]
    })";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].projection");
}

TEST(ConfigTest, UnknownSinkKindRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({
        "issuer": "did:example:integrator",
        "forwarders": [
            {"name":"x","projection":"Full","sink":{"kind":"smoke-signal"}}
        ]
    })";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].sink.kind");
}

TEST(ConfigTest, TsaRequiredWithoutEndpointRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({"tsa_required": true})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "tsa_endpoint");
}

TEST(ConfigTest, HardwareSigningRequiredWithoutUseHardwareSigningRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({"hardware_signing_required": true})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "use_hardware_signing");
}

// --- Scoring weights (Phase 3b) ---

TEST(ConfigTest, ScoringWeightsKnownKeysParsed) {
    Config cfg;
    const char* j = R"({"issuer": "did:example:x",
                        "scoring_weights": {
                            "engagement.face":      0.8,
                            "presence.motion":      0.3
                        }})";
    EXPECT_EQ(parse_config(j, &cfg, nullptr), SENTLOG_OK);
    EXPECT_FLOAT_EQ(cfg.scoring_weights["engagement.face"], 0.8f);
    EXPECT_FLOAT_EQ(cfg.scoring_weights["presence.motion"], 0.3f);
}

TEST(ConfigTest, ScoringWeightsUnknownKeyRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({"scoring_weights": {"engagement.face_typo": 0.5}})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "scoring_weights.engagement.face_typo");
}

TEST(ConfigTest, ScoringWeightsNonNumericRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({"scoring_weights": {"engagement.voice": "high"}})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "scoring_weights.engagement.voice");
}

TEST(ConfigTest, ScoringWeightsNegativeRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({"scoring_weights": {"engagement.voice": -0.1}})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "scoring_weights.engagement.voice");
}

TEST(ConfigTest, ScoringWeightsExcessiveRejected) {
    Config cfg;
    std::string err;
    const char* j = R"({"scoring_weights": {"engagement.voice": 1000.0}})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "scoring_weights.engagement.voice");
}

TEST(ConfigTest, ScoringWeightsObjectShapeRequired) {
    Config cfg;
    std::string err;
    const char* j = R"({"scoring_weights": [0.5, 0.3]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "scoring_weights");
}

TEST(ConfigTest, WeightOrDefaultReturnsOverrideOrPublished) {
    Config cfg;
    cfg.scoring_weights["presence.motion"] = 0.25f;
    EXPECT_FLOAT_EQ(weight_or_default(cfg, "presence.motion"), 0.25f);
    EXPECT_FLOAT_EQ(weight_or_default(cfg, "engagement.voice"), 1.0f)
        << "fallback to published default";
    EXPECT_FLOAT_EQ(weight_or_default(cfg, "engagement.face"), 0.5f);
}

TEST(ConfigTest, ScoringWeightKeysCoversAllPublished) {
    const auto& keys = scoring_weight_keys();
    EXPECT_EQ(keys.size(), 7u);
    Config cfg;  // empty; every key falls back to published defaults
    for (const auto& k : keys) {
        EXPECT_GE(weight_or_default(cfg, k), 0.0f)
            << "missing default for key " << k;
    }
}

// --- Sink-shape validation (Phase 4e) ---

TEST(ConfigTest, HttpSinkRequiresUrl) {
    Config cfg;
    std::string err;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"a","projection":"Full",
        "sink":{"kind":"http"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].sink.url");
}

TEST(ConfigTest, WebhookSinkRequiresUrlAndHmacSecret) {
    Config cfg;
    std::string err;
    const char* missing_secret = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"a","projection":"Full",
        "sink":{"kind":"webhook","url":"https://x/"}}]})";
    EXPECT_EQ(parse_config(missing_secret, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].sink.hmac_secret");

    const char* both_present = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"a","projection":"Full",
        "sink":{"kind":"webhook","url":"https://x/","hmac_secret":"k"}}]})";
    EXPECT_EQ(parse_config(both_present, &cfg, nullptr), SENTLOG_OK);
}

TEST(ConfigTest, LocalQueueSinkRequiresPath) {
    Config cfg;
    std::string err;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"q","projection":"Full",
        "sink":{"kind":"local_queue"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].sink.path");
}

TEST(ConfigTest, CallbackSinkRequiresNoSinkSideFields) {
    Config cfg;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"c","projection":"Full",
        "sink":{"kind":"callback"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, nullptr), SENTLOG_OK);
}

// --- Custom projection's custom_fields validation (Phase 4f) ---

TEST(ConfigTest, CustomProjectionRequiresCustomFields) {
    Config cfg;
    std::string err;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"c","projection":"Custom",
        "sink":{"kind":"callback"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].custom_fields");
}

TEST(ConfigTest, CustomProjectionRejectsEmptyFieldsArray) {
    Config cfg;
    std::string err;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"c","projection":"Custom",
        "custom_fields":[],
        "sink":{"kind":"callback"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].custom_fields");
}

TEST(ConfigTest, CustomProjectionRejectsNonStringEntries) {
    Config cfg;
    std::string err;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"c","projection":"Custom",
        "custom_fields":["transcript.text", 42],
        "sink":{"kind":"callback"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].custom_fields[1]");
}

TEST(ConfigTest, NonCustomProjectionRejectsCustomFields) {
    Config cfg;
    std::string err;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"p","projection":"Full",
        "custom_fields":["transcript.text"],
        "sink":{"kind":"callback"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, &err), SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(err, "forwarders[0].custom_fields");
}

TEST(ConfigTest, CustomProjectionParsesValidFields) {
    Config cfg;
    const char* j = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"c","projection":"Custom",
        "custom_fields":["transcript.text","liveness.score"],
        "sink":{"kind":"callback"}}]})";
    EXPECT_EQ(parse_config(j, &cfg, nullptr), SENTLOG_OK);
    ASSERT_EQ(cfg.forwarders.size(), 1u);
    ASSERT_EQ(cfg.forwarders[0].custom_fields.size(), 2u);
    EXPECT_EQ(cfg.forwarders[0].custom_fields[0], "transcript.text");
    EXPECT_EQ(cfg.forwarders[0].custom_fields[1], "liveness.score");
}
