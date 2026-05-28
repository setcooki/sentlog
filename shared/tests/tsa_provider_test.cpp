// Phase 4h: TSA-provider callback integration tests.
//
// The provider is opt-in (only engaged when tsa_endpoint is set in
// the config) and pluggable (the platform binding owns the HTTPS
// POST + the SHA-256). These tests cover the wiring:
//
//   - happy path: bytes flow through provider → envelope grows
//     a tsa_timestamp{provider, token_b64} sidecar (OUTSIDE proof)
//   - tsa_endpoint absent → provider never runs, no field added
//   - provider returns failure → TsaRequestFailed telemetry emitted
//     + (when tsa_required=false) envelope still dispatched
//   - tsa_required=true + failure → envelope dropped with
//     ForwarderOutcome=dropped telemetry
//   - provider absent + tsa_endpoint set → same failure path
//     (telemetry reason = no_tsa_provider_registered)

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

struct StubTsa {
    std::atomic<int> calls{0};
    bool             fail{false};
    std::string      last_url;
    std::mutex       m;
};

int stub_tsa_cb(const uint8_t* /*to_hash*/, size_t /*len*/,
                const char* url, int /*timeout*/,
                uint8_t* out, size_t* out_len, void* userdata) {
    auto* s = static_cast<StubTsa*>(userdata);
    s->calls.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(s->m);
        s->last_url = url ? url : "";
    }
    if (s->fail) return -1;
    // Pretend TimeStampToken bytes; just need stable + non-empty.
    const uint8_t token[] = {0x30, 0x82, 0x01, 0xFE};   // ASN.1 SEQ tag
    if (*out_len < sizeof(token)) return -2;
    std::memcpy(out, token, sizeof(token));
    *out_len = sizeof(token);
    return 0;
}

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

struct TelemetryCapture {
    std::mutex                m;
    std::vector<std::string>  events;
};

void telemetry_cb(const char* event_json, void* ud) {
    auto* t = static_cast<TelemetryCapture*>(ud);
    std::lock_guard<std::mutex> lock(t->m);
    t->events.push_back(event_json ? event_json : "");
}

bool has_event(TelemetryCapture& t, const std::string& type) {
    std::lock_guard<std::mutex> lock(t.m);
    for (const auto& ev : t.events) {
        try {
            const auto j = json::parse(ev);
            if (j.value("type", "") == type) return true;
        } catch (...) {}
    }
    return false;
}

// Configs ----------------------------------------------------------

const char* kCfgWithTsa = R"({
  "issuer":"did:example:x",
  "tsa_endpoint":"https://tsa.example.invalid/rfc3161",
  "tsa_timeout_ms":3000,
  "forwarders":[{"name":"a","projection":"ScoresOnly",
                 "sink":{"kind":"callback"}}]
})";

const char* kCfgWithTsaRequired = R"({
  "issuer":"did:example:x",
  "tsa_endpoint":"https://tsa.example.invalid/rfc3161",
  "tsa_required":true,
  "forwarders":[{"name":"a","projection":"ScoresOnly",
                 "sink":{"kind":"callback"}}]
})";

const char* kCfgNoTsa = R"({
  "issuer":"did:example:x",
  "forwarders":[{"name":"a","projection":"ScoresOnly",
                 "sink":{"kind":"callback"}}]
})";

// 64-byte RFC 8032 vector-1 secret key — exact same constant used
// in signing_test. Lets the envelope acquire a real proof block
// without re-deriving in each test.
std::vector<uint8_t> rfc8032_vector1_sk() {
    return {
        0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,
        0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,
        0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,
        0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60,
        0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a,
    };
}

void run_one(sentlog_handle_t* h) {
    sentlog_start_capture(h);
    sentlog_evaluate(h, [](sentlog_status_t, const char*, void*){}, nullptr);
}

}  // namespace

TEST(TsaProviderTest, TimestampAttachedOnHappyPath) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfgWithTsa, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    const auto sk = rfc8032_vector1_sk();
    ASSERT_EQ(sentlog_set_signing_key(h, sk.data(), sk.size(),
                                       "did:key:zTest"), SENTLOG_OK);

    StubTsa tsa;
    ASSERT_EQ(sentlog_set_tsa_provider(h, &stub_tsa_cb, &tsa), SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    EXPECT_EQ(tsa.calls.load(), 1);
    {
        std::lock_guard<std::mutex> lock(tsa.m);
        EXPECT_EQ(tsa.last_url, "https://tsa.example.invalid/rfc3161");
    }
    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    ASSERT_TRUE(env.contains("tsa_timestamp"));
    EXPECT_EQ(env["tsa_timestamp"]["provider"].get<std::string>(),
              "https://tsa.example.invalid/rfc3161");
    EXPECT_FALSE(env["tsa_timestamp"]["token_b64"].get<std::string>().empty());
    // tsa_timestamp lives OUTSIDE the proof — verifiers check the
    // signature first, then validate TSA against proof.proofValue.
    EXPECT_FALSE(env["proof"].contains("tsa_timestamp"));
    sentlog_destroy(h);
}

TEST(TsaProviderTest, NoTsaEndpointDoesNotInvokeProvider) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfgNoTsa, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    StubTsa tsa;
    ASSERT_EQ(sentlog_set_tsa_provider(h, &stub_tsa_cb, &tsa), SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    EXPECT_EQ(tsa.calls.load(), 0);
    ASSERT_EQ(dcap.envelopes.size(), 1u);
    EXPECT_FALSE(json::parse(dcap.envelopes[0]).contains("tsa_timestamp"));
    sentlog_destroy(h);
}

TEST(TsaProviderTest, FailureEmitsTelemetryButStillDispatches) {
    TelemetryCapture tcap;
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(kCfgWithTsa, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);
    const auto sk = rfc8032_vector1_sk();
    ASSERT_EQ(sentlog_set_signing_key(h, sk.data(), sk.size(),
                                       "did:key:zTest"), SENTLOG_OK);
    StubTsa tsa; tsa.fail = true;
    ASSERT_EQ(sentlog_set_tsa_provider(h, &stub_tsa_cb, &tsa), SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    EXPECT_EQ(dcap.envelopes.size(), 1u);   // envelope still ships
    EXPECT_FALSE(json::parse(dcap.envelopes[0]).contains("tsa_timestamp"));
    EXPECT_TRUE(has_event(tcap, "TsaRequestFailed"));
    sentlog_destroy(h);
}

TEST(TsaProviderTest, FailureWithRequiredDropsEnvelope) {
    TelemetryCapture tcap;
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(
        kCfgWithTsaRequired, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);
    const auto sk = rfc8032_vector1_sk();
    ASSERT_EQ(sentlog_set_signing_key(h, sk.data(), sk.size(),
                                       "did:key:zTest"), SENTLOG_OK);
    StubTsa tsa; tsa.fail = true;
    ASSERT_EQ(sentlog_set_tsa_provider(h, &stub_tsa_cb, &tsa), SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    EXPECT_EQ(dcap.envelopes.size(), 0u);
    EXPECT_TRUE(has_event(tcap, "TsaRequestFailed"));
    // The skip event for the forwarder also fires.
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(tcap.m);
        for (const auto& ev : tcap.events) {
            try {
                const auto j = json::parse(ev);
                if (j.value("type", "") == "ForwarderOutcome" &&
                    j.value("outcome", "") == "dropped") {
                    dropped = true; break;
                }
            } catch (...) {}
        }
    }
    EXPECT_TRUE(dropped);
    sentlog_destroy(h);
}

TEST(TsaProviderTest, NoProviderRegisteredEmitsTelemetry) {
    TelemetryCapture tcap;
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(
        kCfgWithTsa, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);
    const auto sk = rfc8032_vector1_sk();
    ASSERT_EQ(sentlog_set_signing_key(h, sk.data(), sk.size(),
                                       "did:key:zTest"), SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one(h);

    EXPECT_TRUE(has_event(tcap, "TsaRequestFailed"));
    EXPECT_EQ(dcap.envelopes.size(), 1u);   // not required, ships anyway
    sentlog_destroy(h);
}
