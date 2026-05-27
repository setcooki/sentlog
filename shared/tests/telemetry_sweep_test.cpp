// Phase 7f: on_telemetry sweep coverage.
//
// Exercises the events newly emitted by 7f alongside the ones already
// in place (ForwarderOutcome=dropped, HardwareSigningUnavailable —
// covered in their own test files). What we add here:
//
//   - PipelineTiming { stage: "evaluate", duration_ms } once per
//     successful evaluate
//   - ScoreHistogram { field, bucket } once per top-level score
//     (liveness.score, presence.summary, engagement.score), bucketed
//     to 0.1 width
//   - ForwarderOutcome { outcome: "ok", attempts: 1 } once per
//     successful dispatch (file write OR dispatcher call)
//   - CaptureAborted { reason } when cancel() interrupts capture;
//     silent when cancel() is a no-op on idle
//   - ManifestVerifyFailed { reason, attempted_version } when
//     check_for_updates rejects a manifest

#include "sentlog/sentlog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using nlohmann::json;

struct TelemetryCapture {
    std::mutex                m;
    std::vector<std::string>  events;
};

void telemetry_cb(const char* event_json, void* userdata) {
    auto* t = static_cast<TelemetryCapture*>(userdata);
    std::lock_guard<std::mutex> lock(t->m);
    t->events.push_back(event_json ? event_json : "");
}

void dispatcher_noop(const char*, const char*, const char*, const char*, void*) {}

std::vector<json> events_of_type(TelemetryCapture& t,
                                 const std::string& type) {
    std::vector<json> out;
    std::lock_guard<std::mutex> lock(t.m);
    for (const auto& ev : t.events) {
        try {
            const auto j = json::parse(ev);
            if (j.value("type", "") == type) out.push_back(j);
        } catch (...) {}
    }
    return out;
}

}  // namespace

TEST(TelemetrySweepTest, EvaluateEmitsPipelineTimingAndScoreHistograms) {
    TelemetryCapture tcap;
    const char* cfg = R"({"issuer":"did:example:x"})";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    sentlog_start_capture(h);
    sentlog_evaluate(h, [](sentlog_status_t, const char*, void*) {}, nullptr);

    EXPECT_EQ(events_of_type(tcap, "PipelineTiming").size(), 1u);

    const auto histos = events_of_type(tcap, "ScoreHistogram");
    // One per top-level score: liveness.score, presence.summary,
    // engagement.score. Pipeline always populates all three (even at
    // 0.0) so all three fire.
    EXPECT_EQ(histos.size(), 3u);
    for (const auto& h : histos) {
        const auto bucket = h["bucket"].get<std::string>();
        // Bucket is "0.0" through "1.0" — three chars, one decimal.
        EXPECT_EQ(bucket.size(), 3u);
        EXPECT_EQ(bucket[1], '.');
    }
    sentlog_destroy(h);
}

TEST(TelemetrySweepTest, ForwarderOkAfterSuccessfulDispatch) {
    TelemetryCapture tcap;
    const char* cfg = R"({
      "issuer":"did:example:x",
      "forwarders":[{"name":"hook","projection":"ScoresOnly",
                     "sink":{"kind":"callback"}}]
    })";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_noop, nullptr),
              SENTLOG_OK);

    sentlog_start_capture(h);
    sentlog_evaluate(h, [](sentlog_status_t, const char*, void*) {}, nullptr);

    const auto outcomes = events_of_type(tcap, "ForwarderOutcome");
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_EQ(outcomes[0]["outcome"].get<std::string>(), "ok");
    EXPECT_EQ(outcomes[0]["attempts"].get<int>(), 1);
    EXPECT_EQ(outcomes[0]["forwarder_name"].get<std::string>(), "hook");
    sentlog_destroy(h);
}

TEST(TelemetrySweepTest, CancelDuringCaptureEmitsAborted) {
    TelemetryCapture tcap;
    const char* cfg = R"({"issuer":"did:example:x"})";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    sentlog_start_capture(h);
    sentlog_cancel(h);

    const auto aborts = events_of_type(tcap, "CaptureAborted");
    ASSERT_EQ(aborts.size(), 1u);
    EXPECT_EQ(aborts[0]["reason"].get<std::string>(), "cancel_during_capture");
    sentlog_destroy(h);
}

TEST(TelemetrySweepTest, CancelOnIdleIsSilent) {
    TelemetryCapture tcap;
    const char* cfg = R"({"issuer":"did:example:x"})";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    sentlog_cancel(h);   // we're idle — no event
    sentlog_cancel(h);   // still idle — still no event

    EXPECT_TRUE(events_of_type(tcap, "CaptureAborted").empty());
    sentlog_destroy(h);
}

TEST(TelemetrySweepTest, ManifestVerifyFailedEventEmitted) {
    TelemetryCapture tcap;
    const char* cfg = R"({"issuer":"did:example:x"})";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, &telemetry_cb, &tcap, &status);
    ASSERT_NE(h, nullptr);

    // Well-formed manifest envelope but signature doesn't verify
    // against the placeholder pubkeys baked into the default build.
    const std::string envelope =
        R"({"manifest":{"schema_version":"1","manifest_version":"0.0.5",)"
        R"("bundle_url":"https://e.invalid/x","bundle_sha256":")"
        R"(e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",)"
        R"("bundle_size":1,"issued_at_ms":1},)"
        R"("key_id":"active","signature_b64url":")"
        R"(AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})";

    sentlog_check_for_updates(
        h,
        reinterpret_cast<const uint8_t*>(envelope.data()),
        envelope.size(),
        [](sentlog_status_t, const char*, void*) {},
        nullptr);

    const auto failures = events_of_type(tcap, "ManifestVerifyFailed");
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0]["reason"].get<std::string>(), "bad_signature");
    sentlog_destroy(h);
}

TEST(TelemetrySweepTest, NullTelemetryCallbackSafeAcrossAllPaths) {
    // Telemetry is opt-in via sentlog_create's cb arg; passing null
    // must never crash, even when events would otherwise have fired.
    const char* cfg = R"({"issuer":"did:example:x",
        "forwarders":[{"name":"x","projection":"ScoresOnly",
                       "sink":{"kind":"callback"}}]})";
    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* h = sentlog_create(cfg, nullptr, nullptr, &status);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(sentlog_set_envelope_dispatcher(h, &dispatcher_noop, nullptr),
              SENTLOG_OK);

    sentlog_start_capture(h);
    sentlog_evaluate(h, [](sentlog_status_t, const char*, void*) {}, nullptr);
    sentlog_cancel(h);
    sentlog_destroy(h);
    SUCCEED();
}
