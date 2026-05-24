import Foundation
import whisper

/// whisper.cpp ASR wrapper. Owns a `whisper_context` for the duration
/// of a capture; emits a JSON transcript ready to push through
/// `sentlog_push_transcript`.
///
/// Mirrors the Android `WhisperRunner.kt`. Models are loaded from
/// the SwiftPM `Bundle.module` resources — operators stage
/// `whisper-tiny.bin` per `tools/whisper-model/README.md`. When the
/// resource is absent the runner is `nil` and the C++ pipeline emits
/// the placeholder transcript.
///
/// See ADR 0009 — whisper.cpp runs on the platform side; the
/// transcript flows back into the shared core via push_transcript.
internal final class WhisperRunner {

    private let ctx: OpaquePointer
    private let wantWordTimings: Bool

    private init(ctx: OpaquePointer, wantWordTimings: Bool) {
        self.ctx             = ctx
        self.wantWordTimings = wantWordTimings
    }

    deinit { whisper_free(ctx) }

    /// Try to load the bundled model. Returns nil if the resource is
    /// missing (degrades to no-op mode) or whisper.cpp init fails.
    static func loadIfAvailable(wantWordTimings: Bool = true) -> WhisperRunner? {
        guard let url = Bundle.module.url(
            forResource: "whisper-tiny",
            withExtension: "bin"
        ) else {
            return nil
        }

        var cparams = whisper_context_default_params()
        cparams.use_gpu = false  // CPU-only on iOS for v1.0 — Core ML acceleration is a follow-up
        guard let ctx = url.path.withCString({ pathPtr in
            whisper_init_from_file_with_params(pathPtr, cparams)
        }) else {
            return nil
        }

        return WhisperRunner(ctx: ctx, wantWordTimings: wantWordTimings)
    }

    /// Run whisper.cpp on `audio` (16 kHz mono Float32, [-1, 1]).
    /// Returns a JSON string ready for `sentlog_push_transcript`, or
    /// `nil` if inference failed.
    ///
    /// Language is intentionally auto-detected (we don't bias whisper
    /// with config.language_hint) — the pipeline does post-hoc
    /// language gating against language_hint via the detected value
    /// coming back in the JSON, so biasing would mask mismatches.
    func transcribe(_ audio: [Float]) -> String? {
        guard !audio.isEmpty else { return nil }

        var params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY)
        params.print_realtime    = false
        params.print_progress    = false
        params.print_timestamps  = false
        params.print_special     = false
        params.translate         = false
        params.no_context        = true
        params.single_segment    = false
        params.suppress_blank    = true
        params.token_timestamps  = wantWordTimings
        params.n_threads         = 4
        params.language          = nil   // auto-detect; see doc above

        let rc = audio.withUnsafeBufferPointer { buf -> Int32 in
            whisper_full(ctx, params, buf.baseAddress, Int32(buf.count))
        }
        if rc != 0 { return nil }

        return assembleJson()
    }

    private func assembleJson() -> String? {
        var text = ""
        let nSegments = whisper_full_n_segments(ctx)
        for i in 0..<nSegments {
            if let seg = whisper_full_get_segment_text(ctx, i) {
                text += String(cString: seg)
            }
        }
        text = text.trimmingCharacters(in: .whitespaces)

        let langId = whisper_full_lang_id(ctx)
        let lang   = whisper_lang_str(langId).flatMap { String(cString: $0) } ?? "und"

        // confidence — mean of per-token probabilities, ignoring
        // special tokens (the EOT and anything above).
        var sumP = 0.0
        var nTok = 0
        let eot  = whisper_token_eot(ctx)
        for i in 0..<nSegments {
            let nt = whisper_full_n_tokens(ctx, i)
            for t in 0..<nt {
                let td = whisper_full_get_token_data(ctx, i, t)
                if td.id >= eot { continue }
                sumP += Double(td.p)
                nTok += 1
            }
        }
        let confidence = (nTok > 0) ? (sumP / Double(nTok)) : 0.0

        var json = "{"
        json += "\"text\":\"\(jsonEscape(text))\","
        json += "\"language\":\"\(lang)\","
        json += "\"confidence\":\(confidence)"

        if wantWordTimings {
            var first = true
            json += ",\"word_timings\":["
            for i in 0..<nSegments {
                let nt = whisper_full_n_tokens(ctx, i)
                for t in 0..<nt {
                    let td = whisper_full_get_token_data(ctx, i, t)
                    if td.id >= eot { continue }
                    guard let tokPtr = whisper_full_get_token_text(ctx, i, t) else { continue }
                    let tok = String(cString: tokPtr)
                    // whisper timestamps are in centiseconds (10 ms).
                    let startMs = Int64(td.t0) * 10
                    let endMs   = Int64(td.t1) * 10
                    if !first { json += "," }
                    first = false
                    json += "{\"word\":\"\(jsonEscape(tok))\","
                    json += "\"start_ms\":\(startMs),\"end_ms\":\(endMs)}"
                }
            }
            json += "]"
        }

        json += "}"
        return json
    }

    /// JSON-string-literal escape. The transcript text and per-token
    /// strings are the only user-content paths into the assembled
    /// JSON; the keys, numbers, and structure are constants.
    private func jsonEscape(_ s: String) -> String {
        var out = ""
        out.reserveCapacity(s.count + 8)
        for scalar in s.unicodeScalars {
            switch scalar {
            case "\"":   out += "\\\""
            case "\\":   out += "\\\\"
            case "\n":   out += "\\n"
            case "\r":   out += "\\r"
            case "\t":   out += "\\t"
            case "\u{08}": out += "\\b"
            case "\u{0C}": out += "\\f"
            default:
                if scalar.value < 0x20 {
                    out += String(format: "\\u%04x", scalar.value)
                } else {
                    out += String(scalar)
                }
            }
        }
        return out
    }
}
