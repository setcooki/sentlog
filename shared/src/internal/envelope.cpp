#include "internal/envelope.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace sentlog::internal {

namespace {

using nlohmann::json;

constexpr const char* kContextVcV2     = "https://www.w3.org/ns/credentials/v2";
constexpr const char* kContextSentlog  = "https://sentlog.org/contexts/evidence/v1";

}  // namespace

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
                                    const std::string& valid_from_iso8601_utc) {
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
    if (!config.companion_evidence_refs.empty()) {
        subject["companion_evidence_refs"] = config.companion_evidence_refs;
    }

    // Phase 4a: only the "Full" projection is supported — the entire
    // SentlogResult goes into payload. Other projections drop in
    // here (4c) as the projection enum gains variants.
    subject["projection"] = "Full";
    subject["payload"]    = result_obj;

    env["credentialSubject"] = std::move(subject);

    // No "proof" block in 4a — 4b adds Ed25519 DataIntegrityProof.
    // No "tsa_timestamp" — 4h adds the RFC 3161 hook.

    return env.dump();
}

}  // namespace sentlog::internal
