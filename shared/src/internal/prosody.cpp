#include "internal/prosody.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sentlog::internal {

namespace {

constexpr size_t kSampleRateHz   = 16000;
constexpr size_t kFrameSamples   = 480;   // 30 ms at 16 kHz
constexpr size_t kHopSamples     = 160;   // 10 ms hop
constexpr float  kRmsNoiseFloor  = 0.005f;

constexpr float kMinF0Hz = 50.0f;
constexpr float kMaxF0Hz = 400.0f;
constexpr float kPeakNormThreshold = 0.5f;

float frame_rms(const float* x, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(s / static_cast<double>(n)));
}

/// Estimate F0 via autocorrelation peak picking in the human-voice
/// range. Returns 0 if no peak clears the normalised threshold.
///
/// We compute the autocorrelation only at the lag values that
/// correspond to frequencies in [50, 400] Hz — much cheaper than a
/// full ACF, and the cap on the lag range avoids the trivial peak
/// at lag 0.
float frame_f0(const float* x, size_t n, float sample_rate_hz) {
    const size_t min_lag = static_cast<size_t>(sample_rate_hz / kMaxF0Hz);
    const size_t max_lag = static_cast<size_t>(sample_rate_hz / kMinF0Hz);
    if (max_lag >= n) return 0.0f;

    // r0 is the autocorrelation at lag 0 — used to normalise.
    double r0 = 0.0;
    for (size_t i = 0; i < n; ++i) r0 += static_cast<double>(x[i]) * x[i];
    if (r0 <= 0.0) return 0.0f;

    double best_norm = 0.0;
    size_t best_lag  = 0;
    for (size_t lag = min_lag; lag <= max_lag; ++lag) {
        double r = 0.0;
        for (size_t i = 0; i + lag < n; ++i) {
            r += static_cast<double>(x[i]) * x[i + lag];
        }
        const double norm = r / r0;
        if (norm > best_norm) {
            best_norm = norm;
            best_lag  = lag;
        }
    }
    if (best_norm < static_cast<double>(kPeakNormThreshold)) return 0.0f;
    if (best_lag == 0) return 0.0f;
    return sample_rate_hz / static_cast<float>(best_lag);
}

}  // namespace

ProsodyFeatures extract_prosody(const float* samples, size_t num_samples) {
    ProsodyFeatures out;
    if (samples == nullptr || num_samples < kFrameSamples) return out;

    std::vector<float> rms_voiced;
    std::vector<float> f0_voiced;
    size_t total_frames = 0;

    for (size_t start = 0; start + kFrameSamples <= num_samples;
         start += kHopSamples) {
        ++total_frames;
        const float rms = frame_rms(samples + start, kFrameSamples);
        if (rms < kRmsNoiseFloor) continue;
        const float f0 = frame_f0(samples + start, kFrameSamples,
                                  static_cast<float>(kSampleRateHz));
        if (f0 <= 0.0f) continue;
        rms_voiced.push_back(rms);
        f0_voiced.push_back(f0);
    }

    if (rms_voiced.empty() || total_frames == 0) return out;

    out.has_signal         = true;
    out.voiced_frame_ratio = static_cast<float>(rms_voiced.size())
                            / static_cast<float>(total_frames);

    auto mean_std = [](const std::vector<float>& v) {
        double sum = 0.0, sumsq = 0.0;
        for (float x : v) { sum += x; sumsq += static_cast<double>(x) * x; }
        const double n = static_cast<double>(v.size());
        const double mean = sum / n;
        const double var  = std::max(0.0, sumsq / n - mean * mean);
        return std::pair<float, float>{
            static_cast<float>(mean),
            static_cast<float>(std::sqrt(var))
        };
    };

    std::tie(out.mean_f0_hz, out.std_f0_hz)   = mean_std(f0_voiced);
    std::tie(out.mean_rms,   out.std_rms)     = mean_std(rms_voiced);

    // Heuristic voice score — see prosody.h for the rationale.
    // Engaged speakers show more pitch and energy variability than
    // monotone ones. Divisors normalise: 30 Hz spread is a lot of
    // pitch variation; rms_std / rms_mean is the coefficient of
    // variation (already unitless).
    const float pitch_term  = out.std_f0_hz / 30.0f;
    const float energy_term = (out.mean_rms > 0.0f)
        ? (out.std_rms / out.mean_rms) : 0.0f;
    const float score = 0.5f * pitch_term + 0.5f * energy_term;
    out.voice_score = std::clamp(score, 0.0f, 1.0f);

    return out;
}

}  // namespace sentlog::internal
