// Energy-based VAD — Phase 1 reference implementation.
//
// Algorithm:
//   1. Slice the input into 20 ms frames (320 samples @ 16 kHz).
//   2. Compute RMS for each frame.
//   3. Estimate noise floor as the median RMS of the lowest 20% of
//      frames (robust to brief silences and to short utterances).
//   4. Classify a frame as speech if its RMS exceeds the noise floor
//      by at least kSpeechMarginDb decibels AND exceeds an absolute
//      minimum (so 16-bit dithering doesn't trigger speech).
//   5. Apply hangover: extend each detected segment by kHangoverFrames
//      frames so trailing consonants and brief pauses inside a word
//      don't fragment the segment.
//   6. Merge adjacent segments separated by less than kMergeGapFrames.
//
// This is a baseline — Silero (ONNX) replaces it in a follow-up chunk
// when ORT integration lands. Both share the Vad interface.

#include "internal/vad/vad.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sentlog::internal {

namespace {

constexpr size_t kFrameSizeSamples   = 320;     // 20 ms @ 16 kHz
constexpr size_t kHangoverFrames     = 10;      // 200 ms after speech end
constexpr size_t kMergeGapFrames     = 5;       // merge gaps under 100 ms
constexpr float  kSpeechMarginDb     = 6.0f;    // 6 dB above noise floor
constexpr float  kAbsoluteFloorRms   = 0.001f;  // ~ -60 dBFS

float compute_rms(const float* samples, size_t n) {
    if (n == 0) return 0.0f;
    double sumsq = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s = samples[i];
        sumsq += s * s;
    }
    return static_cast<float>(std::sqrt(sumsq / static_cast<double>(n)));
}

float rms_to_db(float rms) {
    if (rms <= 0.0f) return -120.0f;
    return 20.0f * std::log10(rms);
}

class EnergyVad : public Vad {
public:
    VadReport process(const float* samples, size_t num_samples) override {
        VadReport report{};
        if (samples == nullptr || num_samples < kFrameSizeSamples) {
            return report;
        }

        // 1. Per-frame RMS.
        const size_t num_frames = num_samples / kFrameSizeSamples;
        std::vector<float> frame_rms(num_frames, 0.0f);
        for (size_t i = 0; i < num_frames; ++i) {
            frame_rms[i] = compute_rms(samples + i * kFrameSizeSamples,
                                       kFrameSizeSamples);
        }

        // 2. Noise floor — median of the quietest 20% of frames.
        const float noise_floor_rms = estimate_noise_floor(frame_rms);
        const float speech_threshold_rms = std::max(
            kAbsoluteFloorRms,
            noise_floor_rms * std::pow(10.0f, kSpeechMarginDb / 20.0f)
        );

        // 3. Frame-level speech decisions.
        std::vector<char> is_speech(num_frames, 0);
        for (size_t i = 0; i < num_frames; ++i) {
            if (frame_rms[i] >= speech_threshold_rms) {
                is_speech[i] = 1;
            }
        }

        // 4. Hangover — extend each speech run forward.
        for (size_t i = 0; i < num_frames; ) {
            if (!is_speech[i]) { ++i; continue; }
            size_t end = i + 1;
            while (end < num_frames && is_speech[end]) ++end;
            const size_t hangover_end = std::min(end + kHangoverFrames, num_frames);
            for (size_t k = end; k < hangover_end; ++k) is_speech[k] = 1;
            i = hangover_end;
        }

        // 5. Coalesce contiguous speech frames into segments, merging
        //    short gaps.
        std::vector<SpeechSegment> raw_segments;
        for (size_t i = 0; i < num_frames; ) {
            if (!is_speech[i]) { ++i; continue; }
            size_t end = i + 1;
            while (end < num_frames && is_speech[end]) ++end;
            raw_segments.push_back({i * kFrameSizeSamples,
                                    end * kFrameSizeSamples});
            i = end;
        }
        report.segments = merge_close_segments(raw_segments);

        // 6. Stats — energy in speech vs non-speech.
        double speech_sumsq = 0.0;
        double noise_sumsq  = 0.0;
        size_t speech_samples = 0;
        size_t noise_samples  = 0;
        std::vector<char> sample_is_speech(num_samples, 0);
        for (const auto& seg : report.segments) {
            for (size_t k = seg.start_sample; k < seg.end_sample; ++k) {
                sample_is_speech[k] = 1;
            }
        }
        for (size_t i = 0; i < num_samples; ++i) {
            const double v = samples[i];
            if (sample_is_speech[i]) {
                speech_sumsq += v * v;
                ++speech_samples;
            } else {
                noise_sumsq += v * v;
                ++noise_samples;
            }
        }
        report.total_speech_samples = speech_samples;
        report.mean_speech_rms = speech_samples > 0
            ? static_cast<float>(std::sqrt(speech_sumsq / speech_samples))
            : 0.0f;
        report.mean_noise_rms = noise_samples > 0
            ? static_cast<float>(std::sqrt(noise_sumsq / noise_samples))
            : 0.0f;
        return report;
    }

private:
    static float estimate_noise_floor(const std::vector<float>& frame_rms) {
        if (frame_rms.empty()) return 0.0f;
        std::vector<float> sorted = frame_rms;
        std::sort(sorted.begin(), sorted.end());
        const size_t cutoff = std::max<size_t>(1, sorted.size() / 5);
        const size_t mid = cutoff / 2;
        return sorted[mid];
    }

    static std::vector<SpeechSegment> merge_close_segments(
            const std::vector<SpeechSegment>& in) {
        if (in.empty()) return in;
        std::vector<SpeechSegment> out;
        out.reserve(in.size());
        out.push_back(in.front());
        constexpr size_t gap_samples = kMergeGapFrames * kFrameSizeSamples;
        for (size_t i = 1; i < in.size(); ++i) {
            if (in[i].start_sample - out.back().end_sample <= gap_samples) {
                out.back().end_sample = in[i].end_sample;
            } else {
                out.push_back(in[i]);
            }
        }
        return out;
    }
};

}  // namespace

float VadReport::estimated_snr_db() const {
    if (mean_speech_rms <= 0.0f || mean_noise_rms <= 0.0f) {
        return mean_speech_rms > 0.0f ? 60.0f : 0.0f;
    }
    return rms_to_db(mean_speech_rms) - rms_to_db(mean_noise_rms);
}

std::unique_ptr<Vad> make_energy_vad() {
    return std::make_unique<EnergyVad>();
}

}  // namespace sentlog::internal
