#include "internal/fingerprint.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <string>
#include <vector>

using sentlog::internal::compute_fingerprint;
using sentlog::internal::fingerprint_algo;

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kSampleRateHz = 16000;

std::vector<float> make_sine(float hz, float seconds, float amp = 0.5f) {
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        v[i] = amp * std::sin(2.0f * kPi * hz * t);
    }
    return v;
}

size_t hamming_distance(const std::string& a, const std::string& b) {
    const size_t n = std::min(a.size(), b.size());
    size_t d = 0;
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) ++d;
    d += (a.size() > b.size() ? a.size() - n : b.size() - n);
    return d;
}
}  // namespace

TEST(FingerprintTest, AlgoNameIsStable) {
    EXPECT_STREQ(fingerprint_algo(), "sentlog-pcp-v1");
}

TEST(FingerprintTest, NullInputReturnsEmpty) {
    EXPECT_EQ(compute_fingerprint(nullptr, 0), "");
}

TEST(FingerprintTest, TooShortReturnsEmpty) {
    // Need at least two 200 ms frames (6 400 samples) — anything
    // shorter has no temporal delta to encode.
    const std::vector<float> short_audio(1000, 0.0f);
    EXPECT_EQ(compute_fingerprint(short_audio.data(), short_audio.size()), "");
}

TEST(FingerprintTest, DeterministicForSameInput) {
    const auto audio = make_sine(440.0f, 2.0f);
    const auto a = compute_fingerprint(audio.data(), audio.size());
    const auto b = compute_fingerprint(audio.data(), audio.size());
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.empty());
}

TEST(FingerprintTest, DifferentToneProducesDifferentFingerprint) {
    const auto a = compute_fingerprint(
        make_sine(440.0f, 3.0f).data(), 3 * kSampleRateHz);
    const auto b = compute_fingerprint(
        make_sine(2000.0f, 3.0f).data(), 3 * kSampleRateHz);
    EXPECT_NE(a, b);
}

TEST(FingerprintTest, SmallNoiseDoesNotFlipMostBits) {
    // Speech-like fixture: stack three harmonics with an amplitude
    // envelope. Multiple Goertzel bands stay engaged across the
    // whole clip, so the temporal-delta sign pattern is more stable
    // than a single-tone sweep. Same shape used in pipeline_test.cpp.
    const float seconds = 4.0f;
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    std::vector<float> clean(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        const float env = std::sin(2.0f * kPi * 4.0f * t) * 0.5f + 0.5f;
        clean[i] = 0.30f * env * (
            std::sin(2.0f * kPi *  300.0f * t) +
            std::sin(2.0f * kPi *  700.0f * t) * 0.6f +
            std::sin(2.0f * kPi * 1500.0f * t) * 0.3f
        );
    }
    std::vector<float> noisy = clean;
    std::mt19937 rng(0xC0FFEE);
    // -52 dB additive (signal peak ~0.45, noise sigma 0.001). Matches
    // the "small re-encoding noise" claim in fingerprint.h's doc.
    std::normal_distribution<float> noise(0.0f, 0.001f);
    for (auto& s : noisy) s += noise(rng);

    const auto fp_clean = compute_fingerprint(clean.data(), clean.size());
    const auto fp_noisy = compute_fingerprint(noisy.data(), noisy.size());

    ASSERT_EQ(fp_clean.size(), fp_noisy.size());
    const size_t diff = hamming_distance(fp_clean, fp_noisy);
    // With very small noise on a multi-harmonic signal, base64 strings
    // should differ in well under half their characters — meaningfully
    // better than a random fingerprint. Each base64 char encodes 6
    // bits, so a single byte difference often flips 1-2 chars.
    EXPECT_LT(diff, fp_clean.size() / 2u)
        << "small additive noise should preserve most of the fingerprint";
}

TEST(FingerprintTest, FifteenSecondsProducesExpectedSizeRange) {
    // 15 s / 200 ms = 75 frames → 74 delta bytes → 100 base64 chars.
    const auto audio = make_sine(440.0f, 15.0f);
    const auto fp = compute_fingerprint(audio.data(), audio.size());
    EXPECT_GE(fp.size(), 80u);
    EXPECT_LE(fp.size(), 120u)
        << "spec: ~100 base64 chars for 15 s";
}

TEST(FingerprintTest, SilenceProducesValidButLowEntropyFingerprint) {
    // Pure silence: all magnitudes stay near zero, deltas are noisy
    // floor — fingerprint is well-formed (non-empty) but degenerate.
    const std::vector<float> silence(15 * kSampleRateHz, 0.0f);
    const auto fp = compute_fingerprint(silence.data(), silence.size());
    EXPECT_GE(fp.size(), 80u);
}
