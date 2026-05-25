// Prosody feature extraction — Phase 3d.
//
// Deterministic, MIT-licensed extractor that emits a handful of
// eGeMAPS-style features over a captured utterance. Replaces the
// openSMILE eGeMAPS dependency the original rollout-plan called for
// — openSMILE is dual-licensed GPL / commercial, both incompatible
// with the SDK's MIT positioning. Same fork-in-the-road we took
// for Chromaprint (see fingerprint.h).
//
// The eGeMAPS feature *set* is a published audio-feature spec
// (Eyben et al. 2016). The openSMILE *implementation* is what's
// GPL. This file implements a small subset of the spec, named
// `sentlog-prosody-v1`, sufficient to surface a meaningful
// (but untrained) engagement.voice_score.
//
// Algorithm:
//
//   1. Split audio into 30 ms frames with 10 ms hop (overlapping).
//
//   2. Per frame compute:
//        - RMS energy
//        - Zero-crossing rate (ZCR)
//        - F0 via autocorrelation peak picking in the human-voice
//          range 50-400 Hz. Mark the frame voiced if the
//          normalized autocorrelation peak clears 0.5 AND the
//          RMS clears a small noise floor.
//
//   3. Aggregate over the voiced frames:
//        - voiced_frame_ratio = voiced / total
//        - mean_f0_hz, std_f0_hz
//        - mean_rms, std_rms
//
//   4. voice_score = clamp(0.5 * (std_f0_hz / 30.0)
//                        + 0.5 * (std_rms / mean_rms), 0, 1)
//      Heuristic: animated/engaged speakers show higher pitch
//      variation (std_f0_hz) and energy contour (std_rms /
//      mean_rms). Monotone delivery hugs zero on both.
//
// Limitations (intentional — bumped in a future chunk):
//   - F0 via plain autocorrelation, no octave-error guard, no
//     smoothing across frames. Real speech with strong second
//     harmonics can flip between F0 and 2·F0 frame-to-frame,
//     inflating std_f0_hz and pushing voice_score upward even for
//     monotone speakers. Good enough to compute variability,
//     not good enough as a clinical pitch tracker.
//   - No jitter / shimmer / HNR (the harder eGeMAPS features).
//     Adding them requires per-cycle pitch period extraction.
//   - voice_score saturates cheaply. A 30 Hz F0 spread + a
//     coefficient-of-variation of ~0.6 in RMS already reaches 1.0
//     — both common in conversational English. The heuristic is
//     not calibrated against any corpus; integrators MUST gate on
//     engagement_model_status == "trained" before surfacing the
//     score to end users. Until the trained head lands in Phase
//     6+, the pipeline emits model_status = "training".

#ifndef SENTLOG_INTERNAL_PROSODY_H
#define SENTLOG_INTERNAL_PROSODY_H

#include <cstddef>

namespace sentlog::internal {

struct ProsodyFeatures {
    bool   has_signal{false};       // true iff any frame met the voicing test
    float  voiced_frame_ratio{0.0f};
    float  mean_f0_hz{0.0f};
    float  std_f0_hz{0.0f};
    float  mean_rms{0.0f};
    float  std_rms{0.0f};
    /// Heuristic in [0, 1]. NOT a trained engagement model — the
    /// pipeline surfaces this as engagement.voice_score with
    /// engagement_model_status = "training".
    float  voice_score{0.0f};
};

/// Stable identifier for the prosody algorithm. Emitted as
/// `model_versions.prosody` when this implementation populates the
/// engagement block.
constexpr const char* prosody_algo() {
    return "sentlog-prosody-v1";
}

/// Compute features over `samples` (16 kHz mono Float32, [-1, 1]).
/// Returns features with `has_signal = false` if no frames met the
/// voicing test (silence / too-short audio / no detectable pitch).
ProsodyFeatures extract_prosody(const float* samples, size_t num_samples);

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_PROSODY_H
