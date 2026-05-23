// JNI shim around whisper.cpp. Owned by the Android binding's
// WhisperRunner.kt — not part of the shared/ C++ core. ADR 0009 keeps
// shared/ free of ML runtimes; the transcript comes back into the
// core via sentlog_push_transcript.
//
// Three entry points:
//   nativeInit(modelPath)        -> handle (long) or 0 on failure
//   nativeTranscribe(handle, samples[], wantWordTimings) -> JSON or null
//   nativeRelease(handle)
//
// Transcription is synchronous on the calling thread (the Kotlin side
// schedules it on Dispatchers.Default). Audio must be 16 kHz mono
// Float32 in [-1, 1] — same as the SDK's capture path.

#include <jni.h>

#include <whisper.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct WhisperHandle {
    whisper_context* ctx = nullptr;
};

constexpr int kSampleRateHz = 16000;

// Escape a UTF-8 string for inclusion in a JSON string literal. We
// don't pull nlohmann/json into the binding to keep the JNI lib lean;
// the strings we emit are small and known-shape.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Trim leading/trailing whitespace from whisper segments — whisper.cpp
// often emits a leading space.
std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

}  // namespace

extern "C"
JNIEXPORT jlong JNICALL
Java_me_cooki_sentlog_internal_WhisperRunner_nativeInit(
    JNIEnv* env, jclass, jstring model_path_j) {

    const char* path = env->GetStringUTFChars(model_path_j, nullptr);
    if (path == nullptr) return 0;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;  // CPU-only on Android for v1.0 — see ADR 0009 / Phase 2 notes
    whisper_context* ctx = whisper_init_from_file_with_params(path, cparams);
    env->ReleaseStringUTFChars(model_path_j, path);

    if (ctx == nullptr) return 0;
    auto* h = new WhisperHandle{ctx};
    return reinterpret_cast<jlong>(h);
}

extern "C"
JNIEXPORT void JNICALL
Java_me_cooki_sentlog_internal_WhisperRunner_nativeRelease(
    JNIEnv*, jclass, jlong handle_j) {

    if (handle_j == 0) return;
    auto* h = reinterpret_cast<WhisperHandle*>(handle_j);
    if (h->ctx != nullptr) whisper_free(h->ctx);
    delete h;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_me_cooki_sentlog_internal_WhisperRunner_nativeTranscribe(
    JNIEnv* env, jclass,
    jlong handle_j,
    jfloatArray samples_j,
    jstring language_hint_j,
    jboolean want_word_timings) {

    if (handle_j == 0 || samples_j == nullptr) return nullptr;
    auto* h = reinterpret_cast<WhisperHandle*>(handle_j);
    if (h->ctx == nullptr) return nullptr;

    const jsize n = env->GetArrayLength(samples_j);
    if (n <= 0) return nullptr;

    // Critical mode avoids a copy when the JVM can pin the array.
    auto* pcm = env->GetFloatArrayElements(samples_j, nullptr);
    if (pcm == nullptr) return nullptr;

    const char* lang = nullptr;
    if (language_hint_j != nullptr) {
        lang = env->GetStringUTFChars(language_hint_j, nullptr);
    }

    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_realtime   = false;
    params.print_progress   = false;
    params.print_timestamps = false;
    params.print_special    = false;
    params.translate        = false;
    params.no_context       = true;     // we don't carry context across captures
    params.single_segment   = false;
    params.suppress_blank   = true;
    params.token_timestamps = want_word_timings == JNI_TRUE;
    params.language         = lang;     // nullptr -> auto-detect
    params.n_threads        = 4;

    const int rc = whisper_full(h->ctx, params,
                                pcm, static_cast<int>(n));

    if (lang != nullptr) {
        env->ReleaseStringUTFChars(language_hint_j, lang);
    }
    env->ReleaseFloatArrayElements(samples_j, pcm, JNI_ABORT);

    if (rc != 0) return nullptr;

    // Assemble JSON shaped for sentlog_push_transcript.
    std::ostringstream out;
    out << "{";

    // text — concatenated segment texts, trimmed.
    std::string text;
    const int n_segments = whisper_full_n_segments(h->ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* seg = whisper_full_get_segment_text(h->ctx, i);
        if (seg != nullptr) text += seg;
    }
    text = trim(text);
    out << "\"text\":\"" << json_escape(text) << "\",";

    // language — auto-detected when language=nullptr was passed.
    const int lang_id = whisper_full_lang_id(h->ctx);
    const char* lang_code = whisper_lang_str(lang_id);
    out << "\"language\":\"" << (lang_code ? lang_code : "und") << "\",";

    // confidence — mean exp(p) across all non-special tokens. whisper.cpp
    // stores per-token log-probs in token_data.p (already a probability,
    // not a log-prob, despite the field name).
    double sum_p = 0.0;
    int    n_tok = 0;
    for (int i = 0; i < n_segments; ++i) {
        const int nt = whisper_full_n_tokens(h->ctx, i);
        for (int t = 0; t < nt; ++t) {
            const auto td = whisper_full_get_token_data(h->ctx, i, t);
            if (td.id >= whisper_token_eot(h->ctx)) continue;  // skip specials
            sum_p += td.p;
            ++n_tok;
        }
    }
    const double confidence = (n_tok > 0) ? (sum_p / n_tok) : 0.0;
    out << "\"confidence\":" << confidence;

    // word_timings — only when requested. Each "word" here is one
    // whisper token; the binding can post-merge into orthographic
    // words if needed. start/end in ms from capture start.
    if (want_word_timings == JNI_TRUE) {
        out << ",\"word_timings\":[";
        bool first = true;
        for (int i = 0; i < n_segments; ++i) {
            const int nt = whisper_full_n_tokens(h->ctx, i);
            for (int t = 0; t < nt; ++t) {
                const auto td = whisper_full_get_token_data(h->ctx, i, t);
                if (td.id >= whisper_token_eot(h->ctx)) continue;
                const char* tok = whisper_full_get_token_text(h->ctx, i, t);
                if (tok == nullptr) continue;
                // whisper timestamps are in centiseconds (10 ms units)
                // — see whisper.h header comments on t0/t1 token fields.
                const long start_ms = static_cast<long>(td.t0) * 10;
                const long end_ms   = static_cast<long>(td.t1) * 10;
                if (!first) out << ",";
                first = false;
                out << "{\"word\":\"" << json_escape(tok)
                    << "\",\"start_ms\":" << start_ms
                    << ",\"end_ms\":" << end_ms << "}";
            }
        }
        out << "]";
    }

    out << "}";
    return env->NewStringUTF(out.str().c_str());
}

// kSampleRateHz exists for future bounds-checking; reference once so
// -Werror=unused-const-variable doesn't trip if the strict-warnings job
// adds that flag later.
static_assert(kSampleRateHz == 16000, "Sentlog ASR samples must be 16 kHz mono");
