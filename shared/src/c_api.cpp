// Sentlog C ABI implementation.
//
// Thin marshalling layer between the C ABI declared in
// shared/include/sentlog/sentlog.h and the internal C++ State class.
// The C ABI never sees C++ exceptions — every entry point wraps its
// body in a try/catch and translates any failure to a status code.

#include "sentlog/sentlog.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "internal/config.h"
#include "internal/state.h"

// The opaque sentlog_handle_t the C ABI exposes wraps an internal
// State (forward-declared as an incomplete struct in the public
// header). State holds atomic members and is therefore neither
// copyable nor movable; unique_ptr lets us construct it in place
// without dancing around C++17 copy-elision rules.
struct sentlog_handle {
    std::unique_ptr<sentlog::internal::State> state;
};

namespace {

// Allocate a heap-owned C string the caller will free via
// sentlog_string_free.
char* dup_cstring(const std::string& s) noexcept {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) return nullptr;
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

}  // namespace

extern "C" {

const char* sentlog_version(void) {
    return SENTLOG_VERSION_STRING;
}

sentlog_handle_t* sentlog_create(
    const char* config_json,
    sentlog_telemetry_cb_t telemetry_cb,
    void* telemetry_userdata,
    sentlog_status_t* status_out) {

    if (status_out == nullptr) {
        return nullptr;
    }
    if (config_json == nullptr) {
        *status_out = SENTLOG_ERR_INVALID_CONFIG;
        return nullptr;
    }

    try {
        sentlog::internal::Config cfg;
        std::string error_field;
        const sentlog_status_t s =
            sentlog::internal::parse_config(config_json, &cfg, &error_field);
        if (s != SENTLOG_OK) {
            *status_out = s;
            return nullptr;
        }

        auto* h = new sentlog_handle{};
        h->state = std::make_unique<sentlog::internal::State>(
            std::move(cfg), telemetry_cb, telemetry_userdata);
        *status_out = SENTLOG_OK;
        return h;
    } catch (const std::bad_alloc&) {
        *status_out = SENTLOG_ERR_INTERNAL;
        return nullptr;
    } catch (...) {
        *status_out = SENTLOG_ERR_INTERNAL;
        return nullptr;
    }
}

void sentlog_destroy(sentlog_handle_t* handle) {
    delete handle;
}

sentlog_status_t sentlog_start_capture(sentlog_handle_t* handle) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    try {
        return handle->state->start_capture();
    } catch (...) {
        return SENTLOG_ERR_INTERNAL;
    }
}

sentlog_status_t sentlog_push_audio_frame(sentlog_handle_t* handle,
                                          const float* samples,
                                          size_t num_samples) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (samples == nullptr || num_samples == 0) return SENTLOG_OK;
    // Real-time path: no exceptions, no logging here.
    return handle->state->push_audio(samples, num_samples);
}

sentlog_status_t sentlog_evaluate(sentlog_handle_t* handle,
                                  sentlog_result_cb_t cb,
                                  void* userdata) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (cb == nullptr)     return SENTLOG_ERR_INVALID_CONFIG;
    // Phase 1: the evaluation pipeline (VAD → ASR → liveness →
    // scoring) lands in subsequent chunks. Until then, evaluate()
    // returns NOT_IMPLEMENTED via the callback so bindings can
    // exercise the wiring end to end.
    (void)handle;
    cb(SENTLOG_ERR_INTERNAL, nullptr, userdata);
    return SENTLOG_OK;
}

void sentlog_cancel(sentlog_handle_t* handle) {
    if (handle == nullptr) return;
    (void)handle->state->cancel();
}

char* sentlog_get_permissions(sentlog_handle_t* handle) {
    if (handle == nullptr) return nullptr;
    try {
        return dup_cstring(handle->state->permissions_json());
    } catch (...) {
        return nullptr;
    }
}

char* sentlog_get_capabilities(sentlog_handle_t* handle) {
    if (handle == nullptr) return nullptr;
    try {
        return dup_cstring(handle->state->capabilities_json());
    } catch (...) {
        return nullptr;
    }
}

char* sentlog_model_versions(sentlog_handle_t* handle) {
    if (handle == nullptr) return nullptr;
    try {
        return dup_cstring(handle->state->model_versions_json());
    } catch (...) {
        return nullptr;
    }
}

sentlog_status_t sentlog_check_for_updates(sentlog_handle_t* handle,
                                           sentlog_update_cb_t cb,
                                           void* userdata) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (cb == nullptr)     return SENTLOG_ERR_INVALID_CONFIG;
    // Phase 7. Stub returns the schema-correct "no update found".
    const std::string update_json =
        R"({"checked_at_ms":0,"update_found":false,)"
        R"("previous_version":"0.0.0-phase1","applied":false})";
    cb(SENTLOG_OK, update_json.c_str(), userdata);
    return SENTLOG_OK;
}

int sentlog_drain_queue(sentlog_handle_t* handle,
                        const char* path,
                        sentlog_drain_cb_t cb,
                        void* userdata) {
    (void)handle;
    (void)path;
    (void)cb;
    (void)userdata;
    // Phase 4. Stub returns "drained zero envelopes."
    return 0;
}

void sentlog_string_free(char* str) {
    std::free(str);
}

}  // extern "C"
