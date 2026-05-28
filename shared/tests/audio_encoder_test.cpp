// Phase 4g: audio-encoder callback integration tests.
//
// Validates the wiring from sentlog_set_audio_encoder through the
// dispatch loop to the envelope's audio_clip block. A stub encoder
// returns deterministic bytes; we assert those bytes show up
// base64-encoded in the envelope, that the registered codec name
// flows through, that the encoder runs at most ONCE per evaluate
// (regardless of how many AudioClip-bearing forwarders are
// configured), and that the placeholder still ships when no
// encoder is registered.

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

struct StubEncoder {
    std::atomic<int> calls{0};
};

int stub_encoder_cb(const float* /*pcm*/, size_t /*n*/,
                    int /*sr*/, uint8_t* out, size_t* out_len,
                    void* userdata) {
    auto* e = static_cast<StubEncoder*>(userdata);
    e->calls.fetch_add(1);
    // Write a stable 4-byte payload — easy to spot in the
    // base64-encoded envelope field.
    const uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    if (*out_len < sizeof(bytes)) return -1;
    std::memcpy(out, bytes, sizeof(bytes));
    *out_len = sizeof(bytes);
    return 0;
}

struct DispatchCapture {
    std::mutex                m;
    std::vector<std::string>  envelopes;
};

void dispatcher_cb(const char* /*n*/, const char* /*k*/,
                   const char* /*sj*/, const char* envelope_json,
                   void* userdata) {
    auto* c = static_cast<DispatchCapture*>(userdata);
    std::lock_guard<std::mutex> lock(c->m);
    c->envelopes.push_back(envelope_json ? envelope_json : "");
}

// "deadbeef" → standard base64 → "3q2+7w==" (8 chars).
constexpr const char* kExpectedB64 = "3q2+7w==";

void run_one_evaluation(sentlog_handle_t* h) {
    sentlog_start_capture(h);
    // Push 256 samples so the ring buffer is non-empty when
    // dispatch_envelopes snapshots it. Values don't matter; the stub
    // encoder ignores the PCM and returns the fixed payload.
    std::vector<float> pcm(256, 0.0f);
    sentlog_push_audio_frame(h, pcm.data(), pcm.size());
    sentlog_evaluate(h, [](sentlog_status_t, const char*, void*) {},
                     nullptr);
}

}  // namespace

TEST(AudioEncoderTest, EncoderBytesAppearInEnvelopeForEvidenceClip) {
    const char* cfg = R"({
      "issuer":"did:example:x",
      "forwarders":[{"name":"a","projection":"EvidenceClip",
                     "sink":{"kind":"callback"}}]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    StubEncoder enc;
    ASSERT_EQ(sentlog_set_audio_encoder(h, &stub_encoder_cb, &enc, "opus"),
              SENTLOG_OK);

    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    const auto& payload = env["credentialSubject"]["payload"];
    ASSERT_TRUE(payload.contains("audio_clip"));
    EXPECT_EQ(payload["audio_clip"]["codec"].get<std::string>(), "opus");
    EXPECT_EQ(payload["audio_clip"]["bytes"].get<std::string>(), kExpectedB64);
    EXPECT_FALSE(payload["audio_clip"].contains("note"));
    EXPECT_EQ(enc.calls.load(), 1);
    sentlog_destroy(h);
}

TEST(AudioEncoderTest, NoEncoderRegisteredFallsBackToPlaceholder) {
    const char* cfg = R"({
      "issuer":"did:example:x",
      "forwarders":[{"name":"a","projection":"EvidenceClip",
                     "sink":{"kind":"callback"}}]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    ASSERT_EQ(dcap.envelopes.size(), 1u);
    const auto env = json::parse(dcap.envelopes[0]);
    const auto& clip = env["credentialSubject"]["payload"]["audio_clip"];
    EXPECT_EQ(clip["codec"].get<std::string>(), "opus");
    EXPECT_EQ(clip["bytes"].get<std::string>(), "");
    EXPECT_TRUE(clip.contains("note"));  // placeholder ships
    sentlog_destroy(h);
}

TEST(AudioEncoderTest, EncoderRunsOncePerEvaluateAcrossForwarders) {
    // Two AudioClip-bearing forwarders. The encoder should run ONCE
    // (encode is shared across all forwarders in the same dispatch),
    // not twice.
    const char* cfg = R"({
      "issuer":"did:example:x",
      "forwarders":[
        {"name":"a","projection":"EvidenceClip",
         "sink":{"kind":"callback"}},
        {"name":"b","projection":"Full+AudioClip",
         "sink":{"kind":"callback"}}
      ]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    StubEncoder enc;
    ASSERT_EQ(sentlog_set_audio_encoder(h, &stub_encoder_cb, &enc, "opus"),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    EXPECT_EQ(dcap.envelopes.size(), 2u);
    EXPECT_EQ(enc.calls.load(), 1);
    for (const auto& env_json : dcap.envelopes) {
        const auto env = json::parse(env_json);
        const auto& clip = env["credentialSubject"]["payload"]["audio_clip"];
        EXPECT_EQ(clip["bytes"].get<std::string>(), kExpectedB64);
    }
    sentlog_destroy(h);
}

TEST(AudioEncoderTest, NonClipProjectionsDoNotInvokeEncoder) {
    // Forwarder projection doesn't ask for a clip; encoder should
    // never run.
    const char* cfg = R"({
      "issuer":"did:example:x",
      "forwarders":[{"name":"x","projection":"ScoresOnly",
                     "sink":{"kind":"callback"}}]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    StubEncoder enc;
    ASSERT_EQ(sentlog_set_audio_encoder(h, &stub_encoder_cb, &enc, "opus"),
              SENTLOG_OK);
    DispatchCapture dcap;
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_cb, &dcap),
              SENTLOG_OK);

    run_one_evaluation(h);

    EXPECT_EQ(enc.calls.load(), 0);
    sentlog_destroy(h);
}

TEST(AudioEncoderTest, RejectsRegisterWithEmptyCodecName) {
    const char* cfg = R"({"issuer":"did:example:x"})";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    StubEncoder enc;
    EXPECT_EQ(sentlog_set_audio_encoder(h, &stub_encoder_cb, &enc, ""),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(sentlog_set_audio_encoder(h, &stub_encoder_cb, &enc, nullptr),
              SENTLOG_ERR_INVALID_CONFIG);
    // Clearing (cb=null) accepts any codec_name including null.
    EXPECT_EQ(sentlog_set_audio_encoder(h, nullptr, nullptr, nullptr),
              SENTLOG_OK);
    sentlog_destroy(h);
}
