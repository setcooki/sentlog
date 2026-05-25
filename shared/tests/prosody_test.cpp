#include "internal/prosody.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using sentlog::internal::extract_prosody;
using sentlog::internal::prosody_algo;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kSampleRateHz = 16000;

// A sustained 200 Hz sine — clean voiced signal, constant pitch +
// amplitude. Should produce a low voice_score (no variability).
std::vector<float> sine(float hz, float seconds, float amp = 0.4f) {
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        v[i] = amp * std::sin(2.0f * kPi * hz * t);
    }
    return v;
}

// FM + AM modulated tone — pitch and amplitude vary over time,
// approximating an "engaged" speaker's contour.
std::vector<float> animated_voice(float seconds) {
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        const float t  = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        const float f0 = 180.0f + 60.0f * std::sin(2.0f * kPi * 0.7f * t);  // 120..240 Hz
        const float amp_env = 0.3f + 0.2f * std::sin(2.0f * kPi * 1.5f * t);
        v[i] = amp_env * std::sin(2.0f * kPi * f0 * t);
    }
    return v;
}
}  // namespace

TEST(ProsodyTest, AlgoNameIsStable) {
    EXPECT_STREQ(prosody_algo(), "sentlog-prosody-v1");
}

TEST(ProsodyTest, EmptyAudioReturnsNoSignal) {
    auto f = extract_prosody(nullptr, 0);
    EXPECT_FALSE(f.has_signal);
    EXPECT_FLOAT_EQ(f.voice_score, 0.0f);
}

TEST(ProsodyTest, SilenceReturnsNoSignal) {
    std::vector<float> silence(kSampleRateHz, 0.0f);
    auto f = extract_prosody(silence.data(), silence.size());
    EXPECT_FALSE(f.has_signal);
}

TEST(ProsodyTest, SustainedToneDetectsF0NearTarget) {
    auto v = sine(200.0f, 1.0f);
    auto f = extract_prosody(v.data(), v.size());
    ASSERT_TRUE(f.has_signal);
    EXPECT_NEAR(f.mean_f0_hz, 200.0f, 10.0f)
        << "autocorrelation should land within 10 Hz of the target";
    EXPECT_GT(f.voiced_frame_ratio, 0.9f)
        << "a clean sine should be voiced on nearly every frame";
}

TEST(ProsodyTest, MonotoneSignalScoresLow) {
    // Constant pitch + constant amplitude → low variability → low
    // voice_score.
    auto v = sine(200.0f, 1.5f);
    auto f = extract_prosody(v.data(), v.size());
    ASSERT_TRUE(f.has_signal);
    EXPECT_LT(f.voice_score, 0.2f);
}

TEST(ProsodyTest, AnimatedSignalScoresHigher) {
    // Pitch sweep + amplitude envelope → higher variability →
    // higher voice_score than the monotone case.
    auto v = animated_voice(2.5f);
    auto f = extract_prosody(v.data(), v.size());
    ASSERT_TRUE(f.has_signal);
    EXPECT_GT(f.voice_score, 0.3f);
    EXPECT_LE(f.voice_score, 1.0f);
}

TEST(ProsodyTest, ScoreClampedToUnitInterval) {
    // Extreme amplitude swing — voice_score must still clamp at 1.0.
    const size_t n = 2 * kSampleRateHz;
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        const float f0 = 100.0f + 250.0f * std::sin(2.0f * kPi * 0.5f * t);
        const float amp_env = 0.05f + 0.4f * std::sin(2.0f * kPi * 0.5f * t);
        v[i] = amp_env * std::sin(2.0f * kPi * f0 * t);
    }
    auto f = extract_prosody(v.data(), v.size());
    if (f.has_signal) {
        EXPECT_GE(f.voice_score, 0.0f);
        EXPECT_LE(f.voice_score, 1.0f);
    }
}

TEST(ProsodyTest, DeterministicOnSameInput) {
    auto v = sine(250.0f, 1.0f);
    const auto a = extract_prosody(v.data(), v.size());
    const auto b = extract_prosody(v.data(), v.size());
    EXPECT_EQ(a.has_signal, b.has_signal);
    EXPECT_FLOAT_EQ(a.voice_score, b.voice_score);
    EXPECT_FLOAT_EQ(a.mean_f0_hz, b.mean_f0_hz);
}
