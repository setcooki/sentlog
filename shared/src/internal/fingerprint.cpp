#include "internal/fingerprint.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace sentlog::internal {

namespace {

constexpr size_t kSampleRateHz   = 16000;
constexpr size_t kFrameSamples   = 3200;   // 200 ms at 16 kHz
constexpr size_t kBandCount      = 8;

constexpr std::array<float, kBandCount> kBandHz = {
    200.0f, 400.0f, 800.0f, 1200.0f, 1600.0f, 2400.0f, 3200.0f, 5000.0f
};

/// Goertzel filter — exact squared magnitude at `target_hz` for a
/// fixed-length input buffer. Cheaper than an FFT when you only
/// need a handful of bins (we need 8).
float goertzel_magnitude_sq(const float* x, size_t n,
                            float target_hz, float sample_rate_hz) {
    const float k     = static_cast<float>(n) * target_hz / sample_rate_hz;
    const float w     = 2.0f * 3.14159265358979323846f * k / static_cast<float>(n);
    const float cosw  = std::cos(w);
    const float coeff = 2.0f * cosw;

    float s_prev1 = 0.0f;
    float s_prev2 = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const float s = x[i] + coeff * s_prev1 - s_prev2;
        s_prev2 = s_prev1;
        s_prev1 = s;
    }
    // |X(k)|^2 = s_prev1^2 + s_prev2^2 - coeff * s_prev1 * s_prev2
    return s_prev1 * s_prev1
         + s_prev2 * s_prev2
         - coeff * s_prev1 * s_prev2;
}

/// Per-frame band magnitudes (squared, since we only need ordering).
std::array<float, kBandCount> frame_band_magnitudes(const float* frame) {
    std::array<float, kBandCount> mags{};
    for (size_t i = 0; i < kBandCount; ++i) {
        mags[i] = goertzel_magnitude_sq(frame, kFrameSamples,
                                        kBandHz[i],
                                        static_cast<float>(kSampleRateHz));
    }
    return mags;
}

/// Standard base64 (no padding stripping). ~30 lines, no external dep.
std::string base64_encode(const std::vector<unsigned char>& in) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const unsigned a = in[i];
        const unsigned b = in[i + 1];
        const unsigned c = in[i + 2];
        out += kAlphabet[(a >> 2) & 0x3f];
        out += kAlphabet[((a << 4) | (b >> 4)) & 0x3f];
        out += kAlphabet[((b << 2) | (c >> 6)) & 0x3f];
        out += kAlphabet[c & 0x3f];
        i += 3;
    }
    if (i < in.size()) {
        const unsigned a = in[i];
        const unsigned b = (i + 1 < in.size()) ? in[i + 1] : 0;
        out += kAlphabet[(a >> 2) & 0x3f];
        out += kAlphabet[((a << 4) | (b >> 4)) & 0x3f];
        if (i + 1 < in.size()) {
            out += kAlphabet[(b << 2) & 0x3f];
            out += '=';
        } else {
            out += '=';
            out += '=';
        }
    }
    return out;
}

}  // namespace

std::string compute_fingerprint(const float* samples, size_t num_samples) {
    if (samples == nullptr) return "";

    const size_t n_frames = num_samples / kFrameSamples;
    if (n_frames < 2) return "";   // need at least two frames for a delta

    std::array<float, kBandCount> prev = frame_band_magnitudes(samples);
    std::vector<unsigned char> bytes;
    bytes.reserve(n_frames - 1);

    for (size_t t = 1; t < n_frames; ++t) {
        const auto curr = frame_band_magnitudes(samples + t * kFrameSamples);
        unsigned char b = 0;
        for (size_t i = 0; i < kBandCount; ++i) {
            if (curr[i] > prev[i]) {
                b |= static_cast<unsigned char>(1u << i);
            }
        }
        bytes.push_back(b);
        prev = curr;
    }

    return base64_encode(bytes);
}

}  // namespace sentlog::internal
