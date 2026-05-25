#include "internal/signing.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "tweetnacl/tweetnacl.h"
}

#include <algorithm>
#include <cstring>
#include <vector>

namespace sentlog::internal {

namespace {

using nlohmann::json;

// --- JCS canonical JSON serialisation per RFC 8785 ---------------
//
// Object keys sorted lexicographically by their UTF-16 code-unit
// representation. Strings escaped per JSON minimal-escape rules.
// Numbers per the ES6 Number.prototype.toString output.
// nlohmann::json's default dump() handles the number format and
// most string escaping; we just need sorted-keys output, which
// nlohmann provides via the sort()-then-dump idiom on an ordered
// view.

void sort_keys_recursive(json& j) {
    if (j.is_object()) {
        json sorted = json::object();
        std::vector<std::string> keys;
        keys.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
            sorted[k] = std::move(j[k]);
            sort_keys_recursive(sorted[k]);
        }
        j = std::move(sorted);
    } else if (j.is_array()) {
        for (auto& el : j) sort_keys_recursive(el);
    }
}

// --- Base64url, no padding ---------------------------------------

static constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace

std::string base64url_no_pad(const uint8_t* bytes, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const unsigned a = bytes[i];
        const unsigned b = bytes[i + 1];
        const unsigned c = bytes[i + 2];
        out += kBase64UrlAlphabet[(a >> 2) & 0x3f];
        out += kBase64UrlAlphabet[((a << 4) | (b >> 4)) & 0x3f];
        out += kBase64UrlAlphabet[((b << 2) | (c >> 6)) & 0x3f];
        out += kBase64UrlAlphabet[c & 0x3f];
        i += 3;
    }
    if (i < len) {
        const unsigned a = bytes[i];
        const unsigned b = (i + 1 < len) ? bytes[i + 1] : 0;
        out += kBase64UrlAlphabet[(a >> 2) & 0x3f];
        out += kBase64UrlAlphabet[((a << 4) | (b >> 4)) & 0x3f];
        if (i + 1 < len) {
            out += kBase64UrlAlphabet[(b << 2) & 0x3f];
        }
        // No padding char.
    }
    return out;
}

std::string jcs_canonicalize(const std::string& json_text) {
    auto j = json::parse(json_text);
    sort_keys_recursive(j);
    // nlohmann's dump emits compact JSON by default — no
    // insignificant whitespace, which matches JCS rule on
    // whitespace. Numbers serialise per ES6 Number.toString for
    // values nlohmann can represent in double; the JSON we feed
    // here only carries SDK-produced doubles + strings + bools, so
    // we don't hit JCS's edge cases on very-large integers.
    return j.dump();
}

std::array<uint8_t, kEd25519SignatureBytes> ed25519_sign(
    std::string_view message,
    const Ed25519SecretKey& secret_key) {

    // tweetnacl's crypto_sign returns (signature || message), so the
    // output buffer must be 64 bytes longer than the input.
    std::vector<uint8_t> signed_msg(kEd25519SignatureBytes + message.size());
    unsigned long long signed_len = 0;
    crypto_sign(signed_msg.data(), &signed_len,
                reinterpret_cast<const unsigned char*>(message.data()),
                static_cast<unsigned long long>(message.size()),
                secret_key.data());

    std::array<uint8_t, kEd25519SignatureBytes> sig{};
    std::memcpy(sig.data(), signed_msg.data(), kEd25519SignatureBytes);
    return sig;
}

std::string sign_envelope_eddsa_jcs(const std::string& unsigned_envelope_json,
                                    const Ed25519SecretKey& secret_key,
                                    const std::string& verification_method,
                                    const std::string& created_iso8601_utc) {
    // 1. Parse the unsigned envelope.
    auto env = json::parse(unsigned_envelope_json);

    // 2. Build the proof block WITHOUT proofValue. The proofValue
    //    is computed over the canonicalised envelope-with-proof-
    //    options (per W3C VC Data Integrity §4.4). For
    //    `eddsa-jcs-2022` the signing input is:
    //      canonicalize(envelope_without_proof || proof_options)
    //    We construct the envelope_with_proof_options here and
    //    canonicalise, then replace `proof` on the final envelope
    //    with the proof-options + proofValue.
    json proof_options;
    proof_options["type"]               = "DataIntegrityProof";
    proof_options["cryptosuite"]        = "eddsa-jcs-2022";
    proof_options["created"]            = created_iso8601_utc;
    proof_options["verificationMethod"] = verification_method;
    proof_options["proofPurpose"]       = "assertionMethod";

    // Attach proof options for canonicalisation, then strip back out.
    env["proof"] = proof_options;
    const std::string canonical = jcs_canonicalize(env.dump());

    // 3. Sign the canonical bytes.
    const auto sig = ed25519_sign(canonical, secret_key);

    // 4. Reattach proof with proofValue.
    proof_options["proofValue"] = base64url_no_pad(sig.data(), sig.size());
    env["proof"] = std::move(proof_options);

    return env.dump();
}

}  // namespace sentlog::internal
