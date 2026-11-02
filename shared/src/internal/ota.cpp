#include "internal/ota.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "tweetnacl/tweetnacl.h"
}

#include <cstring>
#include <vector>

namespace sentlog::internal {

namespace {

using nlohmann::json;

// Base64url decode (no padding). Returns empty vector on any
// non-alphabet character or invalid length. Tolerates '=' padding
// if present, but does not require it.
std::vector<uint8_t> base64url_decode_no_pad(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    // Strip trailing '=' padding if present.
    while (!s.empty() && s.back() == '=') s.remove_suffix(1);

    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        const int v = val(c);
        if (v < 0) return {};
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// True for the small-order points of edwards25519, ignoring the sign
// bit. tweetnacl's crypto_sign_open doesn't reject them, and against
// such a key anyone can forge a signature without a private key.
// y = 0, 1, the two order-8 values, then p-1, p and p+1.
bool has_small_order(const Ed25519PublicKey& pubkey) {
    static constexpr std::array<std::array<uint8_t, 32>, 7> kSmallOrder = {{
        {0x00},
        {0x01},
        {0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0, 0x45, 0xc3, 0xf4,
         0x89, 0xf2, 0xef, 0x98, 0xf0, 0xd5, 0xdf, 0xac, 0x05, 0xd3, 0xc6,
         0x33, 0x39, 0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05},
        {0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f, 0xba, 0x3c, 0x0b,
         0x76, 0x0d, 0x10, 0x67, 0x0f, 0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39,
         0xcc, 0xc6, 0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a},
        {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
        {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
        {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    }};
    for (const auto& point : kSmallOrder) {
        if (std::memcmp(pubkey.data(), point.data(), 31) == 0 &&
            (pubkey[31] & 0x7f) == point[31]) {
            return true;
        }
    }
    return false;
}

bool ed25519_verify_detached(const uint8_t* sig,
                             const uint8_t* msg,
                             size_t         msg_len,
                             const Ed25519PublicKey& pubkey) {
    if (has_small_order(pubkey)) return false;

    // tweetnacl's crypto_sign_open verifies a (sig || msg) buffer.
    // Construct one and feed it.
    std::vector<uint8_t> signed_msg(kEd25519SignatureBytes + msg_len);
    std::memcpy(signed_msg.data(), sig, kEd25519SignatureBytes);
    if (msg_len > 0) {
        std::memcpy(signed_msg.data() + kEd25519SignatureBytes, msg, msg_len);
    }
    std::vector<uint8_t> opened(signed_msg.size());
    unsigned long long opened_len = 0;
    const int rc = crypto_sign_open(
        opened.data(), &opened_len,
        signed_msg.data(),
        static_cast<unsigned long long>(signed_msg.size()),
        pubkey.data());
    return rc == 0;
}

}  // namespace

ManifestVerifyStatus parse_and_verify_manifest(
    std::string_view envelope_json,
    const Ed25519PublicKey& active_pubkey,
    const Ed25519PublicKey& standby_pubkey,
    Manifest& out) {

    json env;
    try {
        env = json::parse(envelope_json);
    } catch (const json::exception&) {
        return ManifestVerifyStatus::InvalidJson;
    }

    if (!env.is_object()) return ManifestVerifyStatus::InvalidJson;

    // Envelope-level fields.
    if (!env.contains("manifest") || !env["manifest"].is_object() ||
        !env.contains("key_id")   || !env["key_id"].is_string()   ||
        !env.contains("signature_b64url") || !env["signature_b64url"].is_string()) {
        return ManifestVerifyStatus::MissingFields;
    }

    const std::string key_id = env["key_id"].get<std::string>();
    const Ed25519PublicKey* pubkey = nullptr;
    if (key_id == "active")       pubkey = &active_pubkey;
    else if (key_id == "standby") pubkey = &standby_pubkey;
    else return ManifestVerifyStatus::UnknownKeyId;

    // Manifest-level fields.
    const auto& m = env["manifest"];
    if (!m.contains("schema_version")   || !m["schema_version"].is_string()   ||
        !m.contains("manifest_version") || !m["manifest_version"].is_string() ||
        !m.contains("bundle_url")       || !m["bundle_url"].is_string()       ||
        !m.contains("bundle_sha256")    || !m["bundle_sha256"].is_string()    ||
        !m.contains("bundle_size")      || !m["bundle_size"].is_number_integer() ||
        !m.contains("issued_at_ms")     || !m["issued_at_ms"].is_number_integer()) {
        return ManifestVerifyStatus::MissingFields;
    }

    if (m["schema_version"].get<std::string>() != kManifestSchemaSupported) {
        return ManifestVerifyStatus::UnsupportedSchemaVersion;
    }

    // Decode the signature.
    const auto sig_bytes = base64url_decode_no_pad(
        env["signature_b64url"].get<std::string>());
    if (sig_bytes.size() != kEd25519SignatureBytes) {
        return ManifestVerifyStatus::BadSignature;
    }

    // Verify against the JCS-canonicalised manifest sub-object.
    // jcs_canonicalize takes JSON text + reserialises; feeding it the
    // dump of the sub-object is the simplest path and gives the same
    // byte stream a publisher producing JCS would have signed.
    const std::string canonical = jcs_canonicalize(m.dump());
    if (!ed25519_verify_detached(
            sig_bytes.data(),
            reinterpret_cast<const uint8_t*>(canonical.data()),
            canonical.size(),
            *pubkey)) {
        return ManifestVerifyStatus::BadSignature;
    }

    out.schema_version   = m["schema_version"].get<std::string>();
    out.manifest_version = m["manifest_version"].get<std::string>();
    out.bundle_url       = m["bundle_url"].get<std::string>();
    out.bundle_sha256    = m["bundle_sha256"].get<std::string>();
    out.bundle_size      = m["bundle_size"].get<int64_t>();
    out.issued_at_ms     = m["issued_at_ms"].get<int64_t>();
    return ManifestVerifyStatus::OK;
}

}  // namespace sentlog::internal
