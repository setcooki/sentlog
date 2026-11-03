// Regression tests for core robustness: bad input, resource limits and
// the local-queue drain.

#include "sentlog/sentlog.h"
#include "internal/ota.h"
#include "internal/signing.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

using nlohmann::json;
using sentlog::internal::base64url_no_pad;
using sentlog::internal::ed25519_sign;
using sentlog::internal::Ed25519PublicKey;
using sentlog::internal::Ed25519SecretKey;
using sentlog::internal::jcs_canonicalize;
using sentlog::internal::Manifest;
using sentlog::internal::ManifestVerifyStatus;
using sentlog::internal::parse_and_verify_manifest;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct Handle {
    sentlog_handle_t* h{nullptr};
    std::vector<std::string> telemetry;
    explicit Handle(const std::string& config) {
        sentlog_status_t status = SENTLOG_OK;
        h = sentlog_create(config.c_str(), &on_telemetry, this, &status);
        EXPECT_NE(h, nullptr) << config;
    }
    ~Handle() { sentlog_destroy(h); }
    static void on_telemetry(const char* event, void* userdata) {
        static_cast<Handle*>(userdata)->telemetry.push_back(event);
    }
    std::vector<json> events(const std::string& type) const {
        std::vector<json> out;
        for (const auto& e : telemetry) {
            const auto j = json::parse(e);
            if (j.value("type", "") == type) out.push_back(j);
        }
        return out;
    }
};

json evaluate(sentlog_handle_t* h) {
    std::string out;
    sentlog_status_t status = SENTLOG_ERR_INTERNAL;
    struct Ctx { std::string* out; sentlog_status_t* status; } ctx{&out, &status};
    sentlog_evaluate(h, [](sentlog_status_t s, const char* j, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        *c->status = s;
        if (j != nullptr) *c->out = j;
    }, &ctx);
    EXPECT_EQ(status, SENTLOG_OK);
    return out.empty() ? json() : json::parse(out);
}

std::vector<float> speech(size_t seconds) {
    std::vector<float> v(seconds * 16000);
    for (size_t i = 0; i < v.size(); ++i) {
        const float t = static_cast<float>(i) / 16000.0f;
        const float env = 0.5f + 0.5f * std::sin(2.0f * 3.14159265f * 4.0f * t);
        v[i] = 0.3f * env * std::sin(2.0f * 3.14159265f * 300.0f * t);
    }
    return v;
}

std::string temp_path(const std::string& suffix) {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir != nullptr ? dir : "/tmp") + "/sentlog-hardening-" + suffix;
}

json check_updates(sentlog_handle_t* h, const std::string& manifest,
                   sentlog_status_t* status_out = nullptr) {
    struct Ctx { std::string json; sentlog_status_t status{SENTLOG_OK}; } ctx;
    sentlog_check_for_updates(
        h, reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size(),
        [](sentlog_status_t s, const char* j, void* ud) {
            auto* c = static_cast<Ctx*>(ud);
            c->status = s;
            if (j != nullptr) c->json = j;
        }, &ctx);
    if (status_out != nullptr) *status_out = ctx.status;
    return json::parse(ctx.json);
}

// RFC 8032 §7.1 test vector 1.
Ed25519SecretKey test_key() {
    const std::string hex =
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    Ed25519SecretKey sk{};
    for (size_t i = 0; i < sk.size(); ++i) {
        sk[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    return sk;
}

const char* kOtaConfig = R"({"issuer":"did:example:i",
    "manifest_url":"https://e.example/m",
    "manifest_pubkeys":["11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=",
                        "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="]})";

std::string signed_manifest(const std::string& version) {
    json m = {{"schema_version", "1"}, {"manifest_version", version},
              {"bundle_url", "https://e.example/b.tar"},
              {"bundle_sha256", std::string(64, 'a')},
              {"bundle_size", 123}, {"issued_at_ms", 1700000000000LL}};
    const auto sig = ed25519_sign(jcs_canonicalize(m.dump()), test_key());
    json env = {{"manifest", m}, {"key_id", "active"},
                {"signature_b64url", base64url_no_pad(sig.data(), sig.size())}};
    return env.dump();
}

}  // namespace

// --- NaN never reaches a score ---

TEST(CoreHardeningTest, NaNLivenessBecomesZero) {
    Handle h(R"({"issuer":"did:example:i","min_duration_ms":500})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    const auto audio = speech(1);
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    ASSERT_EQ(sentlog_push_liveness_result(h.h, kNaN, kNaN, kNaN), SENTLOG_OK);
    const auto r = evaluate(h.h);
    ASSERT_TRUE(r["liveness"]["score"].is_number());
    EXPECT_EQ(r["liveness"]["score"].get<float>(), 0.0f);
    EXPECT_EQ(r["liveness"]["replay_risk"].get<float>(), 0.0f);
}

TEST(CoreHardeningTest, NaNFaceFrameStaysInRange) {
    Handle h(R"({"issuer":"did:example:i","min_duration_ms":500,"use_video_affect":true})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    const auto audio = speech(1);
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    ASSERT_EQ(sentlog_push_face_frame(h.h, 1, kNaN, kNaN, nullptr), SENTLOG_OK);
    const auto r = evaluate(h.h);
    ASSERT_TRUE(r["affect"]["valence"].is_number());
    ASSERT_TRUE(r["affect"]["arousal"].is_number());
    ASSERT_TRUE(r["engagement"]["score"].is_number());
}

// --- Capture limits ---

TEST(CoreHardeningTest, DefaultConfigCapturesPast32Seconds) {
    Handle h(R"({"issuer":"did:example:i"})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    const auto audio = speech(40);
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    EXPECT_EQ(evaluate(h.h)["diagnostics"]["duration_ms"].get<int>(), 40000);
}

TEST(CoreHardeningTest, MaxDurationIsEnforced) {
    Handle h(R"({"issuer":"did:example:i","min_duration_ms":500,"max_duration_ms":1000})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    const auto audio = speech(3);
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    EXPECT_EQ(evaluate(h.h)["diagnostics"]["duration_ms"].get<int>(), 1000);
}

TEST(CoreHardeningTest, MaxDurationBeyondBufferIsRejected) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(R"({"issuer":"did:example:i","max_duration_ms":70000})",
                             nullptr, nullptr, &status);
    EXPECT_EQ(h, nullptr);
    EXPECT_EQ(status, SENTLOG_ERR_INVALID_CONFIG);
}

// --- Bad input doesn't lose an evaluation ---

TEST(CoreHardeningTest, InvalidUtf8FauDoesNotDiscardEvaluation) {
    Handle h(R"({"issuer":"did:example:i","min_duration_ms":500,"use_video_affect":true})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    const auto audio = speech(1);
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    ASSERT_EQ(sentlog_push_face_frame(h.h, 1, 0.1f, 0.1f, "AU\xFF\xFE"), SENTLOG_OK);
    EXPECT_TRUE(evaluate(h.h).is_object());
}

TEST(CoreHardeningTest, NegativeSnrIsTooNoisy) {
    Handle h(R"({"issuer":"did:example:i","min_duration_ms":500})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    const auto audio = speech(2);
    sentlog_push_audio_frame(h.h, audio.data(), audio.size());
    // Marks the loud portion as noise and a silent one as speech.
    const std::vector<float> quiet(16000, 0.0005f);
    sentlog_push_audio_frame(h.h, quiet.data(), quiet.size());
    ASSERT_EQ(sentlog_push_vad_frame(h.h, 0, 32000, 0.0f), SENTLOG_OK);
    ASSERT_EQ(sentlog_push_vad_frame(h.h, 32000, 16000, 0.99f), SENTLOG_OK);
    const auto r = evaluate(h.h);
    EXPECT_EQ(r["diagnostics"]["failure_reason"].get<std::string>(), "TooNoisy");
}

// --- Hardware fallback telemetry tells the truth ---

TEST(CoreHardeningTest, FallbackWithoutSoftwareKeyReportsNone) {
    Handle h(R"({"issuer":"did:example:i","use_hardware_signing":true,
                 "forwarders":[{"name":"a","projection":"ScoresOnly",
                                "sink":{"kind":"callback"}}]})");
    sentlog_set_envelope_dispatcher(
        h.h, [](const char*, const char*, const char*, const char*, void*) {}, nullptr);
    sentlog_start_capture(h.h);
    evaluate(h.h);
    const auto events = h.events("HardwareSigningUnavailable");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]["fell_back_to"].get<std::string>(), "none");
}

// --- OTA ---

TEST(CoreHardeningTest, DeeplyNestedManifestIsRejected) {
    Ed25519PublicKey key{};
    key[0] = 0xd7;
    std::string nested(200000, '[');
    nested += std::string(200000, ']');
    const std::string text =
        R"({"key_id":"active","signature_b64url":"AA","manifest":{"schema_version":"1",)"
        R"("manifest_version":"1","bundle_url":"u","bundle_sha256":"a","bundle_size":1,)"
        R"("issued_at_ms":1,"x":)" + nested + "}}";
    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(text, key, key, m),
              ManifestVerifyStatus::MissingFields);
}

TEST(CoreHardeningTest, ManifestVerifyFailedCarriesAttemptedVersion) {
    Handle h(kOtaConfig);
    auto forged = json::parse(signed_manifest("9.9.9"));
    forged["manifest"]["bundle_size"] = 999;
    sentlog_status_t status = SENTLOG_OK;
    check_updates(h.h, forged.dump(), &status);
    EXPECT_EQ(status, SENTLOG_ERR_MANIFEST_VERIFY);
    const auto events = h.events("ManifestVerifyFailed");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]["attempted_version"].get<std::string>(), "9.9.9");
}

TEST(CoreHardeningTest, ManifestVerifyFailedAlwaysHasVersionField) {
    Handle h(kOtaConfig);
    check_updates(h.h, "not json");
    const auto events = h.events("ManifestVerifyFailed");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]["attempted_version"].get<std::string>(), "");
}

TEST(CoreHardeningTest, UpdateResultExposesVerifiedBundle) {
    Handle h(kOtaConfig);
    const auto r = check_updates(h.h, signed_manifest("1.2.3"));
    EXPECT_TRUE(r["update_found"].get<bool>());
    EXPECT_EQ(r["new_version"].get<std::string>(), "1.2.3");
    EXPECT_EQ(r["bundle_url"].get<std::string>(), "https://e.example/b.tar");
    EXPECT_EQ(r["bundle_sha256"].get<std::string>(), std::string(64, 'a'));
    EXPECT_EQ(r["bundle_size"].get<int64_t>(), 123);
}

TEST(CoreHardeningTest, OlderOrEqualManifestIsNotAnUpdate) {
    Handle h(kOtaConfig);
    for (const char* version : {"0.0.0-phase1", "0.0.0-alpha"}) {
        const auto r = check_updates(h.h, signed_manifest(version));
        EXPECT_FALSE(r["update_found"].get<bool>()) << version;
        EXPECT_FALSE(r.contains("bundle_url")) << version;
    }
    // A release is newer than its own prerelease.
    EXPECT_TRUE(check_updates(h.h, signed_manifest("0.0.0"))["update_found"].get<bool>());
    EXPECT_TRUE(check_updates(h.h, signed_manifest("0.0.1"))["update_found"].get<bool>());
    EXPECT_TRUE(check_updates(h.h, signed_manifest("0.10.0"))["update_found"].get<bool>());
}

TEST(CoreHardeningTest, UnparseableManifestVersionIsNotAnUpdate) {
    Handle h(kOtaConfig);
    for (const char* version : {"", "latest", "1.x", "1.2.3.4", "99999999999999999999.0.0"}) {
        const auto r = check_updates(h.h, signed_manifest(version));
        EXPECT_FALSE(r["update_found"].get<bool>()) << version;
    }
}

// --- Local-queue drain ---

namespace {
struct ReentrantDrain {
    sentlog_handle_t* h;
    int calls{0};
};
}  // namespace

TEST(CoreHardeningTest, DrainCallbackCanEvaluateWithoutDeadlockAndKeepsNewEnvelope) {
    const auto path = temp_path("reentrant");
    std::remove(path.c_str());
    { std::ofstream(path) << "{\"old\":1}\n"; }

    Handle h(R"({"issuer":"did:example:i","forwarders":[{"name":"q",
        "projection":"ScoresOnly","sink":{"kind":"local_queue","path":")" + path + R"("}}]})");
    ReentrantDrain ctx{h.h};
    const int delivered = sentlog_drain_queue(
        h.h, path.c_str(),
        [](const char*, size_t, void* ud) -> int {
            auto* c = static_cast<ReentrantDrain*>(ud);
            ++c->calls;
            sentlog_start_capture(c->h);
            evaluate(c->h);  // appends to the queue being drained
            return 1;
        },
        &ctx);
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(ctx.calls, 1);

    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_TRUE(json::parse(lines[0]).contains("proof") ||
                json::parse(lines[0]).contains("credentialSubject"));
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
    std::remove((path + ".drain").c_str());
}

TEST(CoreHardeningTest, DrainFromInsideDrainReturnsZero) {
    const auto path = temp_path("nested");
    { std::ofstream(path) << "{\"old\":1}\n"; }
    static int nested_result;
    nested_result = -1;
    static std::string nested_path;
    nested_path = path;
    sentlog_drain_queue(nullptr, path.c_str(), [](const char*, size_t, void*) -> int {
        nested_result = sentlog_drain_queue(
            nullptr, nested_path.c_str(),
            [](const char*, size_t, void*) -> int { return 1; }, nullptr);
        return 0;
    }, nullptr);
    EXPECT_EQ(nested_result, 0);
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
    std::remove((path + ".drain").c_str());
}

TEST(CoreHardeningTest, DrainKeepsQueueWhenRewriteFails) {
    const auto path = temp_path("rewrite-fails");
    const auto tmp = path + ".drain.tmp";
    { std::ofstream(path) << "{\"a\":1}\n{\"b\":2}\n"; }
    std::remove(tmp.c_str());
    ASSERT_EQ(std::system(("mkdir -p '" + tmp + "'").c_str()), 0);  // blocks the temp file

    const int delivered = sentlog_drain_queue(
        nullptr, path.c_str(), [](const char*, size_t, void*) -> int { return 1; }, nullptr);
    EXPECT_EQ(delivered, 2);

    std::ifstream in(path);
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) ++lines;
    EXPECT_EQ(lines, 2);
    ASSERT_EQ(std::system(("rmdir '" + tmp + "'").c_str()), 0);
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
    std::remove((path + ".drain").c_str());
}

// --- Bounded buffers ---

TEST(CoreHardeningTest, VadFramesAreBounded) {
    Handle h(R"({"issuer":"did:example:i"})");
    ASSERT_EQ(sentlog_start_capture(h.h), SENTLOG_OK);
    for (int i = 0; i < 200000; ++i) {
        ASSERT_EQ(sentlog_push_vad_frame(h.h, 0, 1, 0.5f), SENTLOG_OK);
    }
    EXPECT_TRUE(evaluate(h.h).is_object());
}

TEST(CoreHardeningTest, CheckForUpdatesAlwaysFiresCallback) {
    Handle h(R"({"issuer":"did:example:i"})");
    sentlog_status_t status = SENTLOG_OK;
    check_updates(h.h, "{}", &status);
    EXPECT_NE(status, SENTLOG_OK);
}
