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

#include "internal/config.h"

namespace sentlog::internal {

/// Stable identifier for the envelope wire format. Bumped only when
/// the shape changes incompatibly per spec §6.4. Phase 4a +
/// onwards.
constexpr const char* envelope_format() {
    return "sentlog-evidence/1";
}

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
/// Returns the serialised envelope. No "proof" block — 4b adds it.
std::string build_envelope_unsigned(const Config& config,
                                    const std::string& result_json,
                                    const std::string& valid_from_iso8601_utc);

/// ISO-8601 UTC timestamp helper. Format:
/// "2026-05-25T12:34:56.789Z". Suitable for `validFrom` /
/// `proof.created`.
std::string iso8601_utc_now();

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_ENVELOPE_H
