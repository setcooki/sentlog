#include "internal/envelope.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

#include "internal/config.h"

using nlohmann::json;
using sentlog::internal::build_envelope_unsigned;
using sentlog::internal::Config;
using sentlog::internal::envelope_format;
using sentlog::internal::iso8601_utc_now;

namespace {

const char* kReferenceResultJson = R"({
    "schema_version": "1",
    "transcript": {"text": "hello", "language": "en", "confidence": 0.9},
    "liveness":   {"score": 0.88, "replay_risk": 0.05, "synthetic_risk": 0.07},
    "presence":   {"summary": 0.9, "audio_source": 1.0},
    "direction":  0.5,
    "confidence": 0.6,
    "thresholds_met": false,
    "engagement": {"score": 0.65, "voice_score": 0.65, "model_status": "training"},
    "fingerprint": {"algo": "sentlog-pcp-v1", "bytes": "AAA="},
    "model_versions": {"liveness": "x", "asr": "x", "prosody": "x",
                       "vad": "x", "manifest": "x"},
    "diagnostics":  {"duration_ms": 1500, "audio_quality": "moderate"},
    "evidence_grade": "DeviceUnattested",
    "extensions": {}
})";

Config make_config(const char* issuer,
                   const char* session_id = nullptr) {
    Config cfg;
    cfg.issuer = issuer;
    if (session_id) cfg.session_id = std::string(session_id);
    return cfg;
}

}  // namespace

TEST(EnvelopeTest, ContextAndTypeFollowVC2) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));

    ASSERT_TRUE(env["@context"].is_array());
    EXPECT_EQ(env["@context"][0], "https://www.w3.org/ns/credentials/v2");
    EXPECT_EQ(env["@context"][1], "https://sentlog.org/contexts/evidence/v1");

    ASSERT_TRUE(env["type"].is_array());
    EXPECT_EQ(env["type"][0], "VerifiableCredential");
    EXPECT_EQ(env["type"][1], "SentlogEvidence");
}

TEST(EnvelopeTest, IssuerFromConfig) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_EQ(env["issuer"], "did:example:integrator");
}

TEST(EnvelopeTest, ValidFromPassedThrough) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.123Z"));
    EXPECT_EQ(env["validFrom"], "2026-05-25T10:00:00.123Z");
}

TEST(EnvelopeTest, SubjectIdFromSessionId) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator", "sess-42"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_EQ(env["credentialSubject"]["id"], "did:sentlog:session:sess-42");
}

TEST(EnvelopeTest, SubjectIdOmittedWhenSessionIdAbsent) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_FALSE(env["credentialSubject"].contains("id"));
}

TEST(EnvelopeTest, EnvelopeFormatMatchesConstant) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_EQ(env["credentialSubject"]["envelope_format"], envelope_format());
    EXPECT_EQ(env["credentialSubject"]["envelope_format"],
              "sentlog-evidence/1");
}

TEST(EnvelopeTest, FullProjectionCarriesEntireResult) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_EQ(env["credentialSubject"]["projection"], "Full");
    // payload mirrors the result we passed in
    EXPECT_EQ(env["credentialSubject"]["payload"]["transcript"]["text"],
              "hello");
    EXPECT_EQ(env["credentialSubject"]["payload"]["liveness"]["score"], 0.88);
}

TEST(EnvelopeTest, ModelVersionsAndEvidenceGradeSurfacedAtTopOfSubject) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_EQ(env["credentialSubject"]["evidence_grade"], "DeviceUnattested");
    EXPECT_TRUE(env["credentialSubject"]["model_versions"].is_object());
}

TEST(EnvelopeTest, CompanionEvidenceRefsCarriedFromConfig) {
    Config cfg = make_config("did:example:integrator");
    cfg.companion_evidence_refs = {"ref-a", "ref-b"};
    auto env = json::parse(build_envelope_unsigned(
        cfg, kReferenceResultJson, "2026-05-25T10:00:00.000Z"));
    ASSERT_TRUE(env["credentialSubject"]["companion_evidence_refs"].is_array());
    EXPECT_EQ(env["credentialSubject"]["companion_evidence_refs"][0], "ref-a");
    EXPECT_EQ(env["credentialSubject"]["companion_evidence_refs"][1], "ref-b");
}

TEST(EnvelopeTest, CompanionRefsOmittedWhenEmpty) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_FALSE(env["credentialSubject"].contains("companion_evidence_refs"));
}

TEST(EnvelopeTest, NoProofBlockIn4a) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_FALSE(env.contains("proof"))
        << "Phase 4a emits the envelope without a proof — signing lands in 4b";
}

TEST(EnvelopeTest, NoTsaTimestampIn4a) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z"));
    EXPECT_FALSE(env.contains("tsa_timestamp"))
        << "TSA hook lands in 4h";
}

TEST(EnvelopeTest, Iso8601NowFormatIsCorrect) {
    const std::string s = iso8601_utc_now();
    // YYYY-MM-DDTHH:MM:SS.mmmZ — 24 chars total.
    EXPECT_EQ(s.size(), 24u);
    EXPECT_EQ(s[4],  '-');
    EXPECT_EQ(s[7],  '-');
    EXPECT_EQ(s[10], 'T');
    EXPECT_EQ(s[13], ':');
    EXPECT_EQ(s[16], ':');
    EXPECT_EQ(s[19], '.');
    EXPECT_EQ(s[23], 'Z');
}
