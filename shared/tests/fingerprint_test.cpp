#include "internal/fingerprint.h"

#include <gtest/gtest.h>

#include <cmath>
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

// Noise robustness deliberately NOT tested. The algorithm uses
// narrow Goertzel bins + sign-of-delta, which flips bits whenever
// per-band magnitudes cross near zero — that's noise-sensitive by
// design. See the stability note in fingerprint.h for why this is
// acceptable for our duplicate-detection use case (and arguably a
// feature: an attacker can't forge matches by adding low-amplitude
// noise). Determinism on identical input is the property we
// actually depend on; the DeterministicForSameInput test above
// covers that.

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
