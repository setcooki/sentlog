// Black-box tests that exercise the C ABI exactly as a platform
// binding would. Validates lifecycle, error returns, JSON shapes, and
// the strings-the-caller-must-free contract.

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstring>
#include <string>

namespace {

using nlohmann::json;

const char* kValidConfig =
    R"({"issuer": "did:example:integrator", "language_hint": "en"})";

struct ResultCapture {
    bool             called{false};
    sentlog_status_t status{SENTLOG_OK};
    std::string      result_json;
};

void result_cb(sentlog_status_t status,
               const char* result_json,
               void* userdata) {
    auto* cap = static_cast<ResultCapture*>(userdata);
    cap->called = true;
    cap->status = status;
    cap->result_json = result_json ? result_json : "";
}

}  // namespace

TEST(CApiTest, VersionReturnsNonEmpty) {
    const char* v = sentlog_version();
    ASSERT_NE(v, nullptr);
    EXPECT_GT(std::strlen(v), 0u);
}

TEST(CApiTest, CreateAndDestroy) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    EXPECT_EQ(status, SENTLOG_OK);
    ASSERT_NE(h, nullptr);
    sentlog_destroy(h);
}

TEST(CApiTest, DestroyNullIsSafe) {
    sentlog_destroy(nullptr);  // must not crash
    SUCCEED();
}

TEST(CApiTest, CreateRejectsMalformedConfig) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create("not json", nullptr, nullptr, &status);
    EXPECT_EQ(status, SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(h, nullptr);
}

TEST(CApiTest, CreateRejectsNullConfig) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(nullptr, nullptr, nullptr, &status);
    EXPECT_EQ(status, SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(h, nullptr);
}

TEST(CApiTest, GetCapabilitiesReturnsValidJson) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    char* caps = sentlog_get_capabilities(h);
    ASSERT_NE(caps, nullptr);
    const json j = json::parse(caps);
    EXPECT_TRUE(j.contains("audio_capture"));
    EXPECT_TRUE(j["audio_capture"].is_boolean());
    EXPECT_TRUE(j.contains("runtime"));
    EXPECT_TRUE(j["runtime"].is_string());
    sentlog_string_free(caps);
    sentlog_destroy(h);
}

TEST(CApiTest, GetPermissionsReturnsValidJson) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    char* perms = sentlog_get_permissions(h);
    ASSERT_NE(perms, nullptr);
    const json j = json::parse(perms);
    ASSERT_TRUE(j.contains("microphone"));
    EXPECT_TRUE(j["microphone"].is_string());
    sentlog_string_free(perms);
    sentlog_destroy(h);
}

TEST(CApiTest, ModelVersionsReturnsValidJson) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    char* mv = sentlog_model_versions(h);
    ASSERT_NE(mv, nullptr);
    const json j = json::parse(mv);
    EXPECT_TRUE(j.contains("liveness"));
    EXPECT_TRUE(j.contains("asr"));
    EXPECT_TRUE(j.contains("prosody"));
    EXPECT_TRUE(j.contains("manifest"));
    sentlog_string_free(mv);
    sentlog_destroy(h);
}

TEST(CApiTest, StartCaptureAndPushAudio) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_start_capture(h), SENTLOG_OK);

    std::array<float, 320> frame{};  // 20 ms at 16 kHz
    for (size_t i = 0; i < frame.size(); ++i) {
        frame[i] = 0.1f * static_cast<float>(i);
    }
    EXPECT_EQ(sentlog_push_audio_frame(h, frame.data(), frame.size()),
              SENTLOG_OK);

    sentlog_cancel(h);
    sentlog_destroy(h);
}

TEST(CApiTest, PushAudioBeforeStartIsLifecycleError) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    std::array<float, 16> frame{};
    EXPECT_EQ(sentlog_push_audio_frame(h, frame.data(), frame.size()),
              SENTLOG_ERR_INVALID_STATE);
    sentlog_destroy(h);
}

TEST(CApiTest, DoubleStartCaptureIsLifecycleError) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_start_capture(h), SENTLOG_OK);
    EXPECT_EQ(sentlog_start_capture(h), SENTLOG_ERR_INVALID_STATE);
    sentlog_destroy(h);
}

TEST(CApiTest, EvaluateCallbackFiresWithRealResult) {
    // With the Phase 1 pipeline in place, evaluate() on an empty
    // capture fires the callback with OK and a result whose
    // diagnostics.failure_reason is "TooShort" (default min_duration_ms
    // is 5000; the drained ring contained 0ms of audio). End-to-end
    // wiring is what's being verified here; the pipeline-content
    // tests live in pipeline_test.cpp.
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    ResultCapture cap;
    sentlog_start_capture(h);
    EXPECT_EQ(sentlog_evaluate(h, result_cb, &cap), SENTLOG_OK);
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.status, SENTLOG_OK);
    EXPECT_FALSE(cap.result_json.empty());
    sentlog_destroy(h);
}

TEST(CApiTest, CheckForUpdatesReturnsSchemaCorrectStub) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    struct UpdCapture {
        bool called{false};
        std::string update_json;
    } cap;

    auto cb = [](sentlog_status_t s, const char* j, void* ud) {
        auto* c = static_cast<UpdCapture*>(ud);
        c->called = true;
        if (j) c->update_json = j;
        EXPECT_EQ(s, SENTLOG_OK);
    };

    EXPECT_EQ(sentlog_check_for_updates(h, cb, &cap), SENTLOG_OK);
    EXPECT_TRUE(cap.called);
    const auto j = json::parse(cap.update_json);
    EXPECT_TRUE(j.contains("update_found"));
    EXPECT_FALSE(j["update_found"].get<bool>());
    sentlog_destroy(h);
}

TEST(CApiTest, StringFreeNullIsSafe) {
    sentlog_string_free(nullptr);
    SUCCEED();
}
