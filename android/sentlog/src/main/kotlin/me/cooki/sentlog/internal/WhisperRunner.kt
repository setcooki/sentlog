package me.cooki.sentlog.internal

import android.content.Context
import java.io.File
import java.io.FileNotFoundException
import java.io.FileOutputStream

/**
 * whisper.cpp ASR wrapper. Owns a native whisper_context for the
 * duration of a capture; emits a JSON transcript ready to push
 * through `sentlog_push_transcript`.
 *
 * The native side does the heavy lifting; this class is a thin
 * lifecycle wrapper. Construct once per capture, dispose at evaluate /
 * cancel / destroy.
 *
 * Models are loaded from app assets. The asset bytes are copied to
 * the app cache directory because whisper.cpp's loader needs a real
 * filesystem path (it `mmap`s the file). If the asset isn't present
 * the binding falls back to no-op mode and the C++ pipeline emits
 * the placeholder transcript.
 *
 * See ADR 0009 — whisper.cpp runs on the platform side; the
 * transcript flows back into the shared core via push_transcript.
 */
internal class WhisperRunner private constructor(
    private val handle: Long,
    private val wantWordTimings: Boolean,
) : AutoCloseable {

    /**
     * Run whisper.cpp on [audio] (16 kHz mono Float32, [-1, 1]).
     * Returns a JSON string ready for `sentlog_push_transcript`, or
     * `null` if inference failed.
     *
     * Language is intentionally auto-detected (we don't bias whisper
     * with config.language_hint) — the pipeline does post-hoc
     * language gating against language_hint via the detected value
     * coming back in the JSON, so biasing would mask mismatches.
     */
    fun transcribe(audio: FloatArray): String? {
        if (audio.isEmpty()) return null
        return nativeTranscribe(handle, audio, /* languageHint = */ null, wantWordTimings)
    }

    override fun close() {
        if (handle != 0L) nativeRelease(handle)
    }

    companion object {
        /** Name of the model bundle in app assets. Operators drop the
         *  downloaded whisper-tiny-q5_1.bin in at the matching name
         *  per tools/whisper-model/README.md. */
        const val ASSET_NAME = "whisper-tiny.bin"

        /**
         * Try to load the bundled model. Returns null if the asset
         * is missing (degrades to no-op mode) or whisper.cpp init
         * fails for any reason.
         */
        fun loadIfAvailable(
            ctx: Context,
            wantWordTimings: Boolean = true,
        ): WhisperRunner? {
            NativeBridge.ensureLoaded()
            val modelPath = stageAssetToCache(ctx) ?: return null
            val handle = nativeInit(modelPath.absolutePath)
            if (handle == 0L) return null
            return WhisperRunner(handle, wantWordTimings)
        }

        private fun stageAssetToCache(ctx: Context): File? {
            val out = File(ctx.cacheDir, ASSET_NAME)
            // Re-use the staged copy when its size matches the asset.
            // We don't bother with checksums here — the asset and the
            // staged file have a 1:1 lifetime within the app sandbox.
            try {
                ctx.assets.openFd(ASSET_NAME).use { afd ->
                    if (out.exists() && out.length() == afd.length) return out
                }
            } catch (_: FileNotFoundException) {
                return null
            }
            return try {
                ctx.assets.open(ASSET_NAME).use { input ->
                    FileOutputStream(out).use { fout -> input.copyTo(fout) }
                }
                out
            } catch (_: FileNotFoundException) {
                null
            }
        }

        @JvmStatic external fun nativeInit(modelPath: String): Long
        @JvmStatic external fun nativeTranscribe(
            handle: Long,
            samples: FloatArray,
            languageHint: String?,
            wantWordTimings: Boolean,
        ): String?
        @JvmStatic external fun nativeRelease(handle: Long)
    }
}
