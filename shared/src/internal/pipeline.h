// Evaluation pipeline orchestrator.
//
// Pulls captured audio out of the SDK's ring buffer, runs the
// individual stages (VAD now; ASR + liveness + prosody in later
// chunks), and assembles a SentlogResult.
//
// One Pipeline instance per evaluate() call. Stages own no state
// across calls beyond what's loaded into models — which currently
// is nothing.

#ifndef SENTLOG_INTERNAL_PIPELINE_H
#define SENTLOG_INTERNAL_PIPELINE_H

#include <optional>
#include <string>
#include <vector>

#include "internal/config.h"
#include "internal/state.h"

namespace sentlog::internal {

class RingBuffer;

class Pipeline {
public:
    /// Run the pipeline. Returns a JSON-encoded SentlogResult ready
    /// for delivery to the integrator's callback.
    ///
    /// If `pushed_vad` is non-empty, the pipeline uses those
    /// probabilities (from platform-side ONNX inference, e.g. Silero)
    /// instead of the built-in energy VAD.
    ///
    /// If `pushed_liveness` is set, the pipeline populates the
    /// result's `liveness` block from it (from platform-side
    /// AASIST-L inference). Otherwise the block carries a placeholder
    /// with `model_versions.liveness` reflecting that nothing real
    /// ran.
    ///
    /// If `pushed_transcript` is set, the pipeline emits the
    /// transcript block populated from it (from platform-side
    /// whisper.cpp). Otherwise the block is a placeholder with empty
    /// text, language "und", and confidence 0.0. target_match is
    /// always null at this chunk — direction scoring lands in 2d.
    static std::string run(const Config& config,
                           RingBuffer& ring,
                           const std::vector<PushedVadFrame>& pushed_vad,
                           const std::optional<PushedLiveness>& pushed_liveness,
                           const std::optional<PushedTranscript>& pushed_transcript,
                           sentlog_audio_route_t audio_route);
};

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_PIPELINE_H
