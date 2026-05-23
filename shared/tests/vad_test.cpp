// Tests for the energy-based VAD reference implementation.
// Generates synthetic audio (silence, white noise, sine-wave bursts
// simulating speech) and validates the reported segments.

#include "internal/vad/vad.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace sentlog::internal;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kSampleRateHz = 16000;

// Append `seconds` of silence to `out`.
void append_silence(std::vector<float>& out, double seconds) {
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    out.insert(out.end(), n, 0.0f);
}

// Append `seconds` of low-amplitude white noise to `out` (simulates
// quiet room).
void append_noise(std::vector<float>& out, double seconds, float amplitude) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    for (size_t i = 0; i < n; ++i) out.push_back(dist(rng));
}

// Append a synthetic "speech" burst — a sum of sines at speech-band
// frequencies modulated by an envelope. Loud enough that the energy
// VAD picks it up reliably.
void append_speech(std::vector<float>& out, double seconds, float amplitude) {
    const size_t n = static_cast<size_t>(seconds * kSampleRateHz);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSampleRateHz);
        const float env = std::sin(2.0f * kPi * 4.0f * t) * 0.5f + 0.5f;
        const float v = amplitude * env * (
            std::sin(2.0f * kPi *  300.0f * t) +
            std::sin(2.0f * kPi *  700.0f * t) * 0.6f +
            std::sin(2.0f * kPi * 1500.0f * t) * 0.3f
        );
        out.push_back(v);
    }
}

}  // namespace

TEST(EnergyVadTest, SilenceProducesNoSegments) {
    std::vector<float> buf;
    append_silence(buf, 2.0);
    auto vad = make_energy_vad();
    const VadReport r = vad->process(buf.data(), buf.size());
    EXPECT_FALSE(r.has_speech());
    EXPECT_EQ(r.total_speech_samples, 0u);
}

TEST(EnergyVadTest, NoiseAloneIsNotSpeech) {
    std::vector<float> buf;
    append_noise(buf, 2.0, 0.005f);  // quiet room
    auto vad = make_energy_vad();
    const VadReport r = vad->process(buf.data(), buf.size());
    EXPECT_FALSE(r.has_speech());
}

TEST(EnergyVadTest, ClearSpeechBurstDetected) {
    std::vector<float> buf;
    append_noise(buf,  0.5, 0.003f);  // 500 ms quiet noise
    append_speech(buf, 1.5, 0.30f);   // 1.5 s "speech"
    append_noise(buf,  0.5, 0.003f);  // 500 ms tail
    auto vad = make_energy_vad();
    const VadReport r = vad->process(buf.data(), buf.size());
    ASSERT_TRUE(r.has_speech());
    EXPECT_GE(r.total_speech_samples, kSampleRateHz);  // at least 1 second
    EXPECT_GT(r.mean_speech_rms, r.mean_noise_rms);
    EXPECT_GT(r.estimated_snr_db(), 6.0f);
}

TEST(EnergyVadTest, SpeechWithBriefPausesIsOneSegment) {
    // Speech, 50ms silence, more speech — should merge into one
    // segment thanks to hangover + gap-merging.
    std::vector<float> buf;
    append_speech(buf, 0.8, 0.30f);
    append_silence(buf, 0.05);
    append_speech(buf, 0.8, 0.30f);
    auto vad = make_energy_vad();
    const VadReport r = vad->process(buf.data(), buf.size());
    ASSERT_TRUE(r.has_speech());
    EXPECT_EQ(r.segments.size(), 1u)
        << "Brief 50ms gap should be absorbed by hangover + merge logic";
}

TEST(EnergyVadTest, TooShortBufferReturnsEmpty) {
    std::vector<float> buf(100, 0.5f);  // < one frame
    auto vad = make_energy_vad();
    const VadReport r = vad->process(buf.data(), buf.size());
    EXPECT_FALSE(r.has_speech());
    EXPECT_EQ(r.segments.size(), 0u);
}

TEST(EnergyVadTest, NullSamplesSafe) {
    auto vad = make_energy_vad();
    const VadReport r = vad->process(nullptr, 16000);
    EXPECT_FALSE(r.has_speech());
}

TEST(EnergyVadReportTest, EstimatedSnrZeroWhenNoSpeech) {
    VadReport r{};
    EXPECT_EQ(r.estimated_snr_db(), 0.0f);
}

TEST(EnergyVadReportTest, EstimatedSnrPositiveWhenSpeechLouderThanNoise) {
    VadReport r{};
    r.mean_speech_rms = 0.3f;
    r.mean_noise_rms  = 0.03f;
    EXPECT_NEAR(r.estimated_snr_db(), 20.0f, 0.5f);
}
