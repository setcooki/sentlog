// JNI bridge between the Kotlin Sentlog wrapper and the Sentlog C ABI.
//
// Naming convention follows JNI's mangled-name rule:
//   Java_<package_with_underscores>_<class>_<method>
// Our internal Kotlin class is me.cooki.sentlog.internal.NativeBridge,
// which mangles to Java_me_cooki_sentlog_internal_NativeBridge_*.
//
// Responsibilities:
//   - Translate jlong handles to/from sentlog_handle_t*.
//   - Marshal jstring config/json across the boundary as UTF-8 C
//     strings, releasing them with ReleaseStringUTFChars.
//   - Bridge the C ABI's callback-based async into JNI by holding a
//     global reference to a Kotlin callback object and invoking it
//     from the SDK's I/O thread (with JNI attach/detach).
//   - Never throw or leak. The C ABI's status codes are returned as
//     jint; failures in JNI itself raise a Java exception.

#include "sentlog/sentlog.h"

#include <jni.h>
#include <android/log.h>

#include <cstring>
#include <memory>

#define SENTLOG_LOG_TAG "sentlog-jni"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, SENTLOG_LOG_TAG, __VA_ARGS__)

namespace {

// Cached JavaVM* so callbacks fired from the SDK's I/O thread can
// attach themselves to the JVM and dispatch into Kotlin.
JavaVM* g_vm = nullptr;

// A piece of state held on the native side per async operation
// (evaluate, checkForUpdates, drainQueue) — bundles the global
// reference to the Kotlin callback so the JNI can invoke it from any
// thread.
struct GlobalCallback {
    jobject ref;     // global ref to a Kotlin callback object
    jmethodID method; // method to invoke
};

// JNI-side wrapper around the C ABI handle. Bundles ownership of any
// JNI globals we allocated alongside the C handle so they can be
// released atomically on destroy.
struct JniHandle {
    sentlog_handle_t* native;       // owned
    GlobalCallback*   telemetry;    // owned, may be null
    GlobalCallback*   dispatcher;   // owned, may be null
};

// Attach the current thread to the JVM if needed and run the supplied
// lambda with a JNIEnv*. Detaches on return if it had to attach.
template <typename F>
void with_jni_env(F&& fn) {
    if (g_vm == nullptr) return;

    JNIEnv* env = nullptr;
    bool need_detach = false;
    const int get_env_status =
        g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (get_env_status == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed in callback");
            return;
        }
        need_detach = true;
    } else if (get_env_status != JNI_OK) {
        LOGE("GetEnv failed in callback");
        return;
    }
    fn(env);
    if (need_detach) {
        g_vm->DetachCurrentThread();
    }
}

inline JniHandle* jni_from_jlong(jlong h) {
    return reinterpret_cast<JniHandle*>(static_cast<uintptr_t>(h));
}
inline jlong jlong_from_jni(JniHandle* h) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(h));
}
inline sentlog_handle_t* native_from_jlong(jlong h) {
    JniHandle* j = jni_from_jlong(h);
    return j != nullptr ? j->native : nullptr;
}

// --- Callbacks invoked by the SDK on its I/O thread ----------------

void on_result(sentlog_status_t status,
               const char* result_json,
               void* userdata) {
    auto* cb = static_cast<GlobalCallback*>(userdata);
    with_jni_env([&](JNIEnv* env) {
        jstring js = nullptr;
        if (result_json != nullptr) {
            js = env->NewStringUTF(result_json);
        }
        env->CallVoidMethod(cb->ref, cb->method,
                            static_cast<jint>(status), js);
        if (js != nullptr) env->DeleteLocalRef(js);
        env->DeleteGlobalRef(cb->ref);
    });
    delete cb;
}

void on_update(sentlog_status_t status,
               const char* update_json,
               void* userdata) {
    auto* cb = static_cast<GlobalCallback*>(userdata);
    with_jni_env([&](JNIEnv* env) {
        jstring js = nullptr;
        if (update_json != nullptr) {
            js = env->NewStringUTF(update_json);
        }
        env->CallVoidMethod(cb->ref, cb->method,
                            static_cast<jint>(status), js);
        if (js != nullptr) env->DeleteLocalRef(js);
        env->DeleteGlobalRef(cb->ref);
    });
    delete cb;
}

void on_telemetry(const char* event_json, void* userdata) {
    auto* cb = static_cast<GlobalCallback*>(userdata);
    with_jni_env([&](JNIEnv* env) {
        jstring js = nullptr;
        if (event_json != nullptr) {
            js = env->NewStringUTF(event_json);
        }
        env->CallVoidMethod(cb->ref, cb->method, js);
        if (js != nullptr) env->DeleteLocalRef(js);
        // telemetry callback is long-lived; do NOT delete the global ref.
    });
}

int on_drain_envelope(const char* envelope_json, size_t /*len*/, void* userdata) {
    auto* cb = static_cast<GlobalCallback*>(userdata);
    int keep_or_delete = 0;
    with_jni_env([&](JNIEnv* env) {
        jstring js = env->NewStringUTF(envelope_json ? envelope_json : "");
        const jboolean ret =
            env->CallBooleanMethod(cb->ref, cb->method, js);
        if (env->ExceptionCheck()) {
            LOGE("drain callback threw; treating as 'keep' to be safe");
            env->ExceptionDescribe();
            env->ExceptionClear();
            keep_or_delete = 0;
        } else {
            keep_or_delete = (ret == JNI_TRUE) ? 1 : 0;
        }
        env->DeleteLocalRef(js);
    });
    return keep_or_delete;
}

void on_envelope_dispatch(const char* forwarder_name,
                          const char* sink_kind,
                          const char* sink_json,
                          const char* envelope_json,
                          void* userdata) {
    auto* cb = static_cast<GlobalCallback*>(userdata);
    with_jni_env([&](JNIEnv* env) {
        jstring n = env->NewStringUTF(forwarder_name ? forwarder_name : "");
        jstring k = env->NewStringUTF(sink_kind      ? sink_kind      : "");
        jstring s = env->NewStringUTF(sink_json      ? sink_json      : "");
        jstring e = env->NewStringUTF(envelope_json  ? envelope_json  : "");
        env->CallVoidMethod(cb->ref, cb->method, n, k, s, e);
        // The dispatcher fires inside a loop over configured forwarders.
        // If the integrator's Kotlin dispatcher throws, the JVM leaves
        // the exception pending and the next JNI call inside the same
        // dispatch loop would assert/crash. Clear it (and log a tag the
        // integrator can grep). Exception lifetime + telemetry hook
        // lands in 4f.
        if (env->ExceptionCheck()) {
            LOGE("envelope dispatcher threw; clearing to keep the loop alive");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->DeleteLocalRef(n);
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(s);
        env->DeleteLocalRef(e);
        // long-lived; do NOT delete the global ref.
    });
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeVersion(
    JNIEnv* env, jclass /*clazz*/) {
    return env->NewStringUTF(sentlog_version());
}

JNIEXPORT jlong JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeCreate(
    JNIEnv* env, jclass /*clazz*/,
    jstring config_json,
    jobject telemetry_cb_obj,
    jintArray status_out) {

    if (config_json == nullptr) return 0;

    const char* config_cstr = env->GetStringUTFChars(config_json, nullptr);
    if (config_cstr == nullptr) return 0;

    sentlog_telemetry_cb_t telemetry_cb = nullptr;
    GlobalCallback* telemetry_userdata = nullptr;

    if (telemetry_cb_obj != nullptr) {
        const jclass cb_cls = env->GetObjectClass(telemetry_cb_obj);
        const jmethodID m = env->GetMethodID(cb_cls, "onTelemetry",
                                              "(Ljava/lang/String;)V");
        env->DeleteLocalRef(cb_cls);
        if (m != nullptr) {
            telemetry_userdata = new GlobalCallback{
                env->NewGlobalRef(telemetry_cb_obj), m};
            telemetry_cb = &on_telemetry;
        }
    }

    sentlog_status_t status = SENTLOG_OK;
    sentlog_handle_t* native =
        sentlog_create(config_cstr, telemetry_cb, telemetry_userdata, &status);

    env->ReleaseStringUTFChars(config_json, config_cstr);

    if (status_out != nullptr) {
        const jint s = static_cast<jint>(status);
        env->SetIntArrayRegion(status_out, 0, 1, &s);
    }

    if (native == nullptr) {
        if (telemetry_userdata != nullptr) {
            env->DeleteGlobalRef(telemetry_userdata->ref);
            delete telemetry_userdata;
        }
        return 0;
    }

    auto* j = new JniHandle{native, telemetry_userdata, nullptr};
    return jlong_from_jni(j);
}

JNIEXPORT void JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeDestroy(
    JNIEnv* env, jclass /*clazz*/, jlong handle) {
    JniHandle* j = jni_from_jlong(handle);
    if (j == nullptr) return;
    sentlog_destroy(j->native);
    if (j->telemetry != nullptr) {
        env->DeleteGlobalRef(j->telemetry->ref);
        delete j->telemetry;
    }
    if (j->dispatcher != nullptr) {
        env->DeleteGlobalRef(j->dispatcher->ref);
        delete j->dispatcher;
    }
    delete j;
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeStartCapture(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    return static_cast<jint>(sentlog_start_capture(native_from_jlong(handle)));
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativePushVadFrame(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle,
    jlong start_sample, jint num_samples, jfloat probability) {

    if (num_samples <= 0) return SENTLOG_OK;
    return static_cast<jint>(sentlog_push_vad_frame(
        native_from_jlong(handle),
        static_cast<size_t>(start_sample),
        static_cast<size_t>(num_samples),
        static_cast<float>(probability)));
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativePushLivenessResult(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle,
    jfloat score, jfloat replay_risk, jfloat synthetic_risk) {
    return static_cast<jint>(sentlog_push_liveness_result(
        native_from_jlong(handle),
        static_cast<float>(score),
        static_cast<float>(replay_risk),
        static_cast<float>(synthetic_risk)));
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativePushTranscript(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jstring transcript_json) {

    if (transcript_json == nullptr) return SENTLOG_ERR_INVALID_CONFIG;
    const char* j = env->GetStringUTFChars(transcript_json, nullptr);
    if (j == nullptr) return SENTLOG_ERR_INTERNAL;
    const jint rc = static_cast<jint>(
        sentlog_push_transcript(native_from_jlong(handle), j));
    env->ReleaseStringUTFChars(transcript_json, j);
    return rc;
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativePushMotionFrame(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle,
    jlong timestamp_ns,
    jfloat ax, jfloat ay, jfloat az,
    jfloat gx, jfloat gy, jfloat gz) {
    return static_cast<jint>(sentlog_push_motion_frame(
        native_from_jlong(handle),
        static_cast<int64_t>(timestamp_ns),
        static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(az),
        static_cast<float>(gx), static_cast<float>(gy), static_cast<float>(gz)));
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeSetAudioRoute(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle, jint route) {
    return static_cast<jint>(sentlog_set_audio_route(
        native_from_jlong(handle),
        static_cast<sentlog_audio_route_t>(route)));
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeSnapshotAudio(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jfloatArray out) {

    if (out == nullptr) return 0;
    const jsize cap = env->GetArrayLength(out);
    if (cap <= 0) return 0;

    // Fill into a heap buffer then copy to the Java array. Two-step
    // because env->Get/SetFloatArrayElements would lock the array and
    // we'd hold the lock across the C ABI call.
    std::unique_ptr<float[]> buf(new (std::nothrow) float[cap]);
    if (!buf) return 0;
    size_t written = 0;
    const sentlog_status_t s = sentlog_snapshot_audio(
        native_from_jlong(handle),
        buf.get(),
        static_cast<size_t>(cap),
        &written);
    if (s != SENTLOG_OK) return 0;
    env->SetFloatArrayRegion(out, 0, static_cast<jsize>(written), buf.get());
    return static_cast<jint>(written);
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativePushAudioFrame(
    JNIEnv* env, jclass /*clazz*/, jlong handle,
    jfloatArray samples, jint num_samples) {

    if (samples == nullptr || num_samples <= 0) return SENTLOG_OK;

    // Use GetFloatArrayRegion into a stack-allocated buffer? For
    // typical 20 ms frames at 16 kHz that's 320 samples = 1.28 KB —
    // safe on stack. For larger frames fall back to heap.
    constexpr jint kStackFrameSamples = 4096;
    if (num_samples <= kStackFrameSamples) {
        float buf[kStackFrameSamples];
        env->GetFloatArrayRegion(samples, 0, num_samples, buf);
        return static_cast<jint>(sentlog_push_audio_frame(
            native_from_jlong(handle), buf,
            static_cast<size_t>(num_samples)));
    }
    std::unique_ptr<float[]> buf(new (std::nothrow) float[num_samples]);
    if (!buf) return SENTLOG_ERR_INTERNAL;
    env->GetFloatArrayRegion(samples, 0, num_samples, buf.get());
    return static_cast<jint>(sentlog_push_audio_frame(
        native_from_jlong(handle), buf.get(),
        static_cast<size_t>(num_samples)));
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeEvaluate(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jobject result_cb_obj) {

    if (result_cb_obj == nullptr) return SENTLOG_ERR_INVALID_CONFIG;

    const jclass cb_cls = env->GetObjectClass(result_cb_obj);
    const jmethodID m = env->GetMethodID(cb_cls, "onResult",
                                          "(ILjava/lang/String;)V");
    env->DeleteLocalRef(cb_cls);
    if (m == nullptr) return SENTLOG_ERR_INTERNAL;

    auto* userdata = new GlobalCallback{env->NewGlobalRef(result_cb_obj), m};
    const sentlog_status_t s = sentlog_evaluate(
        native_from_jlong(handle), &on_result, userdata);
    if (s != SENTLOG_OK) {
        env->DeleteGlobalRef(userdata->ref);
        delete userdata;
    }
    return static_cast<jint>(s);
}

JNIEXPORT void JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeCancel(
    JNIEnv* /*env*/, jclass /*clazz*/, jlong handle) {
    sentlog_cancel(native_from_jlong(handle));
}

JNIEXPORT jstring JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeGetPermissions(
    JNIEnv* env, jclass /*clazz*/, jlong handle) {
    char* p = sentlog_get_permissions(native_from_jlong(handle));
    if (p == nullptr) return nullptr;
    jstring js = env->NewStringUTF(p);
    sentlog_string_free(p);
    return js;
}

JNIEXPORT jstring JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeGetCapabilities(
    JNIEnv* env, jclass /*clazz*/, jlong handle) {
    char* c = sentlog_get_capabilities(native_from_jlong(handle));
    if (c == nullptr) return nullptr;
    jstring js = env->NewStringUTF(c);
    sentlog_string_free(c);
    return js;
}

JNIEXPORT jstring JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeModelVersions(
    JNIEnv* env, jclass /*clazz*/, jlong handle) {
    char* m = sentlog_model_versions(native_from_jlong(handle));
    if (m == nullptr) return nullptr;
    jstring js = env->NewStringUTF(m);
    sentlog_string_free(m);
    return js;
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeSetSigningKey(
    JNIEnv* env, jclass /*clazz*/, jlong handle,
    jbyteArray secret_key_bytes, jstring verification_method) {

    if (secret_key_bytes == nullptr || verification_method == nullptr) {
        return SENTLOG_ERR_INVALID_CONFIG;
    }
    const jsize n = env->GetArrayLength(secret_key_bytes);
    auto* bytes = env->GetByteArrayElements(secret_key_bytes, nullptr);
    if (bytes == nullptr) return SENTLOG_ERR_INTERNAL;
    const char* vm = env->GetStringUTFChars(verification_method, nullptr);
    if (vm == nullptr) {
        env->ReleaseByteArrayElements(secret_key_bytes, bytes, JNI_ABORT);
        return SENTLOG_ERR_INTERNAL;
    }
    const jint rc = static_cast<jint>(sentlog_set_signing_key(
        native_from_jlong(handle),
        reinterpret_cast<const uint8_t*>(bytes),
        static_cast<size_t>(n),
        vm));
    env->ReleaseStringUTFChars(verification_method, vm);
    env->ReleaseByteArrayElements(secret_key_bytes, bytes, JNI_ABORT);
    return rc;
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeSetEnvelopeDispatcher(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jobject dispatcher_obj) {

    JniHandle* j = jni_from_jlong(handle);
    if (j == nullptr) return SENTLOG_ERR_INVALID_STATE;

    if (dispatcher_obj == nullptr) {
        // Clear the registration: unregister at the C ABI first so the
        // native side stops calling our trampoline, then free the
        // previously-tracked GlobalCallback.
        const jint rc = static_cast<jint>(sentlog_set_envelope_dispatcher(
            j->native, nullptr, nullptr));
        if (j->dispatcher != nullptr) {
            env->DeleteGlobalRef(j->dispatcher->ref);
            delete j->dispatcher;
            j->dispatcher = nullptr;
        }
        return rc;
    }

    const jclass cb_cls = env->GetObjectClass(dispatcher_obj);
    const jmethodID m = env->GetMethodID(
        cb_cls, "onEnvelope",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    env->DeleteLocalRef(cb_cls);
    if (m == nullptr) return SENTLOG_ERR_INTERNAL;

    auto* userdata = new GlobalCallback{
        env->NewGlobalRef(dispatcher_obj), m};
    const jint rc = static_cast<jint>(sentlog_set_envelope_dispatcher(
        j->native, &on_envelope_dispatch, userdata));
    if (rc != SENTLOG_OK) {
        env->DeleteGlobalRef(userdata->ref);
        delete userdata;
        return rc;
    }
    // Replace any prior registration. The C ABI now owns the new
    // userdata; the old one is unreferenced from the native side and
    // safe to free.
    if (j->dispatcher != nullptr) {
        env->DeleteGlobalRef(j->dispatcher->ref);
        delete j->dispatcher;
    }
    j->dispatcher = userdata;
    return rc;
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeCheckForUpdates(
    JNIEnv* env, jclass /*clazz*/, jlong handle, jobject update_cb_obj) {

    if (update_cb_obj == nullptr) return SENTLOG_ERR_INVALID_CONFIG;

    const jclass cb_cls = env->GetObjectClass(update_cb_obj);
    const jmethodID m = env->GetMethodID(cb_cls, "onUpdate",
                                          "(ILjava/lang/String;)V");
    env->DeleteLocalRef(cb_cls);
    if (m == nullptr) return SENTLOG_ERR_INTERNAL;

    auto* userdata = new GlobalCallback{env->NewGlobalRef(update_cb_obj), m};
    const sentlog_status_t s = sentlog_check_for_updates(
        native_from_jlong(handle), &on_update, userdata);
    if (s != SENTLOG_OK) {
        env->DeleteGlobalRef(userdata->ref);
        delete userdata;
    }
    return static_cast<jint>(s);
}

JNIEXPORT jint JNICALL
Java_me_cooki_sentlog_internal_NativeBridge_nativeDrainQueue(
    JNIEnv* env, jclass /*clazz*/, jlong handle,
    jstring path_j, jobject drain_cb_obj) {

    if (path_j == nullptr || drain_cb_obj == nullptr) return 0;

    const jclass cb_cls = env->GetObjectClass(drain_cb_obj);
    const jmethodID m = env->GetMethodID(
        cb_cls, "onEnvelope", "(Ljava/lang/String;)Z");
    env->DeleteLocalRef(cb_cls);
    if (m == nullptr) return 0;

    // GlobalRef + userdata box live only for the duration of this
    // call — drain is synchronous, unlike telemetry/dispatcher
    // which are long-lived.
    jobject ref = env->NewGlobalRef(drain_cb_obj);
    GlobalCallback gcb{ref, m};

    const char* path = env->GetStringUTFChars(path_j, nullptr);
    if (path == nullptr) {
        env->DeleteGlobalRef(ref);
        return 0;
    }
    const int delivered = sentlog_drain_queue(
        native_from_jlong(handle), path, &on_drain_envelope, &gcb);
    env->ReleaseStringUTFChars(path_j, path);
    env->DeleteGlobalRef(ref);
    return static_cast<jint>(delivered);
}

}  // extern "C"
