// W3C Verifiable Credentials 2.0 evidence envelope — Phase 4a.
//
// Takes a Pipeline::run-produced SentlogResult JSON and wraps it
// into the envelope shape declared in
// docs/technical-specification.md §4.4. This chunk emits the
// envelope *without* a proof block — signing lands in 4b. Forwarder
// dispatch + projection variants land in 4c/4d.
//
// The envelope is the durable evidence artefact. Tools downstream
// (the reference verifier in tools/evidence-verifier/, third-party
// VC-aware wallets, on-chain attestation oracles) read envelopes
// without the SDK source. Stability of the envelope shape across
// SDK versions is governed by `envelope_format` (decision row 1.24
// in the spec); bumping the format requires bumping that string.
//
// What 4a hands the integrator:
//   - "@context" + "type" per VC 2.0
//   - "issuer" pulled from SentlogConfig.issuer (required)
//   - "validFrom" as an ISO 8601 UTC timestamp at envelope-build time
//   - "credentialSubject" carrying the projection + the result payload
//   - NO "proof" block (4b adds Ed25519 software signing)
//   - NO "tsa_timestamp" (4h adds the RFC 3161 hook)
//
// Phase 4a only supports the "Full" projection — the whole
// SentlogResult JSON goes into credentialSubject.payload as-is.
// Other projections (ScoresOnly, TranscriptOnly, etc. per spec
// §4.2) come in 4c, which introduces a Projection enum.

#ifndef SENTLOG_INTERNAL_ENVELOPE_H
#define SENTLOG_INTERNAL_ENVELOPE_H

#include <string>
#include <vector>

#include "internal/config.h"

namespace sentlog::internal {

/// Stable identifier for the envelope wire format. Bumped only when
/// the shape changes incompatibly per spec §6.4. Phase 4a +
/// onwards.
constexpr const char* envelope_format() {
    return "sentlog-evidence/1";
}

/// Spec §4.3 projection variants. Each projection narrows the
/// SentlogResult payload to a different subset before the envelope
/// is built — the projection name surfaces in
/// `credentialSubject.projection` so verifiers know what they're
/// looking at.
enum class Projection {
    Full,                // everything except raw audio clip
    FullPlusAudioClip,   // Full + Opus clip (clip wiring lands in 4g)
    ScoresOnly,          // scores + transcript.text; no fingerprint/clip
    TranscriptOnly,      // transcript + word_timings only
    EvidenceFingerprint, // scores + fingerprint; no transcript
    EvidenceClip,        // scores + Opus clip; no transcript (clip in 4g)
    Custom,              // payload is constructed from custom_fields
};

/// Per-forwarder projection setting. `custom_fields` is only read
/// when `kind == Custom`; each entry is a dotted JSON path
/// (e.g. "transcript.text", "liveness.score", "presence.audio_source").
/// Paths that don't exist in the result are silently dropped — the
/// integrator is responsible for asking for fields the pipeline
/// actually emits.
struct ProjectionSpec {
    Projection               kind{Projection::Full};
    std::vector<std::string> custom_fields;
};

/// Parse a projection name string into the enum. Returns false if
/// `name` isn't one of the published projection names; the config
/// parser uses this to validate forwarder projection at init.
bool parse_projection_name(const std::string& name, Projection* out);

/// Inverse — projection enum to its canonical wire name (matches
/// the spec §4.3 list verbatim). Used when writing the
/// `credentialSubject.projection` field.
const char* projection_name(Projection p);

/// Build the unsigned VC-2.0 envelope around the result JSON.
/// Caller passes the JSON Pipeline::run returned. The function
/// throws nlohmann::json::exception on malformed result JSON; the
/// pipeline only produces well-formed JSON so callers don't need
/// to handle that.
///
/// `valid_from_iso8601_utc` lets the caller inject a deterministic
/// timestamp in tests. In production, the binding passes the
/// current UTC instant.
///
/// `spec` controls what subset of the result lands in
/// `credentialSubject.payload`. For backwards compatibility, the
/// default is the `Full` projection.
///
/// `audio_clip` carries the pre-encoded audio bytes (Phase 4g) to
/// embed in AudioClip-bearing projections. When absent, the
/// envelope ships the documented empty-placeholder block so the
/// wire shape stays stable even without a registered encoder.
struct EncodedAudioClip {
    std::string codec;       // e.g. "opus"
    std::string bytes_b64;   // standard base64, with padding
};

/// Returns the serialised envelope. No "proof" block — 4b adds it.
std::string build_envelope_unsigned(
    const Config& config,
    const std::string& result_json,
    const std::string& valid_from_iso8601_utc,
    const ProjectionSpec& spec = ProjectionSpec{},
    const EncodedAudioClip* audio_clip = nullptr);

/// ISO-8601 UTC timestamp helper. Format:
/// "2026-05-25T12:34:56.789Z". Suitable for `validFrom` /
/// `proof.created`.
std::string iso8601_utc_now();

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_ENVELOPE_H
