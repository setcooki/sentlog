// Phase 5e: device-attestation integration tests.
//
// Validates that:
//   - a pushed attestation token shows up base64-encoded under
//     credentialSubject.device_attestation with the supplied type
//   - evidence_grade gets promoted from DeviceUnattested to
//     DeviceAttested when an attestation is present
//   - clearing (pushing empty bytes) removes the field + reverts
//     the grade
//   - the C ABI rejects empty/null attestation_type when a token
//     is being pushed
//
// The "never demote from EnclaveSigned" rule is covered by inspection
// of envelope.cpp (cur == "DeviceUnattested" guard); the hw-signing
// path that produces EnclaveSigned has its own tests in
// hw_signer_test.cpp.

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

struct DispatchCapture {
    std::mutex                m;
    std::vector<std::string>  envelopes;
};

void dispatcher_cb(const char* /*n*/, const char* /*k*/,
                   const char* /*sj*/, const char* envelope_json, void* ud) {
    auto* c = static_cast<DispatchCapture*>(ud);
    std::lock_guard<std::mutex> lock(c->m);
    c->envelopes.push_back(envelope_json ? envelope_json : "");
}

const char* kCfg = R"({
  "issuer":"did:example:x",
  "forwarders":[{"name":"a","projection":"ScoresOnly",
                 "sink":{"kind":"callback"}}]
})";

void run_one(sentlog_handle_t* h) {
    sentlog_start_capture(h);
    sentlog_evaluate(h, [](sentlog_status_t, const char*, void*){}, nullptr);
}

}  // namespace

TEST(DeviceAttestationTest, AttestedTokenPromotesGradeAndAppears) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    const uint8_t token[] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x11, 0x22};
    ASSERT_EQ(sentlog_set_device_attestation(
                  h, token, sizeof(token), "play_integrity"),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    const auto& subject = env["credentialSubject"];
    EXPECT_EQ(subject["evidence_grade"].get<std::string>(), "DeviceAttested");
    ASSERT_TRUE(subject.contains("device_attestation"));
    EXPECT_EQ(subject["device_attestation"]["type"].get<std::string>(),
              "play_integrity");
    // base64 of {0xCA,0xFE,0xBA,0xBE,0x00,0x11,0x22} → "yv66vgARIg=="
    EXPECT_EQ(subject["device_attestation"]["token_b64"].get<std::string>(),
              "yv66vgARIg==");
    sentlog_destroy(h);
}

TEST(DeviceAttestationTest, FullPayloadReportsSameGradeAsSubject) {
    const char* cfg = R"({
      "issuer":"did:example:x",
      "forwarders":[{"name":"a","projection":"Full",
                     "sink":{"kind":"callback"}}]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    const uint8_t token[] = {0x01, 0x02, 0x03};
    ASSERT_EQ(sentlog_set_device_attestation(
                  h, token, sizeof(token), "play_integrity"),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto subject = json::parse(dcap.envelopes[0])["credentialSubject"];
    EXPECT_EQ(subject["evidence_grade"].get<std::string>(), "DeviceAttested");
    EXPECT_EQ(subject["payload"]["evidence_grade"].get<std::string>(),
              "DeviceAttested");
    sentlog_destroy(h);
}

TEST(DeviceAttestationTest, NoAttestationLeavesGradeUnattested) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    EXPECT_EQ(env["credentialSubject"]["evidence_grade"].get<std::string>(),
              "DeviceUnattested");
    EXPECT_FALSE(env["credentialSubject"].contains("device_attestation"));
    sentlog_destroy(h);
}

TEST(DeviceAttestationTest, ClearingRevertsGrade) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    const uint8_t token[] = {0x01, 0x02, 0x03};
    ASSERT_EQ(sentlog_set_device_attestation(
                  h, token, sizeof(token), "app_attest"),
              SENTLOG_OK);
    // Clear by pushing null bytes.
    ASSERT_EQ(sentlog_set_device_attestation(h, nullptr, 0, nullptr),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    EXPECT_EQ(env["credentialSubject"]["evidence_grade"].get<std::string>(),
              "DeviceUnattested");
    EXPECT_FALSE(env["credentialSubject"].contains("device_attestation"));
    sentlog_destroy(h);
}

// Combined hw-signing + device-attestation: the envelope should
// carry the attestation block (independent of signing path) AND the
// evidence_grade should stay at "EnclaveSigned" — the attestation
// promotion path only fires when current grade is DeviceUnattested.
// This is the cross-feature integration the multi-phase audit
// flagged as missing.
TEST(DeviceAttestationTest, HwSignedKeepsEnclaveSignedDespiteAttestation) {
    const char* cfg = R"({
      "issuer":"did:example:x",
      "use_hardware_signing":true,
      "forwarders":[{"name":"a","projection":"ScoresOnly",
                     "sink":{"kind":"callback"}}]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    // Hardware signer (always-success stub).
    auto hw_cb = [](const uint8_t*, size_t,
                    uint8_t* sig_out, size_t* sig_len, void*) -> int {
        // DER ECDSA signature with r = s = 1.
        const uint8_t bytes[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01};
        if (*sig_len < sizeof(bytes)) return -1;
        std::memcpy(sig_out, bytes, sizeof(bytes));
        *sig_len = sizeof(bytes);
        return 0;
    };
    ASSERT_EQ(sentlog_set_hardware_signer(
                  h, hw_cb, nullptr,
                  "ecdsa-jcs-2019", "did:example:hw"),
              SENTLOG_OK);

    // Push attestation alongside.
    const uint8_t token[] = {0x01, 0x02, 0x03};
    ASSERT_EQ(sentlog_set_device_attestation(
                  h, token, sizeof(token), "play_integrity"),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    const auto& subject = env["credentialSubject"];
    // Both blocks present:
    EXPECT_EQ(subject["evidence_grade"].get<std::string>(), "EnclaveSigned");
    ASSERT_TRUE(subject.contains("device_attestation"));
    EXPECT_EQ(subject["device_attestation"]["type"].get<std::string>(),
              "play_integrity");
    // proof is ecdsa-jcs-2019 (hw), not eddsa (software).
    ASSERT_TRUE(env.contains("proof"));
    EXPECT_EQ(env["proof"]["cryptosuite"].get<std::string>(),
              "ecdsa-jcs-2019");
    sentlog_destroy(h);
}

TEST(DeviceAttestationTest, RejectsTokenWithoutType) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    const uint8_t token[] = {0xAB};
    EXPECT_EQ(sentlog_set_device_attestation(h, token, sizeof(token), nullptr),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(sentlog_set_device_attestation(h, token, sizeof(token), ""),
              SENTLOG_ERR_INVALID_CONFIG);
    sentlog_destroy(h);
}
