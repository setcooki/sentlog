// Sentlog C ABI implementation.
//
// Thin marshalling layer between the C ABI declared in
// shared/include/sentlog/sentlog.h and the internal C++ State class.
// The C ABI never sees C++ exceptions — every entry point wraps its
// body in a try/catch and translates any failure to a status code.

#include "sentlog/sentlog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

// POSIX flock for queue serialisation. Sentlog targets POSIX (Linux,
// macOS, iOS, Android NDK); a Windows port would need LockFileEx.
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

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

sentlog_status_t sentlog_push_vad_frame(sentlog_handle_t* handle,
                                        size_t start_sample,
                                        size_t num_samples,
                                        float  probability) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (num_samples == 0)  return SENTLOG_OK;
    return handle->state->push_vad_frame(start_sample, num_samples, probability);
}

sentlog_status_t sentlog_push_liveness_result(sentlog_handle_t* handle,
                                              float score,
                                              float replay_risk,
                                              float synthetic_risk) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    return handle->state->push_liveness_result(score, replay_risk, synthetic_risk);
}

sentlog_status_t sentlog_push_transcript(sentlog_handle_t* handle,
                                         const char* transcript_json) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    return handle->state->push_transcript(transcript_json);
}

sentlog_status_t sentlog_push_motion_frame(sentlog_handle_t* handle,
                                           int64_t timestamp_ns,
                                           float ax, float ay, float az,
                                           float gx, float gy, float gz) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    return handle->state->push_motion_frame(
        timestamp_ns, ax, ay, az, gx, gy, gz);
}

sentlog_status_t sentlog_push_face_frame(sentlog_handle_t* handle,
                                         int64_t timestamp_ns,
                                         float valence,
                                         float arousal,
                                         const char* dominant_fau_csv) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    return handle->state->push_face_frame(
        timestamp_ns, valence, arousal, dominant_fau_csv);
}

sentlog_status_t sentlog_set_audio_route(sentlog_handle_t* handle,
                                         sentlog_audio_route_t route) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    handle->state->set_audio_route(route);
    return SENTLOG_OK;
}

sentlog_status_t sentlog_snapshot_audio(sentlog_handle_t* handle,
                                        float* out_samples,
                                        size_t out_capacity,
                                        size_t* out_written) {
    if (handle == nullptr || out_samples == nullptr || out_written == nullptr) {
        return SENTLOG_ERR_INVALID_STATE;
    }
    *out_written = handle->state->snapshot_audio(out_samples, out_capacity);
    return SENTLOG_OK;
}

sentlog_status_t sentlog_evaluate(sentlog_handle_t* handle,
                                  sentlog_result_cb_t cb,
                                  void* userdata) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (cb == nullptr)     return SENTLOG_ERR_INVALID_CONFIG;
    std::string result_json;
    sentlog_status_t status;
    try {
        status = handle->state->evaluate(&result_json);
    } catch (...) {
        status = SENTLOG_ERR_INTERNAL;
    }
    if (status == SENTLOG_OK) {
        cb(SENTLOG_OK, result_json.c_str(), userdata);
    } else {
        cb(status, nullptr, userdata);
    }
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

sentlog_status_t sentlog_set_signing_key(sentlog_handle_t* handle,
                                         const uint8_t* secret_key_bytes,
                                         size_t secret_key_len,
                                         const char* verification_method) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (verification_method == nullptr) return SENTLOG_ERR_INVALID_CONFIG;
    return handle->state->set_signing_key(
        secret_key_bytes, secret_key_len,
        std::string(verification_method));
}

sentlog_status_t sentlog_set_envelope_dispatcher(
    sentlog_handle_t* handle,
    sentlog_envelope_dispatch_cb_t cb,
    void* userdata) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    handle->state->set_envelope_dispatcher(cb, userdata);
    return SENTLOG_OK;
}

sentlog_status_t sentlog_set_tsa_provider(
    sentlog_handle_t*         handle,
    sentlog_tsa_provider_cb_t cb,
    void*                     userdata) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    handle->state->set_tsa_provider(cb, userdata);
    return SENTLOG_OK;
}

sentlog_status_t sentlog_set_device_attestation(
    sentlog_handle_t* handle,
    const uint8_t*    token_bytes,
    size_t            token_len,
    const char*       attestation_type) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (token_bytes != nullptr && token_len > 0 &&
        (attestation_type == nullptr || attestation_type[0] == '\0')) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    std::vector<uint8_t> tok;
    if (token_bytes != nullptr && token_len > 0) {
        tok.assign(token_bytes, token_bytes + token_len);
    }
    handle->state->set_device_attestation(
        tok, attestation_type ? std::string(attestation_type) : std::string());
    return SENTLOG_OK;
}

sentlog_status_t sentlog_set_audio_encoder(
    sentlog_handle_t*          handle,
    sentlog_audio_encoder_cb_t cb,
    void*                      userdata,
    const char*                codec_name) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (cb != nullptr && (codec_name == nullptr || codec_name[0] == '\0')) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    handle->state->set_audio_encoder(
        cb, userdata,
        codec_name ? std::string(codec_name) : std::string());
    return SENTLOG_OK;
}

sentlog_status_t sentlog_set_hardware_signer(
    sentlog_handle_t*    handle,
    sentlog_hw_sign_cb_t cb,
    void*                userdata,
    const char*          cryptosuite,
    const char*          verification_method) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (cb != nullptr &&
        (cryptosuite == nullptr || verification_method == nullptr)) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    return handle->state->set_hardware_signer(
        cb, userdata,
        cryptosuite ? std::string(cryptosuite) : std::string(),
        verification_method ? std::string(verification_method) : std::string());
}

sentlog_status_t sentlog_check_for_updates(sentlog_handle_t* handle,
                                           const uint8_t*    manifest_bytes,
                                           size_t            manifest_len,
                                           sentlog_update_cb_t cb,
                                           void* userdata) {
    if (handle == nullptr) return SENTLOG_ERR_INVALID_STATE;
    if (cb == nullptr)     return SENTLOG_ERR_INVALID_CONFIG;
    try {
        std::string update_json;
        const sentlog_status_t s = handle->state->check_for_updates(
            manifest_bytes, manifest_len, &update_json);
        cb(s, update_json.c_str(), userdata);
        return s;
    } catch (...) {
        return SENTLOG_ERR_INVALID_STATE;
    }
}

int sentlog_drain_queue(sentlog_handle_t* handle,
                        const char* path,
                        sentlog_drain_cb_t cb,
                        void* userdata) {
    (void)handle;  // handle is reserved for future telemetry; not needed today
    if (path == nullptr || cb == nullptr) return 0;

    // Phase 4i: read the NDJSON queue file (written by 4d2's
    // LocalQueueSink) line-by-line. Each line is one envelope. The
    // cb returns 1 to delete the envelope from the queue, 0 to
    // keep it for a later drain.
    //
    // Race avoidance (Phase 4 audit A1): an evaluate() running
    // concurrently in another thread / process appends to the same
    // file. Without locking, an append between our read and our
    // tmp+rename would be silently discarded by the rename. We
    // serialise via a sidecar lock file (`<path>.lock`) whose
    // identity is stable across the rename — locking the data
    // file itself doesn't work, because its inode changes mid-
    // drain and the writer's pre-acquired fd ends up pointing at
    // the unlinked old version. LocalQueueSink's append takes the
    // same sidecar lock. POSIX-only; a Windows port would use
    // LockFileEx on a sidecar.
    const std::string lock_path = std::string(path) + ".lock";
    const int lockfd = ::open(lock_path.c_str(),
                              O_RDWR | O_CREAT, 0600);
    if (lockfd < 0) return 0;
    if (::flock(lockfd, LOCK_EX) != 0) { ::close(lockfd); return 0; }

    int delivered = 0;
    std::vector<std::string> kept;
    {
        std::ifstream in(path);
        if (!in.is_open()) {
            ::flock(lockfd, LOCK_UN);
            ::close(lockfd);
            return 0;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            ++delivered;
            const int keep = cb(line.c_str(), line.size(), userdata) == 1
                ? 0   // cb returned 1 → delete
                : 1;  // cb returned 0 (or anything else) → keep
            if (keep) kept.push_back(line);
        }
    }

    // Rewrite if we removed any entries.
    if (kept.size() != static_cast<size_t>(delivered)) {
        const std::string tmp = std::string(path) + ".drain.tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                ::flock(lockfd, LOCK_UN);
                ::close(lockfd);
                return delivered;
            }
            for (const auto& k : kept) out << k << "\n";
        }
        // std::rename atomically replaces on POSIX (Linux, macOS,
        // iOS, Android NDK). On Windows it would fail when the
        // target exists; we don't ship Windows.
        std::rename(tmp.c_str(), path);
    }

    ::flock(lockfd, LOCK_UN);
    ::close(lockfd);
    return delivered;
}

void sentlog_string_free(char* str) {
    std::free(str);
}

}  // extern "C"
