#include "internal/signing.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "tweetnacl/tweetnacl.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sentlog::internal {

namespace {

using nlohmann::json;

// --- JCS canonical JSON serialisation per RFC 8785 ---------------

// Numbers per ECMAScript Number.prototype.toString (RFC 8785 §3.2.2.3).
std::string jcs_number(double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("JCS: non-finite number");
    if (value == 0.0) return "0";

    // Shortest %e precision that round-trips. Going through strtod in
    // the same locale means only the decimal point's position matters.
    char buf[40];
    for (int precision = 0; precision <= 16; ++precision) {
        std::snprintf(buf, sizeof(buf), "%.*e", precision, std::fabs(value));
        if (std::strtod(buf, nullptr) == std::fabs(value)) break;
    }
    const char* e = std::strchr(buf, 'e');
    std::string digits(1, buf[0]);
    if (e - buf > 2) digits.append(buf + 2, static_cast<size_t>(e - buf - 2));
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    const int k = static_cast<int>(digits.size());
    const int n = std::atoi(e + 1) + 1;  // value = 0.digits * 10^n

    std::string out = value < 0.0 ? "-" : "";
    if (k <= n && n <= 21) {
        out += digits + std::string(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, static_cast<size_t>(n)) + "." +
               digits.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        out += "0." + std::string(static_cast<size_t>(-n), '0') + digits;
    } else {
        out += digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        out += n - 1 < 0 ? "e-" : "e+";
        out += std::to_string(std::abs(n - 1));
    }
    return out;
}

// Object keys sort by UTF-16 code unit (RFC 8785 §3.2.3). UTF-8 byte
// order differs from that only for code points above U+FFFF.
std::u16string utf16_sort_key(const std::string& utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        const auto b = static_cast<unsigned char>(utf8[i]);
        const size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        uint32_t cp = len == 1 ? b : b & (0xFF >> (len + 1));
        for (size_t k = 1; k < len && i + k < utf8.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

void jcs_write(const json& j, std::string& out) {
    if (j.is_object()) {
        std::vector<std::pair<std::u16string, json::const_iterator>> entries;
        entries.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            entries.emplace_back(utf16_sort_key(it.key()), it);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        out += '{';
        bool first = true;
        for (const auto& entry : entries) {
            if (!first) out += ',';
            first = false;
            out += json(entry.second.key()).dump();
            out += ':';
            jcs_write(entry.second.value(), out);
        }
        out += '}';
    } else if (j.is_array()) {
        out += '[';
        bool first = true;
        for (const auto& el : j) {
            if (!first) out += ',';
            first = false;
            jcs_write(el, out);
        }
        out += ']';
    } else if (j.is_number()) {
        // Beyond 2^53 an integer no longer survives as an IEEE double.
        constexpr uint64_t kMaxExactInteger = uint64_t{1} << 53;
        const bool too_large =
            j.is_number_unsigned()
                ? j.get<uint64_t>() > kMaxExactInteger
                : j.is_number_integer() &&
                      (j.get<int64_t>() > static_cast<int64_t>(kMaxExactInteger) ||
                       j.get<int64_t>() < -static_cast<int64_t>(kMaxExactInteger));
        if (too_large) throw std::invalid_argument("JCS: integer out of range");
        out += jcs_number(j.get<double>());
    } else {
        // nlohmann emits raw UTF-8 and the minimal escapes RFC 8785 requires.
        out += j.dump();
    }
}

// --- SHA-256 (FIPS 180-4) ----------------------------------------

std::array<uint8_t, 32> sha256(std::string_view message) {
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    std::vector<uint8_t> msg(message.begin(), message.end());
    const uint64_t bits = static_cast<uint64_t>(message.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (size_t t = 0; t < 16; ++t) {
            const uint8_t* p = msg.data() + off + t * 4;
            w[t] = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) |
                   (uint32_t{p[2]} << 8) | uint32_t{p[3]};
        }
        for (size_t t = 16; t < 64; ++t) {
            const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t v[8];
        std::memcpy(v, h, sizeof(v));
        for (size_t t = 0; t < 64; ++t) {
            const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
            const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const uint32_t t1 = v[7] + s1 + ch + K[t] + w[t];
            const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
            const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const uint32_t t2 = s0 + maj;
            v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
            v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
        }
        for (size_t i = 0; i < 8; ++i) h[i] += v[i];
    }

    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 8; ++i) {
        for (size_t b = 0; b < 4; ++b) {
            out[i * 4 + b] = static_cast<uint8_t>(h[i] >> (24 - b * 8));
        }
    }
    return out;
}

// --- Multibase base58btc ("z" prefix) ----------------------------

std::string multibase_base58btc(const std::vector<uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<uint8_t> digits;  // base58, least significant first
    for (const uint8_t byte : bytes) {
        uint32_t carry = byte;
        for (auto& d : digits) {
            carry += static_cast<uint32_t>(d) << 8;
            d = static_cast<uint8_t>(carry % 58);
            carry /= 58;
        }
        while (carry > 0) {
            digits.push_back(static_cast<uint8_t>(carry % 58));
            carry /= 58;
        }
    }
    std::string out = "z";
    for (size_t i = 0; i < bytes.size() && bytes[i] == 0; ++i) out += '1';
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) out += kAlphabet[*it];
    return out;
}

// ASN.1 DER ECDSA-Sig-Value to the fixed r || s form the
// ecdsa-jcs-2019 suite puts in proofValue. Empty on malformed input.
std::vector<uint8_t> p256_der_to_raw(const std::vector<uint8_t>& der) {
    std::vector<uint8_t> raw(64, 0);
    size_t pos = 2;
    if (der.size() < 8 || der[0] != 0x30 || der[1] != der.size() - 2) return {};
    for (size_t part = 0; part < 2; ++part) {
        if (pos + 2 > der.size() || der[pos] != 0x02) return {};
        size_t len = der[pos + 1];
        pos += 2;
        if (len == 0 || pos + len > der.size()) return {};
        size_t start = pos;
        pos += len;
        while (len > 0 && der[start] == 0) { ++start; --len; }
        if (len > 32) return {};
        std::copy_n(der.begin() + static_cast<std::ptrdiff_t>(start), len,
                    raw.begin() + static_cast<std::ptrdiff_t>(32 * (part + 1) - len));
    }
    return pos == der.size() ? raw : std::vector<uint8_t>{};
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

std::string base64_standard(const uint8_t* bytes, size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const unsigned a = bytes[i];
        const unsigned b = bytes[i + 1];
        const unsigned c = bytes[i + 2];
        out += kAlphabet[(a >> 2) & 0x3f];
        out += kAlphabet[((a << 4) | (b >> 4)) & 0x3f];
        out += kAlphabet[((b << 2) | (c >> 6)) & 0x3f];
        out += kAlphabet[c & 0x3f];
        i += 3;
    }
    if (i < len) {
        const unsigned a = bytes[i];
        const unsigned b = (i + 1 < len) ? bytes[i + 1] : 0;
        out += kAlphabet[(a >> 2) & 0x3f];
        out += kAlphabet[((a << 4) | (b >> 4)) & 0x3f];
        if (i + 1 < len) {
            out += kAlphabet[(b << 2) & 0x3f];
            out += '=';
        } else {
            out += '=';
            out += '=';
        }
    }
    return out;
}

std::string jcs_canonicalize(const std::string& json_text) {
    std::string out;
    jcs_write(json::parse(json_text), out);
    return out;
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

std::string data_integrity_hash(const std::string& proof_config_json,
                                const std::string& document_json) {
    const auto proof_hash = sha256(jcs_canonicalize(proof_config_json));
    const auto doc_hash   = sha256(jcs_canonicalize(document_json));
    std::string out(reinterpret_cast<const char*>(proof_hash.data()), proof_hash.size());
    out.append(reinterpret_cast<const char*>(doc_hash.data()), doc_hash.size());
    return out;
}

// Proof creation shared by the software and hardware paths, per the
// W3C vc-di-eddsa and vc-di-ecdsa "jcs" suites.
static std::string sign_envelope_jcs_with_signer(
    const std::string& unsigned_envelope_json,
    const std::function<std::vector<uint8_t>(std::string_view)>& signer,
    const std::string& cryptosuite,
    const std::string& verification_method,
    const std::string& created_iso8601_utc) {

    auto env = json::parse(unsigned_envelope_json);

    json proof;
    proof["type"]               = "DataIntegrityProof";
    proof["cryptosuite"]        = cryptosuite;
    proof["created"]            = created_iso8601_utc;
    proof["verificationMethod"] = verification_method;
    proof["proofPurpose"]       = "assertionMethod";
    if (env.contains("@context")) proof["@context"] = env["@context"];

    std::vector<uint8_t> sig = signer(data_integrity_hash(proof.dump(), env.dump()));
    if (cryptosuite == "ecdsa-jcs-2019") sig = p256_der_to_raw(sig);
    if (sig.size() != kEd25519SignatureBytes) {
        throw std::runtime_error("signer returned a malformed signature");
    }

    proof["proofValue"] = multibase_base58btc(sig);
    env["proof"] = std::move(proof);
    return env.dump();
}

std::string sign_envelope_via_hw_jcs(const std::string& unsigned_envelope_json,
                                     const HardwareSigner& signer,
                                     const std::string& cryptosuite,
                                     const std::string& verification_method,
                                     const std::string& created_iso8601_utc) {
    return sign_envelope_jcs_with_signer(
        unsigned_envelope_json, signer, cryptosuite,
        verification_method, created_iso8601_utc);
}

std::string sign_envelope_eddsa_jcs(const std::string& unsigned_envelope_json,
                                    const Ed25519SecretKey& secret_key,
                                    const std::string& verification_method,
                                    const std::string& created_iso8601_utc) {
    return sign_envelope_jcs_with_signer(
        unsigned_envelope_json,
        [&](std::string_view hash_data) {
            const auto sig = ed25519_sign(hash_data, secret_key);
            return std::vector<uint8_t>(sig.begin(), sig.end());
        },
        "eddsa-jcs-2022", verification_method, created_iso8601_utc);
}

}  // namespace sentlog::internal
