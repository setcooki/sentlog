// Golden fixtures: deterministic synthetic clips run through the public C
// ABI, with results pinned in golden/expected.json. They catch FFI bugs and
// platform drift. They are NOT recordings of people, so they say nothing
// about how well the scores work on real speech.
//
// After an intentional scoring change:
//   SENTLOG_UPDATE_FIXTURES=1 ./tests/sentlog_tests --gtest_filter='GoldenTest.*'

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

constexpr float kPi = 3.14159265f;
constexpr size_t kRate = 16000;
// Wide enough for libm differences between platforms, far narrower than
// any real change in scoring.
constexpr float kTolerance = 0.01f;

struct AudioClip {
    std::string name;
    size_t seconds;
    std::function<float(float t, std::mt19937& rng)> sample;
};

float voiced(float t, float f0, float amp, float mod) {
    const float env = 1.0f - mod + mod * std::sin(2.0f * kPi * 4.0f * t);
    return amp * env * (std::sin(2.0f * kPi * f0 * t) +
                        0.6f * std::sin(2.0f * kPi * 2.3f * f0 * t) +
                        0.3f * std::sin(2.0f * kPi * 5.0f * f0 * t));
}

// mt19937's output is fixed by the standard but <random> distributions'
// aren't, so map it by hand to keep clips identical across platforms.
float noise(std::mt19937& rng, float amp) {
    return amp * (static_cast<float>(rng()) / 2147483648.0f - 1.0f);
}

const std::vector<AudioClip>& audio_clips() {
    static const std::vector<AudioClip> clips = {
        {"silence", 2, [](float, std::mt19937&) { return 0.0f; }},
        {"short_burst", 1, [](float t, std::mt19937&) { return voiced(t, 200.0f, 0.2f, 0.5f); }},
        {"voiced_low", 3, [](float t, std::mt19937&) { return voiced(t, 120.0f, 0.2f, 0.5f); }},
        {"voiced_high", 3, [](float t, std::mt19937&) { return voiced(t, 280.0f, 0.2f, 0.5f); }},
        {"voiced_flat", 3, [](float t, std::mt19937&) { return voiced(t, 200.0f, 0.15f, 0.3f); }},
        {"gliding_pitch", 3, [](float t, std::mt19937&) {
             return voiced(t, 140.0f + 40.0f * t, 0.2f, 0.5f); }},
        {"pure_tone", 3, [](float t, std::mt19937&) {
             return 0.3f * std::sin(2.0f * kPi * 440.0f * t); }},
        {"white_noise", 3, [](float, std::mt19937& rng) {
             return noise(rng, 0.2f); }},
        {"voice_in_noise", 3, [](float t, std::mt19937& rng) {
             return voiced(t, 180.0f, 0.2f, 0.5f) +
                    noise(rng, 0.05f); }},
    };
    return clips;
}

struct Variant {
    std::string name;
    bool motion;
    bool face;
    int liveness;  // percent, or -1 for none
};

const std::vector<Variant>& variants() {
    static const std::vector<Variant> v = {
        {"audio", false, false, -1},
        {"audio_live", false, false, 90},
        {"motion", true, false, 90},
        {"face", false, true, 90},
        {"all", true, true, 90},
        {"all_spoof", true, true, 5},
    };
    return v;
}

json run(const AudioClip& clip, const Variant& variant) {
    json config = {{"issuer", "did:example:golden"},
                   {"min_duration_ms", 1500},
                   {"target_phrase", "please confirm your name"},
                   {"use_motion_sensors", variant.motion},
                   {"use_video_affect", variant.face}};
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(config.dump().c_str(), nullptr, nullptr, &status);
    EXPECT_NE(h, nullptr);
    EXPECT_EQ(sentlog_start_capture(h), SENTLOG_OK);

    std::mt19937 rng(1234);
    std::vector<float> audio(clip.seconds * kRate);
    for (size_t i = 0; i < audio.size(); ++i) {
        audio[i] = clip.sample(static_cast<float>(i) / kRate, rng);
    }
    sentlog_push_audio_frame(h, audio.data(), audio.size());

    EXPECT_EQ(sentlog_push_transcript(
                  h, R"({"text":"please confirm your name","language":"en","confidence":0.9})"),
              SENTLOG_OK);
    if (variant.liveness >= 0) {
        const float score = static_cast<float>(variant.liveness) / 100.0f;
        EXPECT_EQ(sentlog_push_liveness_result(h, score, 1.0f - score, 0.0f), SENTLOG_OK);
    }
    if (variant.motion) {
        for (int i = 0; i < 100; ++i) {
            const float t = static_cast<float>(i) * 0.02f;
            sentlog_push_motion_frame(h, static_cast<int64_t>(i) * 20000000,
                                      0.2f * std::sin(6.0f * t), 0.1f * std::cos(5.0f * t),
                                      9.8f, 0.05f, 0.02f, 0.01f);
        }
    }
    if (variant.face) {
        for (int i = 0; i < 10; ++i) {
            sentlog_push_face_frame(h, static_cast<int64_t>(i) * 33000000, 0.4f, 0.6f, "AU12");
        }
    }

    std::string out;
    sentlog_evaluate(h, [](sentlog_status_t, const char* j, void* ud) {
        if (j != nullptr) *static_cast<std::string*>(ud) = j;
    }, &out);
    sentlog_destroy(h);
    return json::parse(out);
}

json summarize(const json& r) {
    const auto& diag = r["diagnostics"];
    json s = {
        {"liveness", r["liveness"]["score"]},
        {"presence", r["presence"]["summary"]},
        {"engagement", r["engagement"]["score"]},
        {"engagement_status", r["engagement"]["model_status"]},
        {"direction", r["direction"]},
        {"confidence", r["confidence"]},
        {"thresholds_met", r["thresholds_met"]},
        {"duration_ms", diag["duration_ms"]},
        {"failure_reason", diag.value("failure_reason", json())},
        {"has_affect", !r["affect"].is_null()},
        {"has_motion", r["presence"].contains("motion")},
    };
    return s;
}

}  // namespace

TEST(GoldenTest, ClipsMatchExpected) {
    const std::string path = std::string(SENTLOG_REPO_ROOT) + "/shared/tests/golden/expected.json";

    json actual = json::object();
    for (const auto& clip : audio_clips()) {
        for (const auto& variant : variants()) {
            actual[clip.name + "/" + variant.name] = summarize(run(clip, variant));
        }
    }

    if (std::getenv("SENTLOG_UPDATE_FIXTURES") != nullptr) {
        std::ofstream(path) << actual.dump(2) << "\n";
    }
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open()) << path;
    const json expected = json::parse(file);
    ASSERT_EQ(actual.size(), expected.size());

    for (auto it = expected.begin(); it != expected.end(); ++it) {
        ASSERT_TRUE(actual.contains(it.key())) << it.key();
        const json& got = actual[it.key()];
        for (auto field = it.value().begin(); field != it.value().end(); ++field) {
            const std::string label = it.key() + " " + field.key();
            if (field.value().is_number_float()) {
                EXPECT_NEAR(got[field.key()].get<float>(), field.value().get<float>(), kTolerance)
                    << label;
            } else {
                EXPECT_EQ(got[field.key()], field.value()) << label;
            }
        }
    }
}
