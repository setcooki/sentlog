// End-to-end pipeline tests through the C ABI. Push synthetic audio
// into a created handle, evaluate, parse the JSON result, validate
// the schema and diagnostic contents.

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kSampleRateHz = 16000;
const char* kValidConfig =
    R"({"issuer": "did:example:integrator", "min_duration_ms": 500})";

struct Capture {
    sentlog_status_t status{SENTLOG_OK};
    std::string      result_json;
    bool             called{false};
};

void on_result(sentlog_status_t status, const char* result_json, void* userdata) {
    auto* cap = static_cast<Capture*>(userdata);
    cap->called = true;
    cap->status = status;
    cap->result_json = result_json ? result_json : "";
}

std::vector<float> make_silence_2s() {
    return std::vector<float>(2 * kSampleRateHz, 0.0f);
}

std::vector<float> make_speech_2s() {
    std::vector<float> v;
    v.reserve(2 * kSampleRateHz);
    for (size_t i = 0; i < 2 * kSampleRateHz; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        const float env = std::sin(2.0f * kPi * 4.0f * t) * 0.5f + 0.5f;
        const float s = 0.30f * env * (
            std::sin(2.0f * kPi *  300.0f * t) +
            std::sin(2.0f * kPi *  700.0f * t) * 0.6f +
            std::sin(2.0f * kPi * 1500.0f * t) * 0.3f
        );
        v.push_back(s);
    }
    return v;
}

}  // namespace

TEST(PipelineTest, ResultJsonSchemaIsComplete) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    ASSERT_EQ(sentlog_push_audio_frame(h, audio.data(), audio.size()),
              SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    ASSERT_TRUE(cap.called);
    ASSERT_EQ(cap.status, SENTLOG_OK) << cap.result_json;

    const json j = json::parse(cap.result_json);
    // Required top-level fields per spec §3.2.
    for (const auto& key : {"schema_version", "transcript", "liveness",
                            "presence", "direction", "confidence",
                            "thresholds_met", "engagement", "fingerprint",
                            "model_versions", "diagnostics",
                            "evidence_grade", "extensions"}) {
        EXPECT_TRUE(j.contains(key)) << "missing key: " << key;
    }
    EXPECT_EQ(j["schema_version"], "1");
    EXPECT_EQ(j["evidence_grade"], "DeviceUnattested");
    EXPECT_EQ(j["engagement"]["model_status"], "placeholder");
    EXPECT_EQ(j["transcript"]["language"], "en");

    sentlog_destroy(h);
}

TEST(PipelineTest, SpeechAudioYieldsSpeechDiagnostics) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_FALSE(j["diagnostics"].contains("failure_reason"));
    EXPECT_GT(j["diagnostics"]["duration_ms"].get<int>(), 1500);
    EXPECT_TRUE(j["diagnostics"].contains("snr_db"));
    EXPECT_GT(j["diagnostics"]["snr_db"].get<float>(), 6.0f);
    EXPECT_NE(j["diagnostics"]["audio_quality"], "very_noisy");

    sentlog_destroy(h);
}

TEST(PipelineTest, SilentAudioReportsNoSpeechDetected) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_silence_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["diagnostics"]["failure_reason"], "NoSpeechDetected");
    EXPECT_EQ(j["diagnostics"]["audio_quality"], "very_noisy");
    EXPECT_EQ(j["confidence"], 0.0f);

    sentlog_destroy(h);
}

TEST(PipelineTest, TooShortAudioReportsTooShort) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    // Push only 100ms of audio (below min_duration_ms 500).
    std::vector<float> audio(kSampleRateHz / 10, 0.5f);
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["diagnostics"]["failure_reason"], "TooShort");

    sentlog_destroy(h);
}

TEST(PipelineTest, EvaluateOutsideCaptureFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.status, SENTLOG_ERR_INVALID_STATE);

    sentlog_destroy(h);
}

TEST(PipelineTest, EvaluateAfterCancelFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    sentlog_cancel(h);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    EXPECT_EQ(cap.status, SENTLOG_ERR_INVALID_STATE);

    sentlog_destroy(h);
}
