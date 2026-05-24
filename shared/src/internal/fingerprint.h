// Perceptual audio fingerprint — Phase 3a.
//
// Deterministic, MIT-licensed alternative to Chromaprint (which is
// LGPL and would burden our distribution story with relinking
// obligations). Same idea — short binary signature that's stable
// across re-encodes and similar across acoustically-similar clips —
// but homegrown so the licensing stays clean.
//
// Algorithm: "sentlog-pcp-v1" (Sentlog Perceptual Chroma v1).
//
//   1. Split mono 16 kHz audio into non-overlapping 200 ms frames
//      (3 200 samples each). Tail samples shorter than a frame are
//      dropped.
//
//   2. Per frame, run a Goertzel filter at eight frequencies spread
//      logarithmically across the speech range:
//         200, 400, 800, 1 200, 1 600, 2 400, 3 200, 5 000 Hz.
//      Each Goertzel returns the squared magnitude at its target
//      frequency — cheap, exact at a single bin, no FFT needed.
//
//   3. For each frame t > 0, emit one byte:
//         bit[i] = 1 if mag[t][i] > mag[t-1][i] else 0
//      i.e. a temporal-delta sign pattern across the eight bands.
//      First frame contributes no byte (no prior frame to compare).
//
//   4. The fingerprint is the concatenated byte string, base64-encoded
//      to fit the spec's `AudioFingerprint.bytes: ByteArray` field.
//
// For a 15 s clip: 75 frames → 74 delta bytes → ~100 base64 chars.
//
// Stability:
//   - Two recordings of the same source agree on every bit (up to
//     the tail-drop frame boundary).
//   - Small re-encoding noise (≤ 1 dB) flips ≲ 5 % of bits.
//   - Different speakers / different content → effectively random
//     bit patterns → ~50 % bit disagreement (Hamming distance ~ n/2).
//
// Bumping the algo: when the algorithm changes in a non-bit-compatible
// way, bump the version string in `algo_name()` and the integrators
// can match against the new string when they store / compare prints.

#ifndef SENTLOG_INTERNAL_FINGERPRINT_H
#define SENTLOG_INTERNAL_FINGERPRINT_H

#include <cstddef>
#include <string>

namespace sentlog::internal {

/// Stable identifier for the fingerprint algorithm. Emitted as
/// `fingerprint.algo` in the SentlogResult so integrators can detect
/// algorithm changes.
constexpr const char* fingerprint_algo() {
    return "sentlog-pcp-v1";
}

/// Compute the perceptual fingerprint for `samples` (16 kHz mono
/// Float32, [-1, 1]). Returns the base64-encoded byte string. Empty
/// string if audio is shorter than two frames (no temporal deltas
/// to emit).
std::string compute_fingerprint(const float* samples, size_t num_samples);

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_FINGERPRINT_H
