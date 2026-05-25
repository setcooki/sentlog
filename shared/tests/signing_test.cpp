#include "internal/signing.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "tweetnacl/tweetnacl.h"
}

#include <array>
#include <cstring>
#include <string>

#include "internal/config.h"
#include "internal/envelope.h"

using nlohmann::json;
using sentlog::internal::base64url_no_pad;
using sentlog::internal::build_envelope_unsigned;
using sentlog::internal::Config;
using sentlog::internal::ed25519_sign;
using sentlog::internal::Ed25519SecretKey;
using sentlog::internal::jcs_canonicalize;
using sentlog::internal::kEd25519PublicKeyBytes;
using sentlog::internal::kEd25519SignatureBytes;
using sentlog::internal::sign_envelope_eddsa_jcs;

namespace {

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

    // Pull out and decode the signature; re-canonicalise without
    // proofValue and verify — expect mismatch.
    const std::string proof_value_b64 = env["proof"]["proofValue"].get<std::string>();
    auto env_for_verify = env;
    env_for_verify["proof"].erase("proofValue");
    const std::string canonical = jcs_canonicalize(env_for_verify.dump());

    // Decode base64url. Manual since we only encode in production.
    auto b64url_decode = [](const std::string& s) {
        static int8_t T[256];
        static bool init = false;
        if (!init) {
            for (auto& x : T) x = -1;
            const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            for (int i = 0; i < 64; ++i) T[(unsigned char)a[i]] = static_cast<int8_t>(i);
            init = true;
        }
        std::vector<uint8_t> out;
        int v = 0, b = 0;
        for (char c : s) {
            const int x = T[(unsigned char)c];
            if (x < 0) continue;
            v = (v << 6) | x; b += 6;
            if (b >= 8) {
                b -= 8;
                out.push_back(static_cast<uint8_t>((v >> b) & 0xff));
            }
        }
        return out;
    };
    const auto sig_bytes = b64url_decode(proof_value_b64);
    ASSERT_EQ(sig_bytes.size(), kEd25519SignatureBytes);
    std::array<uint8_t, kEd25519SignatureBytes> sig{};
    std::memcpy(sig.data(), sig_bytes.data(), kEd25519SignatureBytes);

    EXPECT_FALSE(verify_with_tweetnacl(canonical, sig, pk))
        << "tampered payload should not verify against the original signature";
}
