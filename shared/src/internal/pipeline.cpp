// Phase 1 pipeline. Audio comes from the ring buffer; VAD runs
// (currently energy-based); the result carries real diagnostics
// and placeholder ML scores. ASR, liveness, prosody, fingerprinting,
// and envelope construction land in subsequent chunks; each plugs
// into this orchestrator without changing the public result shape.

#include "internal/pipeline.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "internal/ring_buffer.h"
#include "internal/vad/vad.h"

namespace sentlog::internal {

namespace {

using nlohmann::json;

constexpr size_t kSampleRateHz = 16000;
constexpr float  kPushedVadSpeechThreshold = 0.5f;

const char* audio_quality_label(float snr_db, bool has_speech) {
    if (!has_speech)         return "very_noisy";
    if (snr_db >= 25.0f)     return "quiet";
    if (snr_db >= 15.0f)     return "moderate";
    if (snr_db >= 6.0f)      return "noisy";
    return "very_noisy";
}

// Drain the ring buffer entirely into a contiguous vector. Phase 1
// only — the streaming pipeline in later chunks will operate on the
// ring directly. For now, simplicity wins.
std::vector<float> drain_audio(RingBuffer& ring) {
    std::vector<float> out;
    const size_t avail = ring.available();
    if (avail == 0) return out;
    out.resize(avail);
    const size_t n = ring.read(out.data(), avail);
    out.resize(n);
    return out;
}

// Build a VadReport from platform-pushed probabilities. Threshold +
// merge contiguous speech frames into segments. RMS stats are derived
// from the audio buffer for snr_db calculation.
VadReport vad_report_from_pushed(const std::vector<PushedVadFrame>& pushed,
                                 const float* audio,
                                 size_t total_samples) {
    VadReport r{};
    std::vector<PushedVadFrame> sorted = pushed;
    std::sort(sorted.begin(), sorted.end(),
              [](const PushedVadFrame& a, const PushedVadFrame& b) {
                  return a.start_sample < b.start_sample;
              });

    // Coalesce speech frames into segments. A frame is "speech" if
    // probability >= threshold; contiguous speech frames merge.
    for (const auto& f : sorted) {
        const bool is_speech = f.probability >= kPushedVadSpeechThreshold;
        if (!is_speech) continue;
        const size_t end = f.start_sample + f.num_samples;
        if (!r.segments.empty() &&
            r.segments.back().end_sample >= f.start_sample) {
            r.segments.back().end_sample = std::max(r.segments.back().end_sample, end);
        } else {
            r.segments.push_back({f.start_sample, std::min(end, total_samples)});
        }
    }

    // Compute RMS stats for snr_db.
    if (audio != nullptr && total_samples > 0) {
        std::vector<char> is_speech_sample(total_samples, 0);
        for (const auto& seg : r.segments) {
            const size_t lo = std::min(seg.start_sample, total_samples);
            const size_t hi = std::min(seg.end_sample,   total_samples);
            for (size_t i = lo; i < hi; ++i) is_speech_sample[i] = 1;
        }
        double speech_sumsq = 0.0, noise_sumsq = 0.0;
        size_t speech_n = 0, noise_n = 0;
        for (size_t i = 0; i < total_samples; ++i) {
            const double v = audio[i];
            if (is_speech_sample[i]) { speech_sumsq += v * v; ++speech_n; }
            else                     { noise_sumsq  += v * v; ++noise_n; }
        }
        r.total_speech_samples = speech_n;
        r.mean_speech_rms = speech_n > 0
            ? static_cast<float>(std::sqrt(speech_sumsq / static_cast<double>(speech_n))) : 0.0f;
        r.mean_noise_rms = noise_n > 0
            ? static_cast<float>(std::sqrt(noise_sumsq / static_cast<double>(noise_n))) : 0.0f;
    }
    return r;
}

}  // namespace

std::string Pipeline::run(const Config& config,
                          RingBuffer& ring,
                          const std::vector<PushedVadFrame>& pushed_vad,
                          const std::optional<PushedLiveness>& pushed_liveness) {
    // 1. Drain audio.
    const std::vector<float> audio = drain_audio(ring);
    const int duration_ms = static_cast<int>(
        (static_cast<double>(audio.size()) / static_cast<double>(kSampleRateHz)) * 1000.0);

    // 2. VAD — prefer pushed probabilities (platform-side Silero) over
    //    the built-in energy VAD when present. See ADR 0009.
    const bool used_pushed_vad = !pushed_vad.empty();
    const VadReport vad_report = used_pushed_vad
        ? vad_report_from_pushed(pushed_vad, audio.data(), audio.size())
        : make_energy_vad()->process(audio.data(), audio.size());
    const float snr_db = vad_report.estimated_snr_db();

    // 3. Failure diagnosis — short captures and silent captures fail
    //    fast with a typed reason rather than producing nonsense
    //    downstream scores.
    json result;
    result["schema_version"] = "1";

    const char* failure_reason = nullptr;
    if (duration_ms < config.min_duration_ms) {
        failure_reason = "TooShort";
    } else if (!vad_report.has_speech()) {
        failure_reason = "NoSpeechDetected";
    } else if (snr_db > 0.0f && snr_db < 3.0f) {
        failure_reason = "TooNoisy";
    }

    // 4. Transcript — placeholder until ASR (chunk 4+) lands. Empty
    //    text, neutral confidence; word_timings omitted.
    {
        json t;
        t["text"]       = "";
        t["language"]   = config.language_hint;
        t["confidence"] = 0.0f;
        result["transcript"] = t;
    }

    // 5. Liveness — pushed AASIST-L result if present; otherwise
    //    a placeholder (0.5 / 0 / 0). model_versions.liveness below
    //    reflects which path ran so integrators don't surface the
    //    placeholder as if it were real.
    {
        json l;
        if (pushed_liveness.has_value()) {
            l["score"]          = pushed_liveness->score;
            l["replay_risk"]    = pushed_liveness->replay_risk;
            l["synthetic_risk"] = pushed_liveness->synthetic_risk;
        } else {
            l["score"]          = 0.5f;
            l["replay_risk"]    = 0.0f;
            l["synthetic_risk"] = 0.0f;
        }
        result["liveness"] = l;
    }

    // 6. Presence — audio_source is always 1.0 from the C++ core's
    //    perspective (the platform binding is the only thing that
    //    actually knows the input route; chunk 5 wires it through).
    //    Motion + integrity stay null in this chunk.
    {
        json p;
        p["summary"]      = 1.0f;
        p["audio_source"] = 1.0f;
        result["presence"] = p;
    }

    result["direction"]      = 0.0f;
    result["confidence"]     = vad_report.has_speech() ? 0.6f : 0.0f;
    result["thresholds_met"] = false;  // pure scores aren't trained yet — see model_status

    // 7. Engagement — placeholder model. model_status = "placeholder"
    //    signals integrators not to surface this until it's "trained".
    {
        json e;
        e["score"]        = 0.65f;
        e["voice_score"]  = 0.65f;
        e["model_status"] = "placeholder";
        result["engagement"] = e;
    }

    // 8. Fingerprint — empty in this chunk. Chromaprint integration
    //    is a small follow-up; the bytes field stays present so the
    //    schema is stable.
    {
        json f;
        f["algo"]  = "chromaprint-v2";
        f["bytes"] = "";
        result["fingerprint"] = f;
    }

    // 9. Model versions — versioned strings for what's actually
    //    loaded. Phase 1 carries the SDK version on each line so
    //    integrators can correlate evidence across upgrades.
    {
        json mv;
        mv["liveness"] = pushed_liveness.has_value()
            ? "aasist-l-platform-onnx"
            : "placeholder-not-loaded";
        mv["asr"]      = "0.0.0-phase1-not-loaded";
        mv["prosody"]  = "0.0.0-phase1-not-loaded";
        mv["vad"]      = used_pushed_vad
            ? "silero-vad-v5-platform-onnx"
            : "energy-vad-baseline";
        mv["manifest"] = "0.0.0-phase1";
        result["model_versions"] = mv;
    }

    // 10. Diagnostics — real this time.
    {
        json d;
        if (failure_reason != nullptr) d["failure_reason"] = failure_reason;
        if (snr_db > 0.0f)             d["snr_db"]         = snr_db;
        d["duration_ms"]    = duration_ms;
        d["audio_quality"]  = audio_quality_label(snr_db, vad_report.has_speech());
        result["diagnostics"] = d;
    }

    result["evidence_grade"] = "DeviceUnattested";
    result["extensions"]     = json::object();

    return result.dump();
}

}  // namespace sentlog::internal
