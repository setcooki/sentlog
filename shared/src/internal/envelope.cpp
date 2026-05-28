#include "internal/envelope.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace sentlog::internal {

namespace {

using nlohmann::json;

constexpr const char* kContextVcV2     = "https://www.w3.org/ns/credentials/v2";
constexpr const char* kContextSentlog  = "https://sentlog.org/contexts/evidence/v1";

// Walk a dotted JSON path (e.g. "transcript.text") and return the
// node, or nullptr if any intermediate is missing.
const json* walk_dotted(const json& root, std::string_view path) {
    const json* cur = &root;
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t dot = path.find('.', pos);
        const std::string segment(path.substr(pos, dot - pos));
        if (!cur->is_object() || !cur->contains(segment)) return nullptr;
        cur = &(*cur)[segment];
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return cur;
}

// Set a value at a dotted path, creating intermediate objects as
// needed. Used by the Custom projection to rebuild a subset payload.
void set_dotted(json& root, std::string_view path, const json& value) {
    json* cur = &root;
    size_t pos = 0;
    while (pos < path.size()) {
        const size_t dot = path.find('.', pos);
        const std::string segment(path.substr(pos, dot - pos));
        if (dot == std::string::npos) {
            (*cur)[segment] = value;
            return;
        }
        if (!cur->contains(segment) || !(*cur)[segment].is_object()) {
            (*cur)[segment] = json::object();
        }
        cur = &(*cur)[segment];
        pos = dot + 1;
    }
}

// Helper: copy a top-level field from src to dst if present.
void copy_if_present(const json& src, json& dst, const char* key) {
    if (src.contains(key)) dst[key] = src[key];
}

// Apply a projection to the SentlogResult JSON and return the
// payload subset that goes into credentialSubject.payload. The
// projection name is recorded separately on the envelope itself —
// this function only shapes the payload.
//
// "scores" in spec §4.3 means liveness, presence, direction,
// confidence, thresholds_met, engagement — the numeric verdict
// surface. diagnostics + model_versions + evidence_grade tag along
// in projections that carry scores, since verifiers need them to
// interpret the numbers.
json project_payload(const json& result, const ProjectionSpec& spec,
                     const EncodedAudioClip* audio_clip = nullptr) {
    json out = json::object();

    auto include_scores = [&]() {
        copy_if_present(result, out, "liveness");
        copy_if_present(result, out, "presence");
        copy_if_present(result, out, "direction");
        copy_if_present(result, out, "confidence");
        copy_if_present(result, out, "thresholds_met");
        copy_if_present(result, out, "engagement");
        copy_if_present(result, out, "diagnostics");
        copy_if_present(result, out, "schema_version");
    };
    auto include_transcript = [&](bool with_word_timings) {
        if (!result.contains("transcript")) return;
        if (with_word_timings) {
            out["transcript"] = result["transcript"];
        } else {
            json t = json::object();
            if (result["transcript"].contains("text"))
                t["text"] = result["transcript"]["text"];
            if (result["transcript"].contains("language"))
                t["language"] = result["transcript"]["language"];
            if (result["transcript"].contains("confidence"))
                t["confidence"] = result["transcript"]["confidence"];
            // No word_timings, no target_match in ScoresOnly.
            out["transcript"] = std::move(t);
        }
    };
    auto include_fingerprint = [&]() {
        copy_if_present(result, out, "fingerprint");
    };
    // AudioClip — Phase 4g: when the binding has registered an
    // audio encoder via sentlog_set_audio_encoder, dispatch_envelopes
    // has pre-encoded the ring-buffer audio and hands the bytes in
    // via audio_clip. Otherwise we ship the documented placeholder
    // so the wire shape stays stable.
    auto include_clip = [&]() {
        json clip = json::object();
        if (audio_clip != nullptr && !audio_clip->bytes_b64.empty()) {
            clip["codec"] = audio_clip->codec;
            clip["bytes"] = audio_clip->bytes_b64;
        } else {
            clip["codec"] = "opus";
            clip["bytes"] = "";
            clip["note"]  = "no audio encoder registered; "
                            "set one via sentlog_set_audio_encoder";
        }
        out["audio_clip"] = std::move(clip);
    };

    switch (spec.kind) {
        case Projection::Full:
            // Everything from the result, minus audio_clip (the SDK
            // doesn't compute one for Full — that's Full+AudioClip's
            // job in 4g).
            out = result;
            break;
        case Projection::FullPlusAudioClip:
            out = result;
            include_clip();
            break;
        case Projection::ScoresOnly:
            include_scores();
            include_transcript(/* with_word_timings = */ false);
            break;
        case Projection::TranscriptOnly:
            include_transcript(/* with_word_timings = */ true);
            break;
        case Projection::EvidenceFingerprint:
            include_scores();
            include_fingerprint();
            break;
        case Projection::EvidenceClip:
            include_scores();
            include_clip();
            break;
        case Projection::Custom:
            for (const auto& path : spec.custom_fields) {
                if (const json* node = walk_dotted(result, path)) {
                    set_dotted(out, path, *node);
                }
            }
            break;
    }
    return out;
}

}  // namespace

bool parse_projection_name(const std::string& name, Projection* out) {
    if (name == "Full")                { *out = Projection::Full;                return true; }
    if (name == "Full+AudioClip")      { *out = Projection::FullPlusAudioClip;   return true; }
    if (name == "ScoresOnly")          { *out = Projection::ScoresOnly;          return true; }
    if (name == "TranscriptOnly")      { *out = Projection::TranscriptOnly;      return true; }
    if (name == "EvidenceFingerprint") { *out = Projection::EvidenceFingerprint; return true; }
    if (name == "EvidenceClip")        { *out = Projection::EvidenceClip;        return true; }
    if (name == "Custom")              { *out = Projection::Custom;              return true; }
    return false;
}

const char* projection_name(Projection p) {
    switch (p) {
        case Projection::Full:                return "Full";
        case Projection::FullPlusAudioClip:   return "Full+AudioClip";
        case Projection::ScoresOnly:          return "ScoresOnly";
        case Projection::TranscriptOnly:      return "TranscriptOnly";
        case Projection::EvidenceFingerprint: return "EvidenceFingerprint";
        case Projection::EvidenceClip:        return "EvidenceClip";
        case Projection::Custom:              return "Custom";
    }
    return "Full";
}

std::string iso8601_utc_now() {
    using namespace std::chrono;
    const auto now    = system_clock::now();
    const auto secs   = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - secs).count();

    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif

    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                  tm_utc.tm_year + 1900,
                  tm_utc.tm_mon + 1,
                  tm_utc.tm_mday,
                  tm_utc.tm_hour,
                  tm_utc.tm_min,
                  tm_utc.tm_sec,
                  static_cast<long long>(millis));
    return std::string(buf);
}

std::string build_envelope_unsigned(const Config& config,
                                    const std::string& result_json,
                                    const std::string& valid_from_iso8601_utc,
                                    const ProjectionSpec& spec,
                                    const EncodedAudioClip* audio_clip,
                                    const DeviceAttestation* device_attestation) {
    // The result must be valid JSON — Pipeline::run only produces
    // well-formed JSON, so this is a programming-error path rather
    // than a hot one.
    const auto result_obj = json::parse(result_json);

    json env;
    env["@context"]  = { kContextVcV2, kContextSentlog };
    env["type"]      = { "VerifiableCredential", "SentlogEvidence" };
    env["issuer"]    = config.issuer;
    env["validFrom"] = valid_from_iso8601_utc;

    json subject;
    // session_id, when integrator-supplied, becomes the subject id
    // as a DID; otherwise the id is omitted (VC 2.0 allows the
    // credentialSubject id to be absent).
    if (config.session_id.has_value() && !config.session_id->empty()) {
        subject["id"] = std::string("did:sentlog:session:") + *config.session_id;
    }
    subject["envelope_format"] = envelope_format();

    if (result_obj.contains("model_versions")) {
        subject["model_versions"] = result_obj["model_versions"];
    }
    if (result_obj.contains("evidence_grade")) {
        subject["evidence_grade"] = result_obj["evidence_grade"];
    }
    // Phase 5e: device-attestation token. Attach the {type, token_b64}
    // block and promote evidence_grade to "DeviceAttested" — but only
    // when the current grade is "DeviceUnattested" (never demote from
    // a higher grade like "EnclaveSigned" set by hw signing in 7c-i).
    if (device_attestation != nullptr &&
        !device_attestation->token_b64.empty()) {
        json att;
        att["type"]      = device_attestation->type;
        att["token_b64"] = device_attestation->token_b64;
        subject["device_attestation"] = std::move(att);
        const auto cur = subject.value("evidence_grade", std::string());
        if (cur == "DeviceUnattested") {
            subject["evidence_grade"] = "DeviceAttested";
        }
    }
    if (!config.companion_evidence_refs.empty()) {
        subject["companion_evidence_refs"] = config.companion_evidence_refs;
    }

    subject["projection"] = projection_name(spec.kind);
    subject["payload"]    = project_payload(result_obj, spec, audio_clip);

    env["credentialSubject"] = std::move(subject);

    // No "proof" block here — sign_envelope_eddsa_jcs adds it.
    // No "tsa_timestamp" — 4h adds the RFC 3161 hook.

    return env.dump();
}

}  // namespace sentlog::internal
