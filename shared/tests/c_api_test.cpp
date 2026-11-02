// Black-box tests that exercise the C ABI exactly as a platform
// binding would. Validates lifecycle, error returns, JSON shapes, and
// the strings-the-caller-must-free contract.

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// POSIX flock for the concurrent-drain race test.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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

// The full manifest-verify pipeline is exercised in ota_test.cpp
// against the raw primitive; here we cover the C ABI plumbing:
// null/empty-bytes rejection, error-path delivery (fails closed
// against the placeholder pubkeys built into the default build), and
// the JSON shape.
TEST(CApiTest, CheckForUpdatesRejectsEmptyBytes) {
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    auto cb = [](sentlog_status_t, const char*, void*) {};
    EXPECT_EQ(sentlog_check_for_updates(h, nullptr, 0, cb, nullptr),
              SENTLOG_ERR_INVALID_CONFIG);
    sentlog_destroy(h);
}

TEST(CApiTest, CheckForUpdatesFailsClosedAgainstPlaceholderKeys) {
    // The default-built SDK has placeholder (all-zero) Sentlog
    // pubkeys until v1.0 publishes the real ones. Any well-formed
    // manifest must therefore fail with bad_signature — never
    // silently succeed.
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h =
        sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    const std::string envelope =
        R"({"manifest":{"schema_version":"1","manifest_version":"0.0.5",)"
        R"("bundle_url":"https://e.invalid/x","bundle_sha256":")"
        R"(e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",)"
        R"("bundle_size":1,"issued_at_ms":1},)"
        R"("key_id":"active","signature_b64url":")"
        R"(AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})";

    struct UpdCapture {
        bool called{false};
        sentlog_status_t status{SENTLOG_OK};
        std::string update_json;
    } cap;
    auto cb = [](sentlog_status_t s, const char* j, void* ud) {
        auto* c = static_cast<UpdCapture*>(ud);
        c->called = true;
        c->status = s;
        if (j) c->update_json = j;
    };

    EXPECT_EQ(sentlog_check_for_updates(
                  h,
                  reinterpret_cast<const uint8_t*>(envelope.data()),
                  envelope.size(),
                  cb, &cap),
              SENTLOG_ERR_MANIFEST_VERIFY);
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.status, SENTLOG_ERR_MANIFEST_VERIFY);
    const auto j = json::parse(cap.update_json);
    EXPECT_FALSE(j["update_found"].get<bool>());
    EXPECT_FALSE(j["applied"].get<bool>());
    EXPECT_EQ(j["error"].get<std::string>(), "bad_signature");
    sentlog_destroy(h);
}

TEST(CApiTest, StringFreeNullIsSafe) {
    sentlog_string_free(nullptr);
    SUCCEED();
}

// --- Error-path coverage (pre-Phase-3 improvement pass) ---
//
// Every status code in sentlog_status_t should have at least one test
// that produces it. The phases above ship the OK paths; these tests
// pin the error paths down so future refactors don't quietly turn an
// error into success.

TEST(CApiTest, EvaluateWithoutStartCaptureFailsLifecycle) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    ResultCapture cap;
    // No sentlog_start_capture first.
    EXPECT_EQ(sentlog_evaluate(h, &result_cb, &cap), SENTLOG_OK)
        << "sentlog_evaluate always returns OK and fires cb with the error";
    EXPECT_TRUE(cap.called);
    EXPECT_EQ(cap.status, SENTLOG_ERR_INVALID_STATE);

    sentlog_destroy(h);
}

TEST(CApiTest, EvaluateWithNullCallbackIsInvalidConfig) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(sentlog_evaluate(h, nullptr, nullptr),
              SENTLOG_ERR_INVALID_CONFIG);

    sentlog_destroy(h);
}

TEST(CApiTest, PushAudioFrameOutsideCaptureFailsLifecycle) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    std::array<float, 4> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    EXPECT_EQ(sentlog_push_audio_frame(h, samples.data(), samples.size()),
              SENTLOG_ERR_INVALID_STATE);

    sentlog_destroy(h);
}

TEST(CApiTest, SnapshotAudioWithNullArgsFailsInvalidState) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);

    std::array<float, 4> buf{};
    size_t written = 0;
    EXPECT_EQ(sentlog_snapshot_audio(nullptr, buf.data(), buf.size(), &written),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_snapshot_audio(h, nullptr, 0, &written),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_snapshot_audio(h, buf.data(), buf.size(), nullptr),
              SENTLOG_ERR_INVALID_STATE);

    sentlog_destroy(h);
}

TEST(CApiTest, PushTranscriptMalformedJsonReturnsInvalidConfig) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create(kValidConfig, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_start_capture(h), SENTLOG_OK);

    EXPECT_EQ(sentlog_push_transcript(h, "{this is not json"),
              SENTLOG_ERR_INVALID_CONFIG);
    EXPECT_EQ(sentlog_push_transcript(h, R"({"text":"x"})"),
              SENTLOG_ERR_INVALID_CONFIG)
        << "missing required language/confidence";
    EXPECT_EQ(sentlog_push_transcript(h, nullptr),
              SENTLOG_ERR_INVALID_CONFIG);

    sentlog_destroy(h);
}

TEST(CApiTest, CreateInvalidJsonReturnsInvalidConfig) {
    sentlog_status_t status = SENTLOG_OK;
    auto* h = sentlog_create("{not valid json", nullptr, nullptr, &status);
    EXPECT_EQ(h, nullptr);
    EXPECT_EQ(status, SENTLOG_ERR_INVALID_CONFIG);
}

// --- Phase 4i: drainQueue ---

namespace {
struct DrainCapture {
    std::vector<std::string> seen;
    bool delete_after_seen = true;
};
int on_drain(const char* envelope_json, size_t /*len*/, void* userdata) {
    auto* c = static_cast<DrainCapture*>(userdata);
    c->seen.emplace_back(envelope_json ? envelope_json : "");
    return c->delete_after_seen ? 1 : 0;
}

std::string tmp_queue_path(const char* suffix) {
    const char* dir = std::getenv("TMPDIR");
    return std::string(dir ? dir : "/tmp") + "/sentlog-drain-" + suffix + ".ndjson";
}

void write_lines(const std::string& path,
                 const std::vector<std::string>& lines) {
    std::remove(path.c_str());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}
}  // namespace

TEST(CApiTest, DrainQueueDeliversAllLinesAndDeletesWhenCbReturnsOne) {
    const auto p = tmp_queue_path("delete-all");
    write_lines(p, {"{\"a\":1}", "{\"b\":2}", "{\"c\":3}"});
    DrainCapture dc;
    const int n = sentlog_drain_queue(nullptr, p.c_str(), &on_drain, &dc);
    EXPECT_EQ(n, 3);
    ASSERT_EQ(dc.seen.size(), 3u);
    EXPECT_EQ(dc.seen[0], "{\"a\":1}");
    EXPECT_EQ(dc.seen[2], "{\"c\":3}");
    // File should now be empty (all deleted).
    EXPECT_TRUE(read_lines(p).empty());
    std::remove(p.c_str());
}

TEST(CApiTest, DrainQueueKeepsLinesWhenCbReturnsZero) {
    const auto p = tmp_queue_path("keep-all");
    write_lines(p, {"{\"x\":1}", "{\"y\":2}"});
    DrainCapture dc;
    dc.delete_after_seen = false;
    const int n = sentlog_drain_queue(nullptr, p.c_str(), &on_drain, &dc);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(dc.seen.size(), 2u);
    // File preserved.
    EXPECT_EQ(read_lines(p).size(), 2u);
    std::remove(p.c_str());
}

TEST(CApiTest, DrainQueueMissingFileReturnsZero) {
    EXPECT_EQ(sentlog_drain_queue(nullptr,
                                  "/tmp/sentlog-drain-does-not-exist.ndjson",
                                  &on_drain, nullptr),
              0);
}

TEST(CApiTest, DrainQueueNullArgsReturnZero) {
    EXPECT_EQ(sentlog_drain_queue(nullptr, nullptr, &on_drain, nullptr), 0);
    EXPECT_EQ(sentlog_drain_queue(nullptr, "/tmp/whatever", nullptr, nullptr),
              0);
}

TEST(CApiTest, NullHandleOperationsAreSafe) {
    // The C ABI guarantees null-handle calls don't crash; they return
    // INVALID_STATE for failable ops and silently no-op for the
    // void-returning ones.
    EXPECT_EQ(sentlog_start_capture(nullptr), SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_push_audio_frame(nullptr, nullptr, 0),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_push_vad_frame(nullptr, 0, 0, 0.5f),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_push_liveness_result(nullptr, 0.5f, 0.0f, 0.0f),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_push_transcript(nullptr, "{}"),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_set_audio_route(nullptr, SENTLOG_AUDIO_ROUTE_BUILT_IN),
              SENTLOG_ERR_INVALID_STATE);
    EXPECT_EQ(sentlog_get_permissions(nullptr), nullptr);
    EXPECT_EQ(sentlog_get_capabilities(nullptr), nullptr);
    EXPECT_EQ(sentlog_model_versions(nullptr), nullptr);
    sentlog_cancel(nullptr);   // void; just must not crash
    sentlog_destroy(nullptr);  // void; just must not crash
    SUCCEED();
}
