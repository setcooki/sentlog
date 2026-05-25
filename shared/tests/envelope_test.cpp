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
using sentlog::internal::parse_projection_name;
using sentlog::internal::Projection;
using sentlog::internal::projection_name;
using sentlog::internal::ProjectionSpec;

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

// --- Projection tests (chunk 4c) ---

namespace {
ProjectionSpec spec_of(Projection p) {
    ProjectionSpec s;
    s.kind = p;
    return s;
}
ProjectionSpec custom_of(std::vector<std::string> fields) {
    ProjectionSpec s;
    s.kind = Projection::Custom;
    s.custom_fields = std::move(fields);
    return s;
}
}  // namespace

TEST(ProjectionTest, ParseAndPrintRoundTripsAllVariants) {
    for (const char* name : {"Full", "Full+AudioClip", "ScoresOnly",
                             "TranscriptOnly", "EvidenceFingerprint",
                             "EvidenceClip", "Custom"}) {
        Projection p;
        ASSERT_TRUE(parse_projection_name(name, &p)) << name;
        EXPECT_STREQ(projection_name(p), name);
    }
}

TEST(ProjectionTest, ParseRejectsUnknownName) {
    Projection p;
    EXPECT_FALSE(parse_projection_name("WhateverElse", &p));
    EXPECT_FALSE(parse_projection_name("",              &p));
}

TEST(ProjectionTest, FullCarriesEntireResult) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        spec_of(Projection::Full)));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_TRUE(p.contains("transcript"));
    EXPECT_TRUE(p.contains("liveness"));
    EXPECT_TRUE(p.contains("presence"));
    EXPECT_TRUE(p.contains("fingerprint"));
    EXPECT_TRUE(p.contains("engagement"));
    EXPECT_TRUE(p.contains("diagnostics"));
}

TEST(ProjectionTest, ScoresOnlyDropsFingerprintAndWordTimings) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        spec_of(Projection::ScoresOnly)));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_TRUE(p.contains("liveness"));
    EXPECT_TRUE(p.contains("presence"));
    EXPECT_TRUE(p.contains("engagement"));
    EXPECT_TRUE(p.contains("transcript"));
    EXPECT_TRUE(p["transcript"].contains("text"));
    EXPECT_FALSE(p["transcript"].contains("word_timings"));
    EXPECT_FALSE(p.contains("fingerprint"));
}

TEST(ProjectionTest, TranscriptOnlyDropsScoresAndFingerprint) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        spec_of(Projection::TranscriptOnly)));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_TRUE(p.contains("transcript"));
    EXPECT_FALSE(p.contains("liveness"));
    EXPECT_FALSE(p.contains("presence"));
    EXPECT_FALSE(p.contains("fingerprint"));
    EXPECT_FALSE(p.contains("engagement"));
}

TEST(ProjectionTest, EvidenceFingerprintCarriesScoresAndFingerprintOnly) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        spec_of(Projection::EvidenceFingerprint)));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_TRUE(p.contains("liveness"));
    EXPECT_TRUE(p.contains("presence"));
    EXPECT_TRUE(p.contains("fingerprint"));
    EXPECT_FALSE(p.contains("transcript"));
}

TEST(ProjectionTest, EvidenceClipCarriesScoresAndClipPlaceholder) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        spec_of(Projection::EvidenceClip)));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_TRUE(p.contains("liveness"));
    EXPECT_TRUE(p.contains("audio_clip"))
        << "EvidenceClip should carry an audio_clip block; Phase 4g fills the bytes";
    EXPECT_EQ(p["audio_clip"]["codec"], "opus");
    EXPECT_TRUE(p["audio_clip"]["bytes"].get<std::string>().empty())
        << "4g lands the actual Opus encoding";
    EXPECT_FALSE(p.contains("transcript"));
}

TEST(ProjectionTest, FullPlusAudioClipIsFullPlusClip) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        spec_of(Projection::FullPlusAudioClip)));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_TRUE(p.contains("transcript"));
    EXPECT_TRUE(p.contains("fingerprint"));
    EXPECT_TRUE(p.contains("audio_clip"));
}

TEST(ProjectionTest, CustomPicksDottedFieldsOnly) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        custom_of({"transcript.text",
                   "liveness.score",
                   "presence.audio_source"})));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_EQ(p["transcript"]["text"], "hello");
    EXPECT_FLOAT_EQ(p["liveness"]["score"].get<float>(), 0.88f);
    EXPECT_FLOAT_EQ(p["presence"]["audio_source"].get<float>(), 1.0f);
    // Fields not requested must be absent.
    EXPECT_FALSE(p["transcript"].contains("language"));
    EXPECT_FALSE(p["liveness"].contains("replay_risk"));
    EXPECT_FALSE(p.contains("fingerprint"));
    EXPECT_FALSE(p.contains("engagement"));
}

TEST(ProjectionTest, CustomSilentlyDropsMissingPaths) {
    auto env = json::parse(build_envelope_unsigned(
        make_config("did:example:integrator"),
        kReferenceResultJson,
        "2026-05-25T10:00:00.000Z",
        custom_of({"transcript.text",
                   "nonexistent.path",
                   "fingerprint.does.not.exist"})));
    const auto& p = env["credentialSubject"]["payload"];
    EXPECT_EQ(p["transcript"]["text"], "hello");
    EXPECT_FALSE(p.contains("nonexistent"));
    EXPECT_FALSE(p.contains("fingerprint"));
}

TEST(ProjectionTest, ProjectionNameSurfacesInSubject) {
    for (const auto p : {Projection::ScoresOnly,
                         Projection::TranscriptOnly,
                         Projection::EvidenceFingerprint,
                         Projection::Custom}) {
        auto env = json::parse(build_envelope_unsigned(
            make_config("did:example:integrator"),
            kReferenceResultJson,
            "2026-05-25T10:00:00.000Z",
            spec_of(p)));
        EXPECT_EQ(env["credentialSubject"]["projection"],
                  projection_name(p));
    }
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
