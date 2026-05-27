// Phase 7c-i: hardware-signer integration tests.
//
// Exercises the C ABI + State + signing-module wiring with a stub
// signer that returns deterministic bytes. The iOS Secure Enclave +
// Android KeyStore integrations are out of scope here (they land in
// 7c-ii / 7d) — what this test covers is that:
//
//   - the stub signer's bytes flow into the envelope's proof block
//     under cryptosuite="ecdsa-jcs-2019"
//   - the envelope's subject.evidence_grade is promoted to
//     "EnclaveSigned" on hw success
//   - a returning-zero signer triggers software fallback +
//     HardwareSigningUnavailable telemetry (when not required)
//   - hardware_signing_required=true converts the same failure to
//     a dropped envelope + ForwarderOutcome=dropped telemetry
//   - clearing the hw signer (cb=null) reverts to the software path

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

// One forwarder using sink_kind=callback so the envelope flows
// through the dispatcher we register. Issuer required because we have
// a forwarder.
const char* kBaseConfig = R"({
  "issuer": "did:example:integrator",
  "use_hardware_signing": true,
  "forwarders": [{
    "name": "test", "projection": "ScoresOnly",
    "sink": {"kind": "callback"}
  }]
})";

const char* kRequiredConfig = R"({
  "issuer": "did:example:integrator",
  "use_hardware_signing": true,
  "hardware_signing_required": true,
  "forwarders": [{
    "name": "test", "projection": "ScoresOnly",
    "sink": {"kind": "callback"}
  }]
})";

// Stub hardware signer — writes 64 bytes of 0x42 into the output
// buffer. Successful, deterministic, easy to assert against.
struct StubSigner {
    std::atomic<int> calls{0};
    bool             fail{false};
};

int stub_signer_cb(const uint8_t* /*msg*/, size_t /*msg_len*/,
                   uint8_t* sig_out, size_t* sig_len_inout,
                   void* userdata) {
    auto* s = static_cast<StubSigner*>(userdata);
    s->calls.fetch_add(1);
    if (s->fail) return -1;
    const size_t want = 64;
    if (*sig_len_inout < want) return -2;
    std::memset(sig_out, 0x42, want);
    *sig_len_inout = want;
    return 0;
}

struct DispatchCapture {
    std::mutex                m;
    std::vector<std::string>  envelopes;
};

void dispatcher_cb(const char* /*name*/, const char* /*kind*/,
                   const char* /*sink_json*/, const char* envelope_json,
                   void* userdata) {
    auto* c = static_cast<DispatchCapture*>(userdata);
    std::lock_guard<std::mutex> lock(c->m);
    c->envelopes.push_back(envelope_json ? envelope_json : "");
}

struct TelemetryCapture {
    std::mutex                m;
    std::vector<std::string>  events;
};

void telemetry_cb(const char* event_json, void* userdata) {
    auto* t = static_cast<TelemetryCapture*>(userdata);
    std::lock_guard<std::mutex> lock(t->m);
    t->events.push_back(event_json ? event_json : "");
}

// Helper: run the lifecycle that ends in dispatch_envelopes firing
// at least once. evaluate() with no audio still produces an envelope.
void run_one_evaluation(sentlog_handle_t* h) {
    sentlog_start_capture(h);
    sentlog_evaluate(h,
        [](sentlog_status_t, const char*, void*) {},
        nullptr);
}

bool contains_event_type(const TelemetryCapture& t, const std::string& type) {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(t.m));
    for (const auto& ev : t.events) {
        try {
            const auto j = json::parse(ev);
            if (j.value("type", "") == type) return true;
        } catch (...) {}
    }
    return false;
}

}  // namespace

TEST(HwSignerTest, RegisterAndClearAccepted) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kBaseConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    StubSigner s;
    EXPECT_EQ(sentlog_set_hardware_signer(
                  h, &stub_signer_cb, &s,
                  "ecdsa-jcs-2019", "did:example:hw"),
              SENTLOG_OK);
    EXPECT_EQ(sentlog_set_hardware_signer(
                  h, nullptr, nullptr, nullptr, nullptr),
              SENTLOG_OK);
    sentlog_destroy(h);
}

TEST(HwSignerTest, RejectsUnknownCryptosuite) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kBaseConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    StubSigner s;
    EXPECT_EQ(sentlog_set_hardware_signer(
                  h, &stub_signer_cb, &s, "rsa-pss-sha384", "did:x"),
              SENTLOG_ERR_INVALID_CONFIG);
    sentlog_destroy(h);
}

TEST(HwSignerTest, SuccessProducesEnclaveSignedEnvelope) {
    TelemetryCapture tcap;
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(
        kBaseConfig, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    StubSigner s;
    ASSERT_EQ(sentlog_set_hardware_signer(
                  h, &stub_signer_cb, &s,
                  "ecdsa-jcs-2019", "did:example:hw"),
              SENTLOG_OK);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    ASSERT_EQ(s.calls.load(), 1);
    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    EXPECT_EQ(env["proof"]["cryptosuite"].get<std::string>(),
              "ecdsa-jcs-2019");
    EXPECT_EQ(env["proof"]["verificationMethod"].get<std::string>(),
              "did:example:hw");
    EXPECT_TRUE(env["proof"].contains("proofValue"));
    EXPECT_EQ(env["credentialSubject"]["evidence_grade"].get<std::string>(),
              "EnclaveSigned");
    EXPECT_FALSE(contains_event_type(tcap, "HardwareSigningUnavailable"));
    sentlog_destroy(h);
}

TEST(HwSignerTest, FailureFallsBackToSoftwareWithTelemetry) {
    TelemetryCapture tcap;
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(
        kBaseConfig, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    StubSigner s;
    s.fail = true;
    ASSERT_EQ(sentlog_set_hardware_signer(
                  h, &stub_signer_cb, &s,
                  "ecdsa-jcs-2019", "did:example:hw"),
              SENTLOG_OK);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    // Software fallback: envelope is unsigned (no software key
    // registered) — proof block absent. evidence_grade reverts to
    // the result's original value (DeviceUnattested today).
    EXPECT_FALSE(env.contains("proof"));
    EXPECT_EQ(env["credentialSubject"]["evidence_grade"].get<std::string>(),
              "DeviceUnattested");
    EXPECT_TRUE(contains_event_type(tcap, "HardwareSigningUnavailable"));
    sentlog_destroy(h);
}

TEST(HwSignerTest, FailureWithRequiredDropsEnvelope) {
    TelemetryCapture tcap;
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(
        kRequiredConfig, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    StubSigner s;
    s.fail = true;
    ASSERT_EQ(sentlog_set_hardware_signer(
                  h, &stub_signer_cb, &s,
                  "ecdsa-jcs-2019", "did:example:hw"),
              SENTLOG_OK);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    // Envelope dispatched 0 times because hardware was required + failed.
    EXPECT_EQ(dcap.envelopes.size(), 0u);
    // A ForwarderOutcome=dropped telemetry event was emitted instead.
    bool saw_dropped = false;
    {
        std::lock_guard<std::mutex> lock(tcap.m);
        for (const auto& ev : tcap.events) {
            try {
                const auto j = json::parse(ev);
                if (j.value("type", "") == "ForwarderOutcome" &&
                    j.value("outcome", "") == "dropped") {
                    saw_dropped = true; break;
                }
            } catch (...) {}
        }
    }
    EXPECT_TRUE(saw_dropped);
    sentlog_destroy(h);
}

TEST(HwSignerTest, ClearingHwSignerRevertsToSoftwarePath) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(
        kBaseConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    StubSigner s;
    ASSERT_EQ(sentlog_set_hardware_signer(
                  h, &stub_signer_cb, &s,
                  "ecdsa-jcs-2019", "did:example:hw"),
              SENTLOG_OK);
    ASSERT_EQ(sentlog_set_hardware_signer(
                  h, nullptr, nullptr, nullptr, nullptr),
              SENTLOG_OK);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    // hw signer cleared → stub never called → envelope built via
    // the software path (unsigned since no software key registered).
    EXPECT_EQ(s.calls.load(), 0);
    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    EXPECT_EQ(env["credentialSubject"]["evidence_grade"].get<std::string>(),
              "DeviceUnattested");
    sentlog_destroy(h);
}
