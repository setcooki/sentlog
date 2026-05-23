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

#include <string>

#include "internal/config.h"

namespace sentlog::internal {

class RingBuffer;

class Pipeline {
public:
    /// Run the pipeline. Returns a JSON-encoded SentlogResult ready
    /// for delivery to the integrator's callback.
    static std::string run(const Config& config, RingBuffer& ring);
};

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_PIPELINE_H
