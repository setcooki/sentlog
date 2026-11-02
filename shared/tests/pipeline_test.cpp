// End-to-end pipeline tests through the C ABI. Push synthetic audio
// into a created handle, evaluate, parse the JSON result, validate
// the schema and diagnostic contents.

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
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
    // Phase 3d: synthetic speech fixture has detectable F0, so the
    // prosody extractor runs and model_status flips to "training".
    EXPECT_EQ(j["engagement"]["model_status"], "training");
    EXPECT_EQ(j["transcript"]["language"], "en");
    // Phase 3a: fingerprint always computed; 2 s of speech is well
    // above the two-frame minimum.
    EXPECT_EQ(j["fingerprint"]["algo"], "sentlog-pcp-v1");
    EXPECT_FALSE(j["fingerprint"]["bytes"].get<std::string>().empty());
    // Phase 3d: prosody extractor populated model_versions.prosody.
    EXPECT_EQ(j["model_versions"]["prosody"], "sentlog-prosody-v1");

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

// --- Direction scoring against target_phrase (chunk 2d) ---

namespace {
const char* kConfigWithTargetPhrase =
    R"({"issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "target_phrase": "please confirm your name",
        "direction_threshold": 0.85})";
}  // namespace

TEST(PipelineTest, DirectionMatchesTargetPhraseExactly) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithTargetPhrase, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"Please confirm your name.","language":"en","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);
    ASSERT_EQ(sentlog_push_liveness_result(h, 0.9f, 0.0f, 0.0f), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_FLOAT_EQ(j["transcript"]["target_match"].get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(j["direction"].get<float>(),                  1.0f);
    EXPECT_TRUE(j["thresholds_met"].get<bool>())
        << "every score clearing its threshold should mark thresholds_met";

    sentlog_destroy(h);
}

namespace {

// Runs an exact-phrase capture, which clears direction_threshold, and
// returns thresholds_met. `liveness` < 0 skips pushing a liveness result.
bool thresholds_met_for(const std::string& extra_config,
                        const std::vector<float>& audio,
                        float liveness) {
    const std::string config =
        R"({"issuer": "did:example:integrator",
            "min_duration_ms": 500,
            "target_phrase": "please confirm your name",)" + extra_config + "}";
    sentlog_status_t s;
    auto* h = sentlog_create(config.c_str(), nullptr, nullptr, &s);
    EXPECT_NE(h, nullptr);
    EXPECT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    EXPECT_EQ(sentlog_push_transcript(
                  h, R"({"text":"please confirm your name","language":"en","confidence":0.9})"),
              SENTLOG_OK);
    if (liveness >= 0.0f) {
        EXPECT_EQ(sentlog_push_liveness_result(h, liveness, 0.0f, 0.0f),
                  SENTLOG_OK);
    }
    Capture cap;
    EXPECT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    sentlog_destroy(h);
    return json::parse(cap.result_json)["thresholds_met"].get<bool>();
}

}  // namespace

TEST(PipelineTest, ThresholdsMetBaselinePasses) {
    EXPECT_TRUE(thresholds_met_for(R"("min_confidence": 0.6)",
                                   make_speech_2s(), 0.9f));
}

TEST(PipelineTest, ThresholdsMetFailsOnLowLiveness) {
    EXPECT_FALSE(thresholds_met_for(R"("min_confidence": 0.6)",
                                    make_speech_2s(), 0.01f));
}

TEST(PipelineTest, ThresholdsMetFailsOnPlaceholderLiveness) {
    // The 0.5 placeholder clears this threshold numerically but no
    // model produced it.
    EXPECT_FALSE(thresholds_met_for(R"("liveness_threshold": 0.3)",
                                    make_speech_2s(), -1.0f));
}

TEST(PipelineTest, ThresholdsMetFailsOnLowEngagement) {
    // Gentle amplitude modulation counts as speech but scores low on
    // prosody. The first check shows engagement alone decides it.
    std::vector<float> flat_speech(2 * kSampleRateHz);
    for (size_t i = 0; i < flat_speech.size(); ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        const float env = 0.7f + 0.3f * std::sin(2.0f * kPi * 4.0f * t);
        flat_speech[i] = 0.15f * env * (
            std::sin(2.0f * kPi *  300.0f * t) +
            std::sin(2.0f * kPi *  700.0f * t) * 0.6f +
            std::sin(2.0f * kPi * 1500.0f * t) * 0.3f);
    }
    EXPECT_TRUE(thresholds_met_for(R"("engagement_threshold": 0.0)",
                                   flat_speech, 0.9f));
    EXPECT_FALSE(thresholds_met_for(R"("engagement_threshold": 0.5)",
                                    flat_speech, 0.9f));
}

TEST(PipelineTest, ThresholdsMetFailsOnPlaceholderEngagement) {
    // Silence gives no prosody, so engagement is the 0.65 placeholder.
    // min_confidence 0 keeps that the only failing check.
    const std::vector<float> silence(32000, 0.0f);
    EXPECT_FALSE(thresholds_met_for(
        R"("engagement_threshold": 0.1, "min_confidence": 0.0)", silence, 0.9f));
}

TEST(PipelineTest, ThresholdsMetFailsOnLowConfidence) {
    EXPECT_FALSE(thresholds_met_for(R"("min_confidence": 0.9)",
                                    make_speech_2s(), 0.9f));
}

TEST(PipelineTest, DirectionPartialMatchScoresBelowOne) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithTargetPhrase, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // Off-phrase but related — should land mid-range, well below 0.85.
    const char* t = R"({"text":"the weather is nice today","language":"en","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    const float dir = j["direction"].get<float>();
    EXPECT_LT(dir, 0.5f);
    EXPECT_GE(dir, 0.0f);
    EXPECT_FLOAT_EQ(j["transcript"]["target_match"].get<float>(), dir);
    EXPECT_FALSE(j["thresholds_met"].get<bool>());

    sentlog_destroy(h);
}

TEST(PipelineTest, NoTargetPhraseLeavesMatchNullAndDirectionZero) {
    // kValidConfig has no target_phrase. The transcript is pushed,
    // but there's nothing to compare against.
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"some words","language":"en","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_TRUE(j["transcript"]["target_match"].is_null());
    EXPECT_FLOAT_EQ(j["direction"].get<float>(), 0.0f);
    EXPECT_FALSE(j["thresholds_met"].get<bool>())
        << "no target_phrase configured → nothing to assert against";

    sentlog_destroy(h);
}

TEST(PipelineTest, NoPushedTranscriptLeavesMatchNullEvenWithTargetPhrase) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithTargetPhrase, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // No push_transcript call.
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_TRUE(j["transcript"]["target_match"].is_null());
    EXPECT_FLOAT_EQ(j["direction"].get<float>(), 0.0f);
    EXPECT_FALSE(j["thresholds_met"].get<bool>());

    sentlog_destroy(h);
}

TEST(PipelineTest, ThresholdsMetHonorsConfiguredThreshold) {
    // Same fixture as the exact-match test but at the boundary —
    // a near-miss that lands above the threshold should still pass.
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithTargetPhrase, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // Small typo — should still be above 0.85.
    const char* t = R"({"text":"please confirm your nme","language":"en","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);
    ASSERT_EQ(sentlog_push_liveness_result(h, 0.9f, 0.0f, 0.0f), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    const float dir = j["direction"].get<float>();
    EXPECT_GE(dir, 0.85f);
    EXPECT_LT(dir, 1.0f);
    EXPECT_TRUE(j["thresholds_met"].get<bool>());

    sentlog_destroy(h);
}

// --- Motion-coherence (chunk 3c) ---

namespace {
const char* kConfigWithMotion =
    R"({"issuer": "did:example:x",
        "min_duration_ms": 500,
        "use_motion_sensors": true})";

// Helper: push N motion frames simulating a held device (small
// per-axis variation around gravity on Y).
void push_held_motion(sentlog_handle_t* h, size_t n, float sigma) {
    std::mt19937 rng(0xBEEF);
    std::normal_distribution<float> jitter(0.0f, sigma);
    for (size_t i = 0; i < n; ++i) {
        const float ax = jitter(rng);
        const float ay = 9.81f + jitter(rng);  // gravity dominates Y
        const float az = jitter(rng);
        sentlog_push_motion_frame(h,
            static_cast<int64_t>(i) * 10'000'000,  // 100 Hz spacing
            ax, ay, az, 0.0f, 0.0f, 0.0f);
    }
}
}  // namespace

TEST(PipelineTest, MotionAbsentLeavesPresenceMotionUnset) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithMotion, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FALSE(j["presence"].contains("motion"))
        << "no pushed frames → motion stays absent";
    sentlog_destroy(h);
}

TEST(PipelineTest, MotionStationaryDeviceScoresLow) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithMotion, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // Phone on a table: no motion variance.
    push_held_motion(h, 200, /* sigma = */ 0.0f);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    ASSERT_TRUE(j["presence"].contains("motion"));
    EXPECT_FLOAT_EQ(j["presence"]["motion"].get<float>(), 0.0f);
    sentlog_destroy(h);
}

TEST(PipelineTest, MotionHeldDeviceScoresHigher) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithMotion, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // Held device: small per-axis variance simulating hand tremor.
    push_held_motion(h, 200, /* sigma = */ 0.20f);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    ASSERT_TRUE(j["presence"].contains("motion"));
    const float m = j["presence"]["motion"].get<float>();
    EXPECT_GT(m, 0.4f) << "held-device sigma should land mid-to-high";
    EXPECT_LE(m, 1.0f);
    sentlog_destroy(h);
}

TEST(PipelineTest, MotionSummaryWeightsAudioSourceAndMotion) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithMotion, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_set_audio_route(h, SENTLOG_AUDIO_ROUTE_BUILT_IN),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    push_held_motion(h, 200, /* sigma = */ 0.20f);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    // built-in mic = audio_source 1.0 (weight 1.0);
    // motion ~ 1.0 at sigma 0.20 (weight 0.7).
    // summary = (1.0*1.0 + ~1.0*0.7) / (1.0+0.7) ≈ ~1.0.
    const float summary = j["presence"]["summary"].get<float>();
    EXPECT_GT(summary, 0.85f);
    EXPECT_LE(summary, 1.0f);
    sentlog_destroy(h);
}

TEST(PipelineTest, MotionPushOutsideCaptureFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithMotion, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(sentlog_push_motion_frame(h, 0, 0, 0, 0, 0, 0, 0),
              SENTLOG_ERR_INVALID_STATE);
    sentlog_destroy(h);
}

TEST(PipelineTest, CustomScoringWeightsShiftPresenceSummary) {
    // Phase 3 exit-criterion test: a custom scoring_weights override
    // changes the composite presence.summary. Two captures with
    // identical inputs, opposite weight extremes, should produce
    // visibly different summaries when both audio_source and motion
    // are populated.
    auto run_with = [](const char* cfg_json) -> float {
        sentlog_status_t s;
        auto* h = sentlog_create(cfg_json, nullptr, nullptr, &s);
        EXPECT_NE(h, nullptr);
        EXPECT_EQ(sentlog_set_audio_route(h, SENTLOG_AUDIO_ROUTE_BUILT_IN),
                  SENTLOG_OK);
        EXPECT_EQ(sentlog_start_capture(h), SENTLOG_OK);
        auto audio = make_speech_2s();
        sentlog_push_audio_frame(h, audio.data(), audio.size());
        push_held_motion(h, 200, /* sigma = */ 0.20f);   // motion ~1.0
        Capture cap;
        EXPECT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
        const json j = json::parse(cap.result_json);
        sentlog_destroy(h);
        return j["presence"]["summary"].get<float>();
    };

    // Defaults: presence.audio_source weight 1.0, presence.motion 0.7.
    // built-in mic = 1.0, motion ~1.0 → summary ~ (1*1 + 1*0.7)/(1+0.7) ≈ 1.0.
    const float defaults_summary = run_with(
        R"({"issuer":"did:example:x","min_duration_ms":500,
            "use_motion_sensors":true})");

    // Heavily down-weight audio_source. Motion (~1.0) should now
    // dominate; summary stays high but the audio_source contribution
    // collapses → summary still ~1.0 since both are ~1.0. Inverse
    // case is more visible: down-weight motion so audio_source
    // dominates and the held-device motion stops boosting the score.
    // Use the more discriminating case: built-in mic AND zero motion,
    // with motion weight cranked up so motion of 0 drags summary down.
    auto run_no_motion = [](float motion_weight) -> float {
        sentlog_status_t s;
        const std::string cfg = std::string(
            R"({"issuer":"did:example:x","min_duration_ms":500,
                "use_motion_sensors":true,
                "scoring_weights":{"presence.motion":)")
            + std::to_string(motion_weight) + "}}";
        auto* h = sentlog_create(cfg.c_str(), nullptr, nullptr, &s);
        EXPECT_NE(h, nullptr);
        EXPECT_EQ(sentlog_set_audio_route(h, SENTLOG_AUDIO_ROUTE_BUILT_IN),
                  SENTLOG_OK);
        EXPECT_EQ(sentlog_start_capture(h), SENTLOG_OK);
        auto audio = make_speech_2s();
        sentlog_push_audio_frame(h, audio.data(), audio.size());
        push_held_motion(h, 200, /* sigma = */ 0.0f);    // motion = 0
        Capture cap;
        EXPECT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
        const json j = json::parse(cap.result_json);
        sentlog_destroy(h);
        return j["presence"]["summary"].get<float>();
    };
    // With audio_source=1.0 and motion=0.0:
    //   weight=0.1 → summary ≈ (1*1 + 0*0.1)/(1+0.1) ≈ 0.91
    //   weight=5.0 → summary ≈ (1*1 + 0*5.0)/(1+5.0) ≈ 0.17
    const float low_motion_weight  = run_no_motion(0.1f);
    const float high_motion_weight = run_no_motion(5.0f);

    EXPECT_GT(defaults_summary, 0.85f);
    EXPECT_GT(low_motion_weight, 0.85f)
        << "stationary device + low motion weight should barely affect summary";
    EXPECT_LT(high_motion_weight, 0.3f)
        << "stationary device + high motion weight should drag summary down";
    EXPECT_GT(low_motion_weight, high_motion_weight)
        << "the scoring_weights override must reach Pipeline::run";
}

TEST(PipelineTest, MotionUseFlagOffSuppressesEvenWhenPushed) {
    sentlog_status_t s;
    // kValidConfig has use_motion_sensors == false (default).
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    push_held_motion(h, 200, /* sigma = */ 0.20f);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FALSE(j["presence"].contains("motion"))
        << "use_motion_sensors=false → motion stays absent even if frames pushed";
    sentlog_destroy(h);
}

// --- Language gating (chunk 2e) ---

namespace {
const char* kConfigEnglishHint =
    R"({"issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "language_hint": "en"})";
}  // namespace

TEST(PipelineTest, LanguageMatchSurfacesNoMismatch) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigEnglishHint, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"hello","language":"en","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["diagnostics"]["detected_language"], "en");
    EXPECT_FALSE(j["diagnostics"]["language_mismatch"].get<bool>());
    EXPECT_FALSE(j["diagnostics"].contains("failure_reason"));
    EXPECT_FLOAT_EQ(j["confidence"].get<float>(), 0.6f);

    sentlog_destroy(h);
}

TEST(PipelineTest, LanguageMismatchHalvesConfidenceAndFlags) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigEnglishHint, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"bonjour","language":"fr","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["diagnostics"]["detected_language"],         "fr");
    EXPECT_TRUE(j["diagnostics"]["language_mismatch"].get<bool>());
    EXPECT_EQ(j["diagnostics"]["failure_reason"],            "LanguageMismatch");
    EXPECT_FLOAT_EQ(j["confidence"].get<float>(), 0.3f)
        << "0.6 baseline × 0.5 mismatch penalty";

    sentlog_destroy(h);
}

TEST(PipelineTest, LanguageCaseInsensitiveMatch) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigEnglishHint, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    const char* t = R"({"text":"hi","language":"EN","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_FALSE(j["diagnostics"]["language_mismatch"].get<bool>());

    sentlog_destroy(h);
}

TEST(PipelineTest, LanguageMismatchDoesNotShadowAudioFailure) {
    // Audio is silence — NoSpeechDetected should win over
    // LanguageMismatch because the audio problem is the bigger one.
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigEnglishHint, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto silence = make_silence_2s();
    sentlog_push_audio_frame(h, silence.data(), silence.size());

    const char* t = R"({"text":"bonjour","language":"fr","confidence":0.9})";
    ASSERT_EQ(sentlog_push_transcript(h, t), SENTLOG_OK);

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_EQ(j["diagnostics"]["failure_reason"], "NoSpeechDetected")
        << "audio-quality failure outranks language mismatch";
    // language_mismatch still surfaces as a separate flag so the
    // integrator sees both signals.
    EXPECT_TRUE(j["diagnostics"]["language_mismatch"].get<bool>());

    sentlog_destroy(h);
}

TEST(PipelineTest, NoTranscriptOmitsLanguageDiagnostics) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigEnglishHint, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    // No push_transcript call.

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);

    EXPECT_FALSE(j["diagnostics"].contains("detected_language"));
    EXPECT_FALSE(j["diagnostics"].contains("language_mismatch"));
    EXPECT_FALSE(j["diagnostics"].contains("failure_reason"));

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

// --- Phase 4d1: forwarder dispatch ---

namespace {
struct DispatchCapture {
    std::vector<std::string> forwarder_names;
    std::vector<std::string> sink_kinds;
    std::vector<std::string> envelope_jsons;
};
void on_dispatch(const char* forwarder_name,
                 const char* sink_kind,
                 const char* /*sink_json*/,
                 const char* envelope_json,
                 void* userdata) {
    auto* cap = static_cast<DispatchCapture*>(userdata);
    cap->forwarder_names.push_back(forwarder_name ? forwarder_name : "");
    cap->sink_kinds.push_back(sink_kind ? sink_kind : "");
    cap->envelope_jsons.push_back(envelope_json ? envelope_json : "");
}
}  // namespace

TEST(PipelineTest, ForwarderDispatcherFiresOncePerCallbackSink) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "primary",   "projection": "Full",
              "sink": { "kind": "callback", "endpoint": "ignored" } },
            { "name": "secondary", "projection": "ScoresOnly",
              "sink": { "kind": "callback", "endpoint": "ignored" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &on_dispatch, &dcap),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);

    ASSERT_EQ(dcap.forwarder_names.size(), 2u);
    EXPECT_EQ(dcap.forwarder_names[0], "primary");
    EXPECT_EQ(dcap.forwarder_names[1], "secondary");
    EXPECT_EQ(dcap.sink_kinds[0],      "callback");
    EXPECT_EQ(dcap.sink_kinds[1],      "callback");

    // Per-projection shape sanity: secondary should be ScoresOnly,
    // i.e. no fingerprint in the payload.
    const json secondary = json::parse(dcap.envelope_jsons[1]);
    EXPECT_EQ(secondary["credentialSubject"]["projection"], "ScoresOnly");
    EXPECT_FALSE(secondary["credentialSubject"]["payload"].contains("fingerprint"));

    // No signing key registered → envelopes are unsigned.
    EXPECT_FALSE(secondary.contains("proof"));

    sentlog_destroy(h);
}

TEST(PipelineTest, ForwarderDispatcherWithSigningKeyAddsProof) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "primary", "projection": "Full",
              "sink": { "kind": "callback", "endpoint": "ignored" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    // RFC 8032 §7.1 test vector 1 seed||pubkey — see signing_test.cpp.
    const uint8_t kSk[64] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
    };
    ASSERT_EQ(sentlog_set_signing_key(h, kSk, 64, "did:key:zTest"),
              SENTLOG_OK);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &on_dispatch, &dcap),
              SENTLOG_OK);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);

    ASSERT_EQ(dcap.envelope_jsons.size(), 1u);
    const json env = json::parse(dcap.envelope_jsons[0]);
    ASSERT_TRUE(env.contains("proof"));
    EXPECT_EQ(env["proof"]["cryptosuite"],        "eddsa-jcs-2022");
    EXPECT_EQ(env["proof"]["verificationMethod"], "did:key:zTest");
    EXPECT_FALSE(env["proof"]["proofValue"].get<std::string>().empty());

    sentlog_destroy(h);
}

TEST(PipelineTest, ForwarderDispatcherSkipsLocalQueueForNow) {
    // 4d2 hooks the C++-side file writer in; 4d1's dispatcher must
    // simply skip local_queue forwarders rather than mis-route them
    // through the callback path.
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "queue", "projection": "Full",
              "sink": { "kind": "local_queue", "path": "/tmp/ignored" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &on_dispatch, &dcap),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    EXPECT_TRUE(dcap.forwarder_names.empty())
        << "local_queue forwarders shouldn't reach the dispatcher; "
           "they land in 4d2's C++-side file writer";
    sentlog_destroy(h);
}

TEST(PipelineTest, NoDispatcherRegisteredSkipsForwarderQuietly) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "primary", "projection": "Full",
              "sink": { "kind": "callback", "endpoint": "ignored" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    Capture cap;
    // No dispatcher registered — must still succeed.
    EXPECT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    sentlog_destroy(h);
}

TEST(PipelineTest, LocalQueueForwarderAppendsNdjsonToFile) {
    // Phase 4d2: local_queue sinks write the envelope as one
    // NDJSON line per evaluate so a future drainQueue can read them
    // back line-by-line.
    const std::string path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
        + "/sentlog-pipeline-test-queue.ndjson";
    std::remove(path.c_str());  // start clean

    const std::string cfg = std::string(R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "q", "projection": "ScoresOnly",
              "sink": { "kind": "local_queue", "path": ")")
        + path + R"(" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg.c_str(), nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    // Two evaluates → two NDJSON lines.
    for (int i = 0; i < 2; ++i) {
        ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
        auto audio = make_speech_2s();
        sentlog_push_audio_frame(h, audio.data(), audio.size());
        Capture cap;
        ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    }

    std::ifstream in(path);
    ASSERT_TRUE(in.is_open()) << "queue file should have been created at " << path;
    std::string line;
    int line_count = 0;
    while (std::getline(in, line)) {
        ++line_count;
        const json env = json::parse(line);
        EXPECT_EQ(env["credentialSubject"]["projection"], "ScoresOnly");
    }
    EXPECT_EQ(line_count, 2);

    std::remove(path.c_str());
    sentlog_destroy(h);
}

TEST(PipelineTest, LocalQueueBadPathSkipsSilently) {
    // Unwritable path → evaluate must still succeed; the envelope
    // is dropped silently (telemetry hook lands in 4f).
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "q", "projection": "Full",
              "sink": { "kind": "local_queue",
                        "path": "/this/path/does/not/exist/queue.ndjson" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    EXPECT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    sentlog_destroy(h);
}

// --- Phase 5a: face-affect aggregation ---

namespace {
const char* kConfigWithVideoAffect =
    R"({"issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "use_video_affect": true})";

void push_face_run(sentlog_handle_t* h,
                   size_t n,
                   float valence,
                   float arousal,
                   const char* fau) {
    for (size_t i = 0; i < n; ++i) {
        sentlog_push_face_frame(h,
            static_cast<int64_t>(i) * 100'000'000,  // 10 Hz spacing
            valence, arousal, fau);
    }
}
}  // namespace

TEST(PipelineTest, FaceAbsentLeavesAffectNull) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithVideoAffect, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_TRUE(j["affect"].is_null());
    sentlog_destroy(h);
}

TEST(PipelineTest, FaceFramesPopulateAffectBlock) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithVideoAffect, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    // Steady "happy" affect — high valence, mid arousal, AU06+AU12.
    push_face_run(h, 20, /*v=*/0.7f, /*a=*/0.5f, "AU06,AU12");

    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    ASSERT_FALSE(j["affect"].is_null());
    EXPECT_FLOAT_EQ(j["affect"]["valence"].get<float>(), 0.7f);
    EXPECT_FLOAT_EQ(j["affect"]["arousal"].get<float>(), 0.5f);
    ASSERT_TRUE(j["affect"].contains("dominant_fau"));
    EXPECT_EQ(j["affect"]["dominant_fau"].size(), 2u);
    EXPECT_EQ(j["model_versions"]["affect"], "placeholder-derived");
    sentlog_destroy(h);
}

TEST(PipelineTest, FaceScoreContributesToEngagementEnsemble) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithVideoAffect, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    // Maximum-valence affect → face_score = 1.0.
    push_face_run(h, 10, /*v=*/1.0f, /*a=*/0.5f, nullptr);
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_FLOAT_EQ(j["engagement"]["face_score"].get<float>(), 1.0f);
    EXPECT_TRUE(j["engagement"].contains("voice_score"));
    EXPECT_EQ(j["engagement"]["model_status"], "training");
    // engagement.score is a weighted mean of voice (w=1.0) + face
    // (w=0.5 default). With voice_score = whatever prosody returns
    // and face_score = 1.0, the score must land between them.
    const float vs = j["engagement"]["voice_score"].get<float>();
    const float es = j["engagement"]["score"].get<float>();
    EXPECT_GE(es, std::min(vs, 1.0f));
    EXPECT_LE(es, std::max(vs, 1.0f) + 1e-5f);
    sentlog_destroy(h);
}

TEST(PipelineTest, FaceFramesSuppressedWhenUseVideoAffectIsOff) {
    // kValidConfig has use_video_affect == false. Pushed frames are
    // accepted but the pipeline does NOT aggregate them — affect
    // stays null and engagement reverts to voice-only.
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    push_face_run(h, 5, 0.5f, 0.5f, nullptr);
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    EXPECT_TRUE(j["affect"].is_null());
    EXPECT_FALSE(j["engagement"].contains("face_score"));
    sentlog_destroy(h);
}

TEST(PipelineTest, FaceFramesClampValenceArousalToValidRange) {
    sentlog_status_t s;
    auto* h = sentlog_create(kConfigWithVideoAffect, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    // Out-of-range inputs must be clamped to [-1,1] and [0,1].
    sentlog_push_face_frame(h, 0, /*v=*/ 5.0f, /*a=*/ 2.0f, nullptr);
    sentlog_push_face_frame(h, 1, /*v=*/-5.0f, /*a=*/-1.0f, nullptr);
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    const json j = json::parse(cap.result_json);
    // Mean of clamped (1.0, -1.0) = 0.0; mean of clamped (1.0, 0.0) = 0.5.
    EXPECT_NEAR(j["affect"]["valence"].get<float>(), 0.0f, 0.01f);
    EXPECT_NEAR(j["affect"]["arousal"].get<float>(), 0.5f, 0.01f);
    sentlog_destroy(h);
}

TEST(PipelineTest, PushFaceOutsideCaptureFailsLifecycle) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(sentlog_push_face_frame(h, 0, 0.0f, 0.5f, nullptr),
              SENTLOG_ERR_INVALID_STATE);
    sentlog_destroy(h);
}

// --- Phase 4f: sampling + custom_fields + telemetry on skip ---

namespace {
struct TelemetryCapture {
    std::vector<std::string> events;
};
void on_telemetry(const char* event_json, void* userdata) {
    auto* tc = static_cast<TelemetryCapture*>(userdata);
    tc->events.push_back(event_json ? event_json : "");
}
}  // namespace

TEST(PipelineTest, SampleRateZeroSkipsDispatcherAndEmitsTelemetry) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "p", "projection": "Full", "sample_rate": 0.0,
              "sink": { "kind": "callback", "endpoint": "x" } }
        ]
    })";
    sentlog_status_t s;
    TelemetryCapture tc;
    auto* h = sentlog_create(cfg, &on_telemetry, &tc, &s);
    ASSERT_NE(h, nullptr);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &on_dispatch, &dcap),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);

    EXPECT_TRUE(dcap.forwarder_names.empty())
        << "sample_rate 0.0 must never dispatch";
    bool saw_dropped = false;
    for (const auto& ev : tc.events) {
        const auto j = json::parse(ev);
        if (j["type"] == "ForwarderOutcome" &&
            j["forwarder_name"] == "p" &&
            j["outcome"] == "dropped") {
            saw_dropped = true; break;
        }
    }
    EXPECT_TRUE(saw_dropped) << "skip must emit a telemetry event";
    sentlog_destroy(h);
}

TEST(PipelineTest, SampleRateOneAlwaysDispatches) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "p", "projection": "Full", "sample_rate": 1.0,
              "sink": { "kind": "callback", "endpoint": "x" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &on_dispatch, &dcap),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);
    EXPECT_EQ(dcap.forwarder_names.size(), 1u);
    sentlog_destroy(h);
}

TEST(PipelineTest, CustomProjectionFieldsFlowThroughDispatcher) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "custom", "projection": "Custom",
              "custom_fields": ["transcript.text", "presence.audio_source"],
              "sink": { "kind": "callback", "endpoint": "x" } }
        ]
    })";
    sentlog_status_t s;
    auto* h = sentlog_create(cfg, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_set_audio_route(h, SENTLOG_AUDIO_ROUTE_BUILT_IN),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &on_dispatch, &dcap),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);

    ASSERT_EQ(dcap.envelope_jsons.size(), 1u);
    const json env = json::parse(dcap.envelope_jsons[0]);
    EXPECT_EQ(env["credentialSubject"]["projection"], "Custom");
    const auto& payload = env["credentialSubject"]["payload"];
    // Requested fields present.
    EXPECT_TRUE(payload["transcript"].contains("text"));
    EXPECT_FLOAT_EQ(payload["presence"]["audio_source"].get<float>(), 1.0f);
    // Unrequested fields absent.
    EXPECT_FALSE(payload.contains("liveness"));
    EXPECT_FALSE(payload.contains("fingerprint"));
    EXPECT_FALSE(payload.contains("engagement"));
    sentlog_destroy(h);
}

TEST(PipelineTest, NoDispatcherEmitsForwarderOutcomeDropped) {
    const char* cfg = R"({
        "issuer": "did:example:integrator",
        "min_duration_ms": 500,
        "forwarders": [
            { "name": "p", "projection": "Full",
              "sink": { "kind": "callback", "endpoint": "x" } }
        ]
    })";
    sentlog_status_t s;
    TelemetryCapture tc;
    auto* h = sentlog_create(cfg, &on_telemetry, &tc, &s);
    ASSERT_NE(h, nullptr);

    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    auto audio = make_speech_2s();
    sentlog_push_audio_frame(h, audio.data(), audio.size());
    Capture cap;
    ASSERT_EQ(sentlog_evaluate(h, &on_result, &cap), SENTLOG_OK);

    bool saw_dropped = false;
    for (const auto& ev : tc.events) {
        const auto j = json::parse(ev);
        if (j["type"] == "ForwarderOutcome" &&
            j["outcome"] == "dropped") {
            saw_dropped = true; break;
        }
    }
    EXPECT_TRUE(saw_dropped);
    sentlog_destroy(h);
}

TEST(PipelineTest, SetSigningKeyRejectsWrongLength) {
    sentlog_status_t s;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &s);
    ASSERT_NE(h, nullptr);
    const uint8_t k[32] = {0};
    EXPECT_EQ(sentlog_set_signing_key(h, k, 32, "did:key:x"),
              SENTLOG_ERR_INVALID_CONFIG);
    sentlog_destroy(h);
}
