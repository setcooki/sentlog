#include "internal/signing.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "tweetnacl/tweetnacl.h"
}

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "internal/config.h"
#include "internal/envelope.h"

using nlohmann::json;
using sentlog::internal::base64url_no_pad;
using sentlog::internal::build_envelope_unsigned;
using sentlog::internal::Config;
using sentlog::internal::data_integrity_hash;
using sentlog::internal::ed25519_sign;
using sentlog::internal::Ed25519SecretKey;
using sentlog::internal::jcs_canonicalize;
using sentlog::internal::kEd25519PublicKeyBytes;
using sentlog::internal::kEd25519SignatureBytes;
using sentlog::internal::sign_envelope_eddsa_jcs;
using sentlog::internal::sign_envelope_via_hw_jcs;

namespace {

std::vector<uint8_t> hex_decode(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::vector<uint8_t> base58_decode(const std::string& text) {
    const std::string alphabet =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    std::vector<uint8_t> out;  // big-endian
    for (const char c : text) {
        uint32_t carry = static_cast<uint32_t>(alphabet.find(c));
        for (auto it = out.rbegin(); it != out.rend(); ++it) {
            carry += static_cast<uint32_t>(*it) * 58;
            *it = static_cast<uint8_t>(carry & 0xff);
            carry >>= 8;
        }
        while (carry > 0) {
            out.insert(out.begin(), static_cast<uint8_t>(carry & 0xff));
            carry >>= 8;
        }
    }
    return out;
}

// RFC 8032 §7.1 test vector 1 — pinned (seed, pubkey) pair so we
// don't need to call tweetnacl's keypair derivation (which depends
// on randombytes, which the production stub deliberately aborts).
// The pubkey is the verified-correct derivation of this seed.
//
//   SEED:   9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60
//   PUBKEY: d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
//
// The same vector verifies tweetnacl's Ed25519 against the RFC, so
// any drift in the vendored tweetnacl would show up here.
Ed25519SecretKey make_test_secret_key() {
    Ed25519SecretKey sk{};
    static const uint8_t kSeed[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
    };
    static const uint8_t kPubKey[32] = {
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
    };
    for (size_t i = 0; i < 32; ++i) sk[i]      = kSeed[i];
    for (size_t i = 0; i < 32; ++i) sk[32 + i] = kPubKey[i];
    return sk;
}

std::array<uint8_t, kEd25519PublicKeyBytes> pubkey_from(const Ed25519SecretKey& sk) {
    std::array<uint8_t, kEd25519PublicKeyBytes> pk{};
    std::memcpy(pk.data(), sk.data() + 32, kEd25519PublicKeyBytes);
    return pk;
}

bool verify_with_tweetnacl(const std::string& message,
                           const std::array<uint8_t, kEd25519SignatureBytes>& sig,
                           const std::array<uint8_t, kEd25519PublicKeyBytes>& pk) {
    // crypto_sign_open expects (signature || message); the unsigned
    // crypto_sign_open output is the recovered message bytes.
    std::vector<uint8_t> signed_msg;
    signed_msg.reserve(kEd25519SignatureBytes + message.size());
    signed_msg.insert(signed_msg.end(), sig.begin(), sig.end());
    signed_msg.insert(signed_msg.end(),
                      reinterpret_cast<const uint8_t*>(message.data()),
                      reinterpret_cast<const uint8_t*>(message.data()) + message.size());

    std::vector<uint8_t> recovered(signed_msg.size());
    unsigned long long mlen = 0;
    const int rc = crypto_sign_open(recovered.data(), &mlen,
                                    signed_msg.data(),
                                    signed_msg.size(),
                                    pk.data());
    return rc == 0;
}

const char* kReferenceResultJson = R"({
    "schema_version": "1",
    "transcript": {"text": "hi", "language": "en", "confidence": 0.9},
    "liveness":   {"score": 0.88, "replay_risk": 0.05, "synthetic_risk": 0.07},
    "presence":   {"summary": 0.9, "audio_source": 1.0},
    "direction":  0.5,
    "confidence": 0.6,
    "thresholds_met": false,
    "engagement": {"score": 0.65, "voice_score": 0.65, "model_status": "training"},
    "fingerprint": {"algo": "sentlog-pcp-v1", "bytes": ""},
    "model_versions": {"liveness": "x", "asr": "x", "prosody": "x",
                       "vad": "x", "manifest": "x"},
    "diagnostics":  {"duration_ms": 1500, "audio_quality": "moderate"},
    "evidence_grade": "DeviceUnattested",
    "extensions": {}
})";

}  // namespace

// --- JCS canonicalisation ---

TEST(JcsTest, SortsObjectKeysLexicographically) {
    const std::string c = jcs_canonicalize(R"({"b":2,"a":1,"c":3})");
    EXPECT_EQ(c, R"({"a":1,"b":2,"c":3})");
}

TEST(JcsTest, RecursesIntoNestedObjects) {
    const std::string c = jcs_canonicalize(R"({"x":{"z":1,"a":2}})");
    EXPECT_EQ(c, R"({"x":{"a":2,"z":1}})");
}

TEST(JcsTest, PreservesArrayOrder) {
    const std::string c = jcs_canonicalize(R"({"a":[3,1,2]})");
    EXPECT_EQ(c, R"({"a":[3,1,2]})");
}

TEST(JcsTest, StripsInsignificantWhitespace) {
    const std::string c = jcs_canonicalize("{ \"a\" :  1 ,  \"b\":2 }");
    EXPECT_EQ(c, R"({"a":1,"b":2})");
}

TEST(JcsTest, IdenticalForReencodedInput) {
    // The fundamental JCS guarantee — same logical JSON, same bytes.
    const std::string a = jcs_canonicalize(R"({"a":1,"b":2})");
    const std::string b = jcs_canonicalize(R"({"b":2, "a":1})");
    EXPECT_EQ(a, b);
}

// --- base64url no-pad ---

TEST(Base64UrlTest, EncodesRfc4648TestVectors) {
    // RFC 4648 §10 test vectors, base64url, no padding.
    EXPECT_EQ(base64url_no_pad(reinterpret_cast<const uint8_t*>("f"),  1), "Zg");
    EXPECT_EQ(base64url_no_pad(reinterpret_cast<const uint8_t*>("fo"), 2), "Zm8");
    EXPECT_EQ(base64url_no_pad(reinterpret_cast<const uint8_t*>("foo"), 3), "Zm9v");
    EXPECT_EQ(base64url_no_pad(reinterpret_cast<const uint8_t*>("foob"), 4), "Zm9vYg");
    EXPECT_EQ(base64url_no_pad(reinterpret_cast<const uint8_t*>("fooba"), 5), "Zm9vYmE");
    EXPECT_EQ(base64url_no_pad(reinterpret_cast<const uint8_t*>("foobar"), 6), "Zm9vYmFy");
}

TEST(Base64UrlTest, UsesUrlSafeAlphabet) {
    // Bytes 0xfb, 0xff = '+ /' in base64; '- _' in base64url.
    const uint8_t bytes[] = {0xfb, 0xff};
    EXPECT_EQ(base64url_no_pad(bytes, 2), "-_8");
}

// --- Ed25519 sign + verify round-trip ---

TEST(Ed25519Test, SignProducesVerifiableSignature) {
    const auto sk = make_test_secret_key();
    const auto pk = pubkey_from(sk);
    const std::string msg = "the cat sat on the mat";
    const auto sig = ed25519_sign(msg, sk);
    EXPECT_TRUE(verify_with_tweetnacl(msg, sig, pk));
}

TEST(Ed25519Test, SignatureIsDeterministic) {
    const auto sk = make_test_secret_key();
    const std::string msg = "deterministic ed25519";
    const auto a = ed25519_sign(msg, sk);
    const auto b = ed25519_sign(msg, sk);
    EXPECT_EQ(a, b);
}

TEST(Ed25519Test, DifferentMessageDifferentSignature) {
    const auto sk = make_test_secret_key();
    const auto a = ed25519_sign("a", sk);
    const auto b = ed25519_sign("b", sk);
    EXPECT_NE(a, b);
}

// --- sign_envelope_eddsa_jcs end-to-end ---

TEST(SignEnvelopeTest, AddsProofBlockWithExpectedShape) {
    Config cfg;
    cfg.issuer = "did:example:integrator";
    const std::string unsigned_env = build_envelope_unsigned(
        cfg, kReferenceResultJson, "2026-05-25T10:00:00.000Z");

    const auto sk = make_test_secret_key();
    const std::string signed_env_str = sign_envelope_eddsa_jcs(
        unsigned_env, sk, "did:key:zTestPubKey",
        "2026-05-25T10:00:00.000Z");

    const auto env = json::parse(signed_env_str);
    ASSERT_TRUE(env.contains("proof"));
    const auto& proof = env["proof"];
    EXPECT_EQ(proof["type"],               "DataIntegrityProof");
    EXPECT_EQ(proof["cryptosuite"],        "eddsa-jcs-2022");
    EXPECT_EQ(proof["created"],            "2026-05-25T10:00:00.000Z");
    EXPECT_EQ(proof["verificationMethod"], "did:key:zTestPubKey");
    EXPECT_EQ(proof["proofPurpose"],       "assertionMethod");
    ASSERT_TRUE(proof.contains("proofValue"));
    EXPECT_FALSE(proof["proofValue"].get<std::string>().empty());
}

TEST(SignEnvelopeTest, SignatureIsDeterministicForSameInputs) {
    Config cfg;
    cfg.issuer = "did:example:integrator";
    const std::string unsigned_env = build_envelope_unsigned(
        cfg, kReferenceResultJson, "2026-05-25T10:00:00.000Z");
    const auto sk = make_test_secret_key();

    const std::string a = sign_envelope_eddsa_jcs(
        unsigned_env, sk, "did:key:k", "2026-05-25T10:00:00.000Z");
    const std::string b = sign_envelope_eddsa_jcs(
        unsigned_env, sk, "did:key:k", "2026-05-25T10:00:00.000Z");
    EXPECT_EQ(json::parse(a)["proof"]["proofValue"],
              json::parse(b)["proof"]["proofValue"]);
}

TEST(SignEnvelopeTest, TamperingChangesVerification) {
    Config cfg;
    cfg.issuer = "did:example:integrator";
    const std::string unsigned_env = build_envelope_unsigned(
        cfg, kReferenceResultJson, "2026-05-25T10:00:00.000Z");
    const auto sk = make_test_secret_key();
    const auto pk = pubkey_from(sk);
    const std::string signed_env_str = sign_envelope_eddsa_jcs(
        unsigned_env, sk, "did:key:k", "2026-05-25T10:00:00.000Z");

    auto env = json::parse(signed_env_str);
    // Tamper the payload after signing.
    env["credentialSubject"]["payload"]["transcript"]["text"] = "TAMPERED";

    const std::string proof_value = env["proof"]["proofValue"].get<std::string>();
    auto doc = env;
    doc.erase("proof");
    auto proof_config = env["proof"];
    proof_config.erase("proofValue");
    const std::string canonical =
        data_integrity_hash(proof_config.dump(), doc.dump());

    ASSERT_EQ(proof_value[0], 'z');
    const auto sig_bytes = base58_decode(proof_value.substr(1));
    ASSERT_EQ(sig_bytes.size(), kEd25519SignatureBytes);
    std::array<uint8_t, kEd25519SignatureBytes> sig{};
    std::copy(sig_bytes.begin(), sig_bytes.end(), sig.begin());
    EXPECT_FALSE(verify_with_tweetnacl(canonical, sig, pk))
        << "tampered payload should not verify against the original signature";
}

// --- RFC 8785 ---

TEST(JcsTest, NumbersMatchRfc8785) {
    // RFC 8785 Appendix B, plus the values SDK scores actually take.
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"0", "0"}, {"-0.0", "0"}, {"0.0", "0"}, {"1.0", "1"},
        {"5e-324", "5e-324"},
        {"1.7976931348623157e308", "1.7976931348623157e+308"},
        {"-1.7976931348623157e308", "-1.7976931348623157e+308"},
        {"9007199254740992", "9007199254740992"},
        {"295147905179352830000.0", "295147905179352830000"},
        {"1e21", "1e+21"}, {"1e20", "100000000000000000000"},
        {"1e-7", "1e-7"}, {"0.000001", "0.000001"}, {"1e-05", "0.00001"},
        {"333333333.33333329", "333333333.3333333"},
        {"4.5", "4.5"}, {"2e-3", "0.002"},
        {"0.6000000238418579", "0.6000000238418579"},
    };
    for (const auto& c : cases) {
        EXPECT_EQ(jcs_canonicalize("[" + c.first + "]"), "[" + c.second + "]")
            << c.first;
    }
}

TEST(JcsTest, RejectsIntegerBeyondDoublePrecision) {
    EXPECT_THROW(jcs_canonicalize("[9007199254740993]"), std::invalid_argument);
}

TEST(JcsTest, KeysSortByUtf16CodeUnit) {
    // RFC 8785 §3.2.3 example. U+1F600 is D83D DE00 in UTF-16, so it
    // sorts before U+FB33, the reverse of their UTF-8 byte order.
    const std::string in =
        "{\"\u20ac\":1,\"\\r\":2,\"\xF0\x9F\x98\x80\":3,"
        "\"\xEF\xAC\xB3\":4,\"1\":5,\"\xC2\x80\":6,\"\xC3\xB6\":7}";
    EXPECT_EQ(jcs_canonicalize(in),
              "{\"\\r\":2,\"1\":5,\"\xC2\x80\":6,\"\xC3\xB6\":7,"
              "\"\u20ac\":1,\"\xF0\x9F\x98\x80\":3,\"\xEF\xAC\xB3\":4}");
}

TEST(JcsTest, StringsStayRawUtf8WithMinimalEscapes) {
    EXPECT_EQ(jcs_canonicalize(R"(["gr\u00fc\u00df\n\u001f\/"])"),
              "[\"gr\xC3\xBC\xC3\x9F\\n\\u001f/\"]");
}

// --- W3C Data Integrity test vectors ---
//
// From w3c/vc-di-eddsa TestVectors/eddsa-jcs-2022 and w3c/vc-di-ecdsa
// TestVectors/ecdsa-jcs-2019-p256.

namespace {
const char* kW3cUnsigned =
    R"json({"@context":["https://www.w3.org/ns/credentials/v2","https://www.w3.org/ns/credentials/examples/v2"],"id":"urn:uuid:58172aac-d8ba-11ed-83dd-0b3aef56cc33","type":["VerifiableCredential","AlumniCredential"],"name":"Alumni Credential","description":"A minimum viable example of an Alumni Credential.","issuer":"https://vc.example/issuers/5678","validFrom":"2023-01-01T00:00:00Z","credentialSubject":{"id":"did:example:abcdefgh","alumniOf":"The School of Examples"}})json";
const char* kW3cCreated = "2023-02-24T23:36:38Z";
}  // namespace

TEST(W3cVectorTest, Eddsa) {
    Ed25519SecretKey sk{};
    const auto seed = hex_decode("c96ef9ea10c5e414c471723aff9de72c35fa5b70fae97e8832ecac7d2e2b8ed6");
    const auto pub  = hex_decode("b00d8d938e7f773d51565aad36a623f5344f7f5d1960f9cf3e8e12620ea2810f");
    std::copy(seed.begin(), seed.end(), sk.begin());
    std::copy(pub.begin(), pub.end(), sk.begin() + 32);

    const auto env = json::parse(sign_envelope_eddsa_jcs(
        kW3cUnsigned, sk, "did:key:z6MkrJVnaZkeFzdQyMZu1cgjg7k1pZZ6pvBQ7XJPt4swbTQ2#z6MkrJVnaZkeFzdQyMZu1cgjg7k1pZZ6pvBQ7XJPt4swbTQ2", kW3cCreated));
    EXPECT_EQ(env["proof"]["proofValue"].get<std::string>(),
              "z2HnFSSPPBzR36zdDgK8PbEHeXbR56YF24jwMpt3R1eHXQzJDMWS93FCzpvJpwTWd3GAVFuUfjoJdcnTMuVor51aX");
}

TEST(W3cVectorTest, EcdsaSignsCombinedHashAndEncodesRawSignature) {
    const auto raw = hex_decode("f15c3b599eb9b3cad05df9d8e8b39a70a86375833b53743c764ac0a88c4457d60707fd7d073e03d906130631d87803f80a9824dc9939632ba92d418181be9d16");
    std::vector<uint8_t> der = {0x30, 0x00, 0x02};
    const auto append_int = [&der](auto first, auto last) {
        const bool pad = (*first & 0x80) != 0;
        der.push_back(static_cast<uint8_t>(32 + (pad ? 1 : 0)));
        if (pad) der.push_back(0);
        der.insert(der.end(), first, last);
    };
    append_int(raw.begin(), raw.begin() + 32);
    der.push_back(0x02);
    append_int(raw.begin() + 32, raw.end());
    der[1] = static_cast<uint8_t>(der.size() - 2);

    std::vector<uint8_t> signed_bytes;
    const auto env = json::parse(sign_envelope_via_hw_jcs(
        kW3cUnsigned,
        [&](std::string_view message) {
            signed_bytes.assign(message.begin(), message.end());
            return der;
        },
        "ecdsa-jcs-2019", "did:key:zDnaepBuvsQ8cpsWrVKw8fbpGpvPeNSjVPTWoq6cRqaYzBKVP#zDnaepBuvsQ8cpsWrVKw8fbpGpvPeNSjVPTWoq6cRqaYzBKVP", kW3cCreated));

    EXPECT_EQ(signed_bytes, hex_decode("fe5799489119c7fe3c528715e72bd39d2ec6b4ab345978df32e9a9312648ec2559b7cb6251b8991add1ce0bc83107e3db9dbbab5bd2c28f687db1a03abc92f19"));
    EXPECT_EQ(env["proof"]["proofValue"].get<std::string>(),
              "z5ptCet75SaEgzG4v4zJhbJtfNi74Wv7Fq15hhKouJQQjEPQvPZKaYxcMXAMLPQS2FXrkCWokNJkFVkwxNzZfD5oT");
}

TEST(W3cVectorTest, RejectsMalformedHardwareSignature) {
    const auto sign_with = [](std::vector<uint8_t> sig) {
        return sign_envelope_via_hw_jcs(
            kW3cUnsigned, [&](std::string_view) { return sig; },
            "ecdsa-jcs-2019", "did:example:k", kW3cCreated);
    };
    EXPECT_THROW(sign_with({}), std::runtime_error);
    EXPECT_THROW(sign_with(std::vector<uint8_t>(64, 0x42)), std::runtime_error);
    EXPECT_THROW(sign_with({0x30, 0x04, 0x02, 0x7f, 0x01, 0x01}), std::runtime_error);
}

// --- Fixture shared with tools/evidence-verifier/selftest.py ---

TEST(VerifierFixtureTest, SignedEnvelopeMatchesCommittedFixture) {
    // A Python verifier reading this file proves C++ and Python agree
    // on the signed bytes. Non-ASCII, exponent-range floats and an
    // astral-plane key are the inputs that used to diverge.
    const std::string unsigned_env = R"json({
        "@context": ["https://www.w3.org/ns/credentials/v2"],
        "type": ["VerifiableCredential", "SentlogEvidence"],
        "issuer": "did:example:integrator",
        "validFrom": "2026-05-25T10:00:00.000Z",
        "credentialSubject": {
            "envelope_format": "1.0",
            "evidence_grade": "DeviceUnattested",
            "payload": {
                "transcript": {"text": "Grüße aus München — 你好 😀"},
                "direction": 0.0,
                "liveness": {"score": 0.6000000238418579, "replay_risk": 1e-05},
                "😀": 1, "\ufb33": 2,
                "duration_ms": 1500
            }
        }
    })json";
    const std::string actual = sign_envelope_eddsa_jcs(
        unsigned_env, make_test_secret_key(), "did:example:k",
        "2026-05-25T10:00:00.000Z");

    const std::string path = std::string(SENTLOG_REPO_ROOT) +
        "/tools/evidence-verifier/fixtures/cpp_signed_envelope.json";
    if (std::getenv("SENTLOG_UPDATE_FIXTURES") != nullptr) {
        std::ofstream(path) << actual << "\n";
    }
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open());
    std::string expected;
    std::getline(file, expected);
    EXPECT_EQ(actual, expected);
}
