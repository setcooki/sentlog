// Voice activity detection interface.
//
// The SDK runs VAD over captured audio to (a) tell the rest of the
// pipeline where speech actually is — ASR and liveness only run on
// speech segments, not silence — and (b) populate the audio_quality
// and snr_db diagnostics that integrators consult when a result fails.
//
// One Vad instance per evaluation. The interface is intentionally
// stateful (so implementations can apply hangover/smoothing) but
// resettable between evaluations.
//
// Phase 1 implementation: EnergyVad — RMS-based detection with
// hangover smoothing. No model file, no external dependencies.
// Reasonable accuracy in clean conditions; Phase 1.5 swaps in
// Silero (ONNX-Runtime-based) for cross-environment robustness.
// Both implementations satisfy this interface.

#ifndef SENTLOG_INTERNAL_VAD_VAD_H
#define SENTLOG_INTERNAL_VAD_VAD_H

#include <cstddef>
#include <memory>
#include <vector>

namespace sentlog::internal {

/// A contiguous range of samples classified as speech.
struct SpeechSegment {
    size_t start_sample;   // inclusive
    size_t end_sample;     // exclusive
};

/// Output of running a VAD over a full audio buffer.
struct VadReport {
    std::vector<SpeechSegment> segments;
    size_t total_speech_samples;  // sum of segment lengths
    float  mean_speech_rms;       // average RMS across speech segments, 0 if none
    float  mean_noise_rms;        // average RMS across non-speech segments, 0 if none

    // Convenience derivations used by Diagnostics.
    float estimated_snr_db() const;
    bool  has_speech() const { return !segments.empty(); }
};

class Vad {
public:
    virtual ~Vad() = default;

    /// Process an entire buffer of mono 16 kHz float samples and
    /// return the resulting report. NOT safe to call concurrently
    /// with itself on the same instance; resets internal state.
    virtual VadReport process(const float* samples, size_t num_samples) = 0;
};

/// Construct the Phase 1 reference VAD (energy-based).
std::unique_ptr<Vad> make_energy_vad();

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_VAD_VAD_H
