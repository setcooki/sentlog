// OTA manifest parse + Ed25519 verification — Phase 7a.
//
// The SDK's model bundle is updated out-of-band via a signed JSON
// manifest. Two rotatable pinned Ed25519 public keys (active +
// standby) guard the supply chain; rotation is by overlap, not by
// flag day. Spec decisions 1.3 + 1.20 + §5.1.
//
// 7a scope: pure verification primitive — given the bytes of a
// fetched manifest envelope and the two pinned pubkeys, parse and
// verify. No fetch, no apply, no C ABI surface change. The platform
// HTTPS fetch + C ABI plumbing lands in 7b.
//
// --- Wire format (signed manifest envelope) ----------------------
//
//   {
//     "manifest": {
//       "schema_version":   "1",
//       "manifest_version": "0.0.5-phase7",
//       "bundle_url":       "https://.../models-0.0.5.tar",
//       "bundle_sha256":    "<lowercase hex, 64 chars>",
//       "bundle_size":      123456,
//       "issued_at_ms":     1735689600000
//     },
//     "key_id":            "active",      // "active" | "standby"
//     "signature_b64url":  "<base64url, no padding>"
//   }
//
// The signature covers the JCS-canonicalised (RFC 8785) bytes of
// the `manifest` sub-object. The same JCS function used for envelope
// signing (signing.cpp) is reused; the canonicalisation rule must
// match between manifest-publisher and verifier or signatures won't
// validate. `key_id` selects which of the two pinned pubkeys the
// verifier uses; unknown values are rejected outright.
//
// `bundle_sha256` and `bundle_size` are the integrity check for the
// actual bundle download (a separate concern handled in 7b — the
// 7a primitive only verifies the manifest itself).

#ifndef SENTLOG_INTERNAL_OTA_H
#define SENTLOG_INTERNAL_OTA_H

#include "internal/signing.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace sentlog::internal {

/// Parsed manifest payload (the inner `manifest` object).
struct Manifest {
    std::string schema_version;
    std::string manifest_version;
    std::string bundle_url;
    std::string bundle_sha256;   // lowercase hex, 64 chars
    int64_t     bundle_size = 0;
    int64_t     issued_at_ms = 0;
};

enum class ManifestVerifyStatus {
    OK = 0,
    InvalidJson,
    MissingFields,
    UnknownKeyId,
    BadSignature,
    UnsupportedSchemaVersion,
};

/// Manifest schema versions this build understands.
constexpr const char* kManifestSchemaSupported = "1";

/// Parse the signed manifest envelope, verify the Ed25519 signature
/// against whichever of the two pinned pubkeys `key_id` selects, and
/// populate `out` on success. On any error `out` is left untouched.
///
/// Verification is constant-time per tweetnacl's `crypto_sign_open`.
/// Returns OK only when JSON parses, all required fields are present,
/// the schema version is supported, the key_id selects a known pubkey,
/// and the signature validates against the JCS-canonicalised manifest
/// bytes.
ManifestVerifyStatus parse_and_verify_manifest(
    std::string_view envelope_json,
    const Ed25519PublicKey& active_pubkey,
    const Ed25519PublicKey& standby_pubkey,
    Manifest& out);

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_OTA_H
