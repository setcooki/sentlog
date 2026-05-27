#include "internal/ota.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "internal/signing.h"

using nlohmann::json;
using sentlog::internal::base64url_no_pad;
using sentlog::internal::ed25519_sign;
using sentlog::internal::Ed25519PublicKey;
using sentlog::internal::Ed25519SecretKey;
using sentlog::internal::jcs_canonicalize;
using sentlog::internal::Manifest;
using sentlog::internal::ManifestVerifyStatus;
using sentlog::internal::parse_and_verify_manifest;

namespace {

// RFC 8032 §7.1 vector 1 — used as the "active" key.
Ed25519SecretKey make_active_secret_key() {
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

// RFC 8032 §7.1 vector 2 — used as the "standby" key.
Ed25519SecretKey make_standby_secret_key() {
    Ed25519SecretKey sk{};
    static const uint8_t kSeed[32] = {
        0x4c, 0xcd, 0x08, 0x9b, 0x28, 0xff, 0x96, 0xda,
        0x9d, 0xb6, 0xc3, 0x46, 0xec, 0x11, 0x4e, 0x0f,
        0x5b, 0x8a, 0x31, 0x9f, 0x35, 0xab, 0xa6, 0x24,
        0xda, 0x8c, 0xf6, 0xed, 0x4f, 0xb8, 0xa6, 0xfb,
    };
    static const uint8_t kPubKey[32] = {
        0x3d, 0x40, 0x17, 0xc3, 0xe8, 0x43, 0x89, 0x5a,
        0x92, 0xb7, 0x0a, 0xa7, 0x4d, 0x1b, 0x7e, 0xbc,
        0x9c, 0x98, 0x2c, 0xcf, 0x2e, 0xc4, 0x96, 0x8c,
        0xc0, 0xcd, 0x55, 0xf1, 0x2a, 0xf4, 0x66, 0x0c,
    };
    for (size_t i = 0; i < 32; ++i) sk[i]      = kSeed[i];
    for (size_t i = 0; i < 32; ++i) sk[32 + i] = kPubKey[i];
    return sk;
}

Ed25519PublicKey pubkey_from(const Ed25519SecretKey& sk) {
    Ed25519PublicKey pk{};
    std::memcpy(pk.data(), sk.data() + 32, pk.size());
    return pk;
}

json make_manifest_object() {
    json m;
    m["schema_version"]   = "1";
    m["manifest_version"] = "0.0.5-phase7-a";
    m["bundle_url"]       = "https://example.invalid/sentlog/models-0.0.5.tar";
    m["bundle_sha256"]    =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    m["bundle_size"]      = 4096;
    m["issued_at_ms"]     = static_cast<int64_t>(1735689600000);
    return m;
}

// Build a signed envelope mirroring what a real manifest publisher
// would produce: JCS the manifest sub-object, sign those bytes, wrap
// in {manifest, key_id, signature_b64url}.
std::string build_signed_envelope(const json& manifest,
                                  const std::string& key_id,
                                  const Ed25519SecretKey& sk) {
    const std::string canonical = jcs_canonicalize(manifest.dump());
    const auto sig = ed25519_sign(canonical, sk);
    json env;
    env["manifest"]         = manifest;
    env["key_id"]           = key_id;
    env["signature_b64url"] = base64url_no_pad(sig.data(), sig.size());
    return env.dump();
}

}  // namespace

TEST(OtaManifestTest, VerifiesWithActiveKey) {
    const auto active_sk  = make_active_secret_key();
    const auto standby_sk = make_standby_secret_key();
    const auto envelope = build_signed_envelope(
        make_manifest_object(), "active", active_sk);

    Manifest m;
    const auto rc = parse_and_verify_manifest(
        envelope, pubkey_from(active_sk), pubkey_from(standby_sk), m);
    ASSERT_EQ(rc, ManifestVerifyStatus::OK);
    EXPECT_EQ(m.schema_version,   "1");
    EXPECT_EQ(m.manifest_version, "0.0.5-phase7-a");
    EXPECT_EQ(m.bundle_url,
              "https://example.invalid/sentlog/models-0.0.5.tar");
    EXPECT_EQ(m.bundle_size,  4096);
    EXPECT_EQ(m.issued_at_ms, 1735689600000);
}

TEST(OtaManifestTest, VerifiesWithStandbyKey) {
    const auto active_sk  = make_active_secret_key();
    const auto standby_sk = make_standby_secret_key();
    const auto envelope = build_signed_envelope(
        make_manifest_object(), "standby", standby_sk);

    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(envelope,
                                        pubkey_from(active_sk),
                                        pubkey_from(standby_sk), m),
              ManifestVerifyStatus::OK);
}

TEST(OtaManifestTest, RejectsActiveSigVerifiedAgainstStandbySlot) {
    // Signed with active key but key_id says "standby" — must fail.
    const auto active_sk  = make_active_secret_key();
    const auto standby_sk = make_standby_secret_key();
    const auto envelope = build_signed_envelope(
        make_manifest_object(), "standby", active_sk);  // mismatched

    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(envelope,
                                        pubkey_from(active_sk),
                                        pubkey_from(standby_sk), m),
              ManifestVerifyStatus::BadSignature);
}

TEST(OtaManifestTest, RejectsUnknownKeyId) {
    const auto active_sk  = make_active_secret_key();
    const auto standby_sk = make_standby_secret_key();
    const auto envelope = build_signed_envelope(
        make_manifest_object(), "rotated-2099", active_sk);

    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(envelope,
                                        pubkey_from(active_sk),
                                        pubkey_from(standby_sk), m),
              ManifestVerifyStatus::UnknownKeyId);
}

TEST(OtaManifestTest, RejectsTamperedManifestField) {
    const auto active_sk  = make_active_secret_key();
    const auto standby_sk = make_standby_secret_key();

    // Sign the original manifest, then mutate a field after signing.
    auto manifest = make_manifest_object();
    const std::string canonical = jcs_canonicalize(manifest.dump());
    const auto sig = ed25519_sign(canonical, active_sk);

    manifest["bundle_sha256"] =
        "0000000000000000000000000000000000000000000000000000000000000000";

    json env;
    env["manifest"]         = manifest;
    env["key_id"]           = "active";
    env["signature_b64url"] = base64url_no_pad(sig.data(), sig.size());

    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(env.dump(),
                                        pubkey_from(active_sk),
                                        pubkey_from(standby_sk), m),
              ManifestVerifyStatus::BadSignature);
}

TEST(OtaManifestTest, RejectsTamperedSignature) {
    const auto active_sk  = make_active_secret_key();
    const auto standby_sk = make_standby_secret_key();
    auto envelope_str = build_signed_envelope(
        make_manifest_object(), "active", active_sk);

    auto env = json::parse(envelope_str);
    std::string sig = env["signature_b64url"].get<std::string>();
    // Flip a bit by swapping one base64url char to another.
    sig[0] = (sig[0] == 'A') ? 'B' : 'A';
    env["signature_b64url"] = sig;

    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(env.dump(),
                                        pubkey_from(active_sk),
                                        pubkey_from(standby_sk), m),
              ManifestVerifyStatus::BadSignature);
}

TEST(OtaManifestTest, RejectsMalformedJson) {
    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(
                  "{not-json", pubkey_from(make_active_secret_key()),
                  pubkey_from(make_standby_secret_key()), m),
              ManifestVerifyStatus::InvalidJson);
}

TEST(OtaManifestTest, RejectsMissingManifestField) {
    json env;
    env["key_id"]           = "active";
    env["signature_b64url"] = "AAAA";
    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(
                  env.dump(),
                  pubkey_from(make_active_secret_key()),
                  pubkey_from(make_standby_secret_key()), m),
              ManifestVerifyStatus::MissingFields);
}

TEST(OtaManifestTest, RejectsMissingInnerField) {
    auto manifest = make_manifest_object();
    manifest.erase("bundle_url");
    const auto envelope = build_signed_envelope(
        manifest, "active", make_active_secret_key());
    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(
                  envelope,
                  pubkey_from(make_active_secret_key()),
                  pubkey_from(make_standby_secret_key()), m),
              ManifestVerifyStatus::MissingFields);
}

TEST(OtaManifestTest, RejectsUnsupportedSchemaVersion) {
    auto manifest = make_manifest_object();
    manifest["schema_version"] = "2";
    const auto envelope = build_signed_envelope(
        manifest, "active", make_active_secret_key());
    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(
                  envelope,
                  pubkey_from(make_active_secret_key()),
                  pubkey_from(make_standby_secret_key()), m),
              ManifestVerifyStatus::UnsupportedSchemaVersion);
}

TEST(OtaManifestTest, RejectsWrongLengthSignature) {
    auto manifest = make_manifest_object();
    json env;
    env["manifest"]         = manifest;
    env["key_id"]           = "active";
    env["signature_b64url"] = "AAAA";  // 3 decoded bytes — not 64
    Manifest m;
    EXPECT_EQ(parse_and_verify_manifest(
                  env.dump(),
                  pubkey_from(make_active_secret_key()),
                  pubkey_from(make_standby_secret_key()), m),
              ManifestVerifyStatus::BadSignature);
}
