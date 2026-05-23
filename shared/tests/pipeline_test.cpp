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

// --- Pushed VAD path (platform-side ONNX inference, per ADR 0009) ---

TEST(PipelineTest, PushedVadFramesAreUsedWhenPresent) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    // Push audio AND push speech probabilities for the speech region.
    // 16000 samples = 1 s; we push 32 chunks of 512 samples (one
    // second of audio classified entirely as speech by "platform"
    // inference).
    auto audio = make_speech_2s();
    ASSERT_EQ(sentlog_push_audio_frame(h, audio.data(), audio.size()),
              SENTLOG_OK);
    for (size_t start = 0; start + 512 <= audio.size(); start += 512) {
        ASSERT_EQ(sentlog_push_vad_frame(h, start, 512, 0.95f), SENTLOG_OK);
    }

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    ASSERT_EQ(cap.status, SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    // model_versions.vad reflects which path ran.
    EXPECT_EQ(j["model_versions"]["vad"], "silero-vad-v5-platform-onnx");
    EXPECT_FALSE(j["diagnostics"].contains("failure_reason"));
    EXPECT_GT(j["diagnostics"]["snr_db"].get<float>(), 0.0f);

    sentlog_destroy(h);
}

TEST(PipelineTest, NoPushedVadFallsBackToEnergyVad) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    // Deliberately no sentlog_push_vad_frame calls.

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_EQ(j["model_versions"]["vad"], "energy-vad-baseline");

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedVadAllSilentFlagsNoSpeech) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();  // loud audio
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    // Push probabilities all below threshold — Silero says "no speech"
    // even though the audio is loud. The pipeline trusts the pushed
    // result over its own energy estimate (ADR 0009).
    for (size_t start = 0; start + 512 <= audio.size(); start += 512) {
        ASSERT_EQ(sentlog_push_vad_frame(h, start, 512, 0.10f), SENTLOG_OK);
    }

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_EQ(j["diagnostics"]["failure_reason"], "NoSpeechDetected");
    EXPECT_EQ(j["model_versions"]["vad"], "silero-vad-v5-platform-onnx");

    sentlog_destroy(h);
}

TEST(PipelineTest, PushVadOutsideCaptureFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_push_vad_frame(h, 0, 512, 0.5f),
              SENTLOG_ERR_INVALID_STATE);

    sentlog_destroy(h);
}

// --- Pushed liveness (platform-side AASIST-L inference, per ADR 0009) ---

TEST(PipelineTest, PushedLivenessReplacesPlaceholder) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    ASSERT_EQ(sentlog_push_liveness_result(h, 0.92f, 0.03f, 0.05f),
              SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_FLOAT_EQ(j["liveness"]["score"].get<float>(),          0.92f);
    EXPECT_FLOAT_EQ(j["liveness"]["replay_risk"].get<float>(),    0.03f);
    EXPECT_FLOAT_EQ(j["liveness"]["synthetic_risk"].get<float>(), 0.05f);
    EXPECT_EQ(j["model_versions"]["liveness"], "aasist-l-platform-onnx");

    sentlog_destroy(h);
}

TEST(PipelineTest, NoPushedLivenessKeepsPlaceholderAndFlagsIt) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_FLOAT_EQ(j["liveness"]["score"].get<float>(), 0.5f);
    EXPECT_EQ(j["model_versions"]["liveness"], "placeholder-not-loaded");

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedLivenessClampedToValidRange) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    // Out-of-range inputs — the C++ side clamps to [0,1] so the
    // pipeline can't emit nonsense to integrators.
    ASSERT_EQ(sentlog_push_liveness_result(h, 2.0f, -0.5f, 1.5f),
              SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["liveness"]["score"].get<float>(),          1.0f);
    EXPECT_FLOAT_EQ(j["liveness"]["replay_risk"].get<float>(),    0.0f);
    EXPECT_FLOAT_EQ(j["liveness"]["synthetic_risk"].get<float>(), 1.0f);

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedLivenessLastWriteWins) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    sentlog_push_liveness_result(h, 0.10f, 0.90f, 0.80f);  // first
    sentlog_push_liveness_result(h, 0.95f, 0.02f, 0.03f);  // second wins

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["liveness"]["score"].get<float>(), 0.95f);

    sentlog_destroy(h);
}

TEST(PipelineTest, PushLivenessOutsideCaptureFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(sentlog_push_liveness_result(h, 0.5f, 0.0f, 0.0f),
              SENTLOG_ERR_INVALID_STATE);
    sentlog_destroy(h);
}

// --- Audio route → presence.audio_source mapping (chunk 1f) ---

static void run_with_route(sentlog_audio_route_t route,
                           Capture& cap) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_set_audio_route(h, route), SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    sentlog_destroy(h);
}

TEST(PipelineTest, AudioSourceFromBuiltInMicIsOne) {
    Capture cap;
    run_with_route(SENTLOG_AUDIO_ROUTE_BUILT_IN, cap);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["presence"]["audio_source"].get<float>(), 1.0f);
}

TEST(PipelineTest, AudioSourceFromUsbIsLow) {
    Capture cap;
    run_with_route(SENTLOG_AUDIO_ROUTE_USB, cap);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["presence"]["audio_source"].get<float>(), 0.2f);
}

TEST(PipelineTest, AudioSourceFromBluetoothIsLow) {
    Capture cap;
    run_with_route(SENTLOG_AUDIO_ROUTE_BLUETOOTH, cap);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["presence"]["audio_source"].get<float>(), 0.2f);
}

TEST(PipelineTest, AudioSourceFromLoopbackIsZero) {
    Capture cap;
    run_with_route(SENTLOG_AUDIO_ROUTE_LOOPBACK, cap);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["presence"]["audio_source"].get<float>(), 0.0f);
}

TEST(PipelineTest, SnapshotAudioReturnsCapturedSamplesWithoutDraining) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // Snapshot doesn't consume — call it twice.
    std::vector<float> snap1(audio.size(), 0.0f);
    std::vector<float> snap2(audio.size(), 0.0f);
    size_t n1 = 0, n2 = 0;
    ASSERT_EQ(sentlog_snapshot_audio(h, snap1.data(), snap1.size(), &n1),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_snapshot_audio(h, snap2.data(), snap2.size(), &n2),
              SENTLOG_OK);
    EXPECT_EQ(n1, audio.size());
    EXPECT_EQ(n2, audio.size());
    EXPECT_EQ(snap1[0], audio[0]);
    EXPECT_EQ(snap1[audio.size() - 1], audio[audio.size() - 1]);

    // Pipeline still drains the audio in evaluate.
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FALSE(j["diagnostics"].contains("failure_reason"))
        << "evaluate should still see the audio after a snapshot";

    sentlog_destroy(h);
}

TEST(PipelineTest, SnapshotAudioRespectsCallerCapacity) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    std::vector<float> audio(8000, 0.5f);
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    std::vector<float> small(1000, 0.0f);
    size_t n = 0;
    ASSERT_EQ(sentlog_snapshot_audio(h, small.data(), small.size(), &n),
              SENTLOG_OK);
    EXPECT_EQ(n, 1000u);  // capped at caller capacity, not what's available

    sentlog_destroy(h);
}

// --- Pushed transcript (platform-side whisper.cpp, chunk 2a) ---

TEST(PipelineTest, PushedTranscriptReplacesPlaceholder) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({
        "text":       "hello world",
        "language":   "en",
        "confidence": 0.91,
        "word_timings": [
            {"word": "hello", "start_ms": 100, "end_ms": 450},
            {"word": "world", "start_ms": 480, "end_ms": 820}
        ]
    })";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["transcript"]["text"],     "hello world");
    EXPECT_EQ(j["transcript"]["language"], "en");
    EXPECT_FLOAT_EQ(j["transcript"]["confidence"].get<float>(), 0.91f);
    ASSERT_EQ(j["transcript"]["word_timings"].size(), 2u);
    EXPECT_EQ(j["transcript"]["word_timings"][0]["word"],         "hello");
    EXPECT_EQ(j["transcript"]["word_timings"][0]["start_ms"],     100);
    EXPECT_EQ(j["transcript"]["word_timings"][1]["end_ms"],       820);
    EXPECT_TRUE(j["transcript"]["target_match"].is_null())
        << "target_match lands in chunk 2d, not 2a";
    EXPECT_EQ(j["model_versions"]["asr"], "whisper-cpp-platform");

    sentlog_destroy(h);
}

TEST(PipelineTest, NoPushedTranscriptKeepsPlaceholderAndFlagsIt) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["transcript"]["text"],       "");
    EXPECT_FALSE(j["transcript"].contains("word_timings"));
    EXPECT_TRUE(j["transcript"]["target_match"].is_null());
    EXPECT_EQ(j["model_versions"]["asr"], "placeholder-not-loaded");

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedTranscriptWithoutWordTimingsIsAllowed) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"hi","language":"en","confidence":0.5})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_EQ(j["transcript"]["text"], "hi");
    EXPECT_FALSE(j["transcript"].contains("word_timings"));

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedTranscriptConfidenceClampedToValidRange) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"x","language":"en","confidence":2.5})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["transcript"]["confidence"].get<float>(), 1.0f);

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedTranscriptMalformedJsonRejected) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);

    EXPECT_EQ(sentlog_push_transcript(h, "{not json"),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(sentlog_push_transcript(h, R"({"text":"x"})"),
              SENTLOG_ERR_INVALID_CONFIG)   // missing required fields
        << "missing language and confidence";
    EXPECT_EQ(sentlog_push_transcript(h, nullptr),
              SENTLOG_ERR_INVALID_CONFIG);

    sentlog_destroy(h);
}

TEST(PipelineTest, PushedTranscriptLastWriteWins) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    sentlog_push_transcript(h, R"({"text":"first","language":"en","confidence":0.1})");
    sentlog_push_transcript(h, R"({"text":"second","language":"en","confidence":0.9})");

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_EQ(j["transcript"]["text"], "second");

    sentlog_destroy(h);
}

TEST(PipelineTest, PushTranscriptOutsideCaptureFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(sentlog_push_transcript(h, R"({"text":"x","language":"en","confidence":0.5})"),
              SENTLOG_ERR_INVALID_STATE);
    sentlog_destroy(h);
}

TEST(PipelineTest, NoRoutePushedDefaultsToZero) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["presence"]["audio_source"].get<float>(), 0.0f);

    sentlog_destroy(h);
}
