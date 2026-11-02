// Ed25519 signing + W3C DataIntegrityProof — Phase 4b.
//
// Takes the unsigned envelope from envelope.{h,cpp} and adds a
// `proof` block per W3C VC Data Integrity 1.0 using the
// `eddsa-jcs-2022` cryptosuite. Spec decision row 1.25 called for
// `eddsa-rdfc-2022`; we ship `eddsa-jcs-2022` instead because:
//
//   - `eddsa-rdfc-2022` requires URDNA2015 (RDF Dataset
//     Canonicalisation) — that's a ~5000-line algorithm and
//     a substantial dependency to vendor.
//   - `eddsa-jcs-2022` uses RFC 8785 JSON Canonicalisation Scheme,
//     which is ~150 lines of code with no external dependency.
//   - Both are W3C-registered Data Integrity cryptosuites; both
//     produce verifiable proofs that VC-aware verifiers accept.
//
// The spec is updated in the same chunk to record the choice.
//
// Signing key plumbing:
//
//   - The integrator hands a 64-byte Ed25519 secret key to the
//     SDK via `sentlog_set_signing_key` (in c_api.h). The first
//     32 bytes are the seed; the last 32 are the derived public
//     key, matching the RFC 8032 wire format that tweetnacl
//     consumes. The same call sets the `verificationMethod`
//     string that the proof block carries — typically a `did:key:`
//     URI the integrator's verifier can resolve to the public key.
//
//   - Hardware-backed signing (Secure Enclave / KeyStore) lands in
//     Phase 7. The proof cryptosuite then changes to
//     `ecdsa-jcs-2019`; this header gains a second sign function.

#ifndef SENTLOG_INTERNAL_SIGNING_H
#define SENTLOG_INTERNAL_SIGNING_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sentlog::internal {

constexpr size_t kEd25519SecretKeyBytes = 64;   // RFC 8032 seed||pubkey
constexpr size_t kEd25519PublicKeyBytes = 32;
constexpr size_t kEd25519SignatureBytes = 64;

using Ed25519SecretKey = std::array<uint8_t, kEd25519SecretKeyBytes>;
using Ed25519PublicKey = std::array<uint8_t, kEd25519PublicKeyBytes>;

/// JCS — RFC 8785 JSON Canonicalisation Scheme. Deterministic
/// serialisation: object keys sorted, no insignificant whitespace,
/// specific number formatting. Takes a serialised JSON string and
/// returns its canonical form.
std::string jcs_canonicalize(const std::string& json_text);

/// Ed25519 sign per RFC 8032. Returns the 64-byte signature.
/// Wraps tweetnacl's `crypto_sign` (which prepends the signature
/// to the message); we extract just the signature bytes.
std::array<uint8_t, kEd25519SignatureBytes> ed25519_sign(
    std::string_view message,
    const Ed25519SecretKey& secret_key);

/// Base64url-encode without padding (per W3C VC Data Integrity proof
/// value spec). Used for `proof.proofValue`.
std::string base64url_no_pad(const uint8_t* bytes, size_t len);

/// Standard base64 (RFC 4648, with `=` padding). Used for embedding
/// binary fields (fingerprint bytes, encoded audio clip) inside the
/// envelope JSON.
std::string base64_standard(const uint8_t* bytes, size_t len);

/// The bytes a `*-jcs-*` Data Integrity proof signs:
/// SHA-256(JCS(proof config)) || SHA-256(JCS(document)).
std::string data_integrity_hash(const std::string& proof_config_json,
                                const std::string& document_json);

/// Apply `eddsa-jcs-2022` to the unsigned envelope JSON: sign
/// data_integrity_hash with Ed25519, attach the `proof` block, return
/// the serialised signed envelope. proofValue is multibase base58btc.
///
/// `verification_method` is copied into proof.verificationMethod —
/// typically a `did:key:` URI the verifier resolves to the public
/// key.
/// `created_iso8601_utc` is copied into proof.created.
std::string sign_envelope_eddsa_jcs(const std::string& unsigned_envelope_json,
                                    const Ed25519SecretKey& secret_key,
                                    const std::string& verification_method,
                                    const std::string& created_iso8601_utc);

/// Phase 7c hardware-signed proof. Same flow as
/// sign_envelope_eddsa_jcs, but the signing step is delegated to a
/// caller-supplied signer. Used for Secure Enclave
/// (cryptosuite="ecdsa-jcs-2019") and Android KeyStore Ed25519
/// (cryptosuite="eddsa-jcs-2022"). ECDSA signers return ASN.1/DER,
/// Ed25519 signers the fixed 64 bytes.
///
/// Throws std::runtime_error on a malformed or empty signature;
/// callers translate to fallback or skip per the
/// hardware_signing_required policy.
using HardwareSigner = std::function<std::vector<uint8_t>(std::string_view)>;

std::string sign_envelope_via_hw_jcs(const std::string& unsigned_envelope_json,
                                     const HardwareSigner& signer,
                                     const std::string& cryptosuite,
                                     const std::string& verification_method,
                                     const std::string& created_iso8601_utc);

}  // namespace sentlog::internal

#endif  // SENTLOG_INTERNAL_SIGNING_H
