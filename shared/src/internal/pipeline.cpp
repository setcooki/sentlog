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

#include "internal/fingerprint.h"
#include "internal/ring_buffer.h"
#include "internal/text_match.h"
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

namespace {

float audio_source_from_route(sentlog_audio_route_t route) {
    switch (route) {
        case SENTLOG_AUDIO_ROUTE_BUILT_IN:   return 1.0f;
        case SENTLOG_AUDIO_ROUTE_USB:        return 0.2f;
        case SENTLOG_AUDIO_ROUTE_BLUETOOTH:  return 0.2f;
        case SENTLOG_AUDIO_ROUTE_LOOPBACK:   return 0.0f;
        case SENTLOG_AUDIO_ROUTE_UNKNOWN:    return 0.0f;
    }
    return 0.0f;
}

}  // namespace

namespace {

/// Phase 3c motion-coherence — baseline. Computes the standard
/// deviation of the accelerometer magnitude across pushed frames
/// (gravity-bearing axes; we don't separate gravity from motion at
/// this chunk because the sigma we care about is dominated by
/// non-gravity dynamics regardless). Maps to [0, 1]:
///
///   stddev = 0.00      -> 0.0   (clearly stationary device)
///   stddev = 0.10      -> 1.0   (held device with normal hand motion)
///   stddev > 0.10      -> 1.0   (saturates)
///
/// Why this metric: a truly static recording (phone on a desk)
/// produces near-zero stddev; a held device shows small but
/// consistent hand tremor + speech-coupled motion. A tremor /
/// speech-coupled FFT analysis (the rollout plan's "real" version)
/// drops in here in a follow-up; this baseline is enough to
/// distinguish stationary-from-handheld which is the primary
/// presence.motion signal.
float motion_score_from_frames(const std::vector<PushedMotionFrame>& frames) {
    if (frames.size() < 2) return 0.0f;
    double sum = 0.0, sumsq = 0.0;
    for (const auto& f : frames) {
        const double m = std::sqrt(static_cast<double>(f.ax) * f.ax
                                 + static_cast<double>(f.ay) * f.ay
                                 + static_cast<double>(f.az) * f.az);
        sum   += m;
        sumsq += m * m;
    }
    const double n    = static_cast<double>(frames.size());
    const double mean = sum / n;
    const double var  = std::max(0.0, sumsq / n - mean * mean);
    const double sd   = std::sqrt(var);
    const double mapped = sd / 0.10;
    return static_cast<float>(std::clamp(mapped, 0.0, 1.0));
}

}  // namespace

std::string Pipeline::run(const Config& config,
                          RingBuffer& ring,
                          const std::vector<PushedVadFrame>& pushed_vad,
                          const std::optional<PushedLiveness>& pushed_liveness,
                          const std::optional<PushedTranscript>& pushed_transcript,
                          const std::vector<PushedMotionFrame>& pushed_motion,
                          sentlog_audio_route_t audio_route) {
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
    // Language mismatch is decided inside the transcript block below;
    // we apply it as a lowest-priority failure reason once the block
    // has run. See `language_mismatch` initialisation a few lines
    // down — this `if (failure_reason == nullptr && ...)` check fires
    // at the end of section 4.

    // 4. Transcript — pushed by platform-side whisper.cpp if present
    //    (chunks 2b/2c); otherwise schema-correct placeholder so the
    //    result shape stays stable.
    //
    //    target_match (chunk 2d) is computed here when both a pushed
    //    transcript and config.target_phrase are present. Otherwise
    //    it stays null. See text_match.{h,cpp} for the comparison.
    //
    //    Language gating (chunk 2e): when the pushed transcript's
    //    language doesn't match config.language_hint we surface that
    //    in diagnostics and downgrade confidence below. We compare
    //    case-insensitively but otherwise byte-for-byte — the SDK
    //    doesn't try to normalize ISO 639-1 vs 639-2 (e.g. "en" vs
    //    "eng"); integrators that need that normalisation can do it
    //    on the way in.
    std::optional<float> target_match_score;
    bool language_mismatch = false;
    {
        json t;
        if (pushed_transcript.has_value()) {
            t["text"]       = pushed_transcript->text;
            t["language"]   = pushed_transcript->language;
            t["confidence"] = pushed_transcript->confidence;
            if (!pushed_transcript->word_timings.empty()) {
                json wts = json::array();
                for (const auto& w : pushed_transcript->word_timings) {
                    json wt;
                    wt["word"]     = w.word;
                    wt["start_ms"] = w.start_ms;
                    wt["end_ms"]   = w.end_ms;
                    wts.push_back(wt);
                }
                t["word_timings"] = wts;
            }
            if (config.target_phrase.has_value()) {
                target_match_score = target_match_ratio(
                    pushed_transcript->text, *config.target_phrase);
            }
            if (!config.language_hint.empty()) {
                // case-insensitive ASCII compare
                const auto& a = pushed_transcript->language;
                const auto& b = config.language_hint;
                if (a.size() != b.size()) {
                    language_mismatch = true;
                } else {
                    for (size_t k = 0; k < a.size(); ++k) {
                        const char ca = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(a[k])));
                        const char cb = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(b[k])));
                        if (ca != cb) { language_mismatch = true; break; }
                    }
                }
            }
        } else {
            t["text"]       = "";
            t["language"]   = config.language_hint;
            t["confidence"] = 0.0f;
        }
        if (target_match_score.has_value()) {
            t["target_match"] = *target_match_score;
        } else {
            t["target_match"] = nullptr;
        }
        result["transcript"] = t;
    }

    // Apply language mismatch as the lowest-priority failure reason
    // — TooShort/TooNoisy/NoSpeechDetected describe bigger problems,
    // so don't shadow them.
    if (failure_reason == nullptr && language_mismatch) {
        failure_reason = "LanguageMismatch";
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

    // 6. Presence — audio_source from the platform-pushed audio
    //    route (chunk 1f). Motion from the buffered IMU when
    //    use_motion_sensors=true AND samples were pushed
    //    (chunk 3c). Integrity stays null until Phase 7 (Play
    //    Integrity / DeviceCheck). Summary is the weighted ensemble
    //    of the populated sub-fields per scoring_weights.
    {
        const float audio_source = audio_source_from_route(audio_route);
        json p;
        p["audio_source"] = audio_source;

        std::optional<float> motion_score;
        if (config.use_motion_sensors && !pushed_motion.empty()) {
            motion_score = motion_score_from_frames(pushed_motion);
            p["motion"] = *motion_score;
        }

        // Weighted ensemble of populated sub-fields. integrity is
        // absent (Phase 7+) so it doesn't contribute. If only one
        // sub-field is populated, summary equals it.
        const float w_audio  = weight_or_default(config, "presence.audio_source");
        float num = audio_source * w_audio;
        float den = w_audio;
        if (motion_score.has_value()) {
            const float w_motion = weight_or_default(config, "presence.motion");
            num += *motion_score * w_motion;
            den += w_motion;
        }
        p["summary"] = (den > 0.0f) ? (num / den) : 0.0f;

        result["presence"] = p;
    }

    // direction — same value as transcript.target_match when the
    // comparison ran (chunk 2d). When no target_phrase is configured
    // or no transcript was pushed, direction stays at 0.0 because the
    // SDK has nothing to score against. Per spec §5.5 this is a raw
    // [0,1] score; thresholds_met below applies the config threshold.
    const float direction = target_match_score.value_or(0.0f);
    result["direction"]      = direction;
    // Confidence baseline is the existing speech/no-speech split.
    // Language mismatch (chunk 2e) halves it — non-target-language
    // ASR isn't trustworthy enough to surface as a confident result.
    float base_confidence = vad_report.has_speech() ? 0.6f : 0.0f;
    if (language_mismatch) base_confidence *= 0.5f;
    result["confidence"]     = base_confidence;
    // thresholds_met is the only place threshold values are consumed
    // per the spec. Phase 2 reads direction_threshold; the rest of
    // the per-metric thresholds light up as their scores land
    // (liveness in Phase 3+, engagement when trained). For now: only
    // assertable when target_match was actually computed; otherwise
    // nothing to gate on.
    const bool thresholds_met = target_match_score.has_value()
        && direction >= config.direction_threshold;
    result["thresholds_met"] = thresholds_met;

    // 7. Engagement — placeholder model. model_status = "placeholder"
    //    signals integrators not to surface this until it's "trained".
    {
        json e;
        e["score"]        = 0.65f;
        e["voice_score"]  = 0.65f;
        e["model_status"] = "placeholder";
        result["engagement"] = e;
    }

    // 8. Fingerprint — MIT perceptual hash (Phase 3a). Always
    //    computed on the captured audio. See fingerprint.{h,cpp} for
    //    the algorithm; Chromaprint was dropped for licence reasons
    //    (LGPL would burden the SDK's distribution).
    {
        json f;
        f["algo"]  = fingerprint_algo();
        f["bytes"] = audio.empty()
            ? std::string{}
            : compute_fingerprint(audio.data(), audio.size());
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
        mv["asr"]      = pushed_transcript.has_value()
            ? "whisper-cpp-platform"
            : "placeholder-not-loaded";
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
        if (pushed_transcript.has_value()) {
            d["detected_language"] = pushed_transcript->language;
            d["language_mismatch"] = language_mismatch;
        }
        result["diagnostics"] = d;
    }

    result["evidence_grade"] = "DeviceUnattested";
    result["extensions"]     = json::object();

    return result.dump();
}

}  // namespace sentlog::internal
