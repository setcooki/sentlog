package me.cooki.sentlog

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import java.nio.ByteBuffer

/**
 * Phase 4g-ii helper: an Opus encoder backed by Android's MediaCodec
 * (which gained `audio/opus` encoding support in API 29 — Android 10).
 * Ready to plug into [Sentlog.setAudioEncoder]:
 *
 * ```kotlin
 * OpusAudioEncoder.install()
 * ```
 *
 * That single call registers the encoder closure; from then on every
 * Full+AudioClip / EvidenceClip envelope carries Opus bytes instead
 * of the placeholder block.
 *
 * Encodes 16 kHz mono float PCM (the SDK's capture format) to Opus
 * at 32 kbps — ~120 KiB worst case for a 30 s clip.
 *
 * **API requirement:** Opus encoding via MediaCodec requires API 29+.
 * On older devices `install()` throws [IllegalStateException]; the
 * integrator either bumps `minSdk` to 29, vendors libopus, or skips
 * the AudioClip projection on pre-29 devices.
 *
 * On encoder failure the closure returns null; the SDK's
 * dispatch_envelopes path then falls back to the placeholder
 * audio_clip block with the documented note.
 */
public object OpusAudioEncoder {

    /** Opus packets, each prefixed with its length as a big-endian
     *  uint16. See spec §4. */
    private const val CODEC_NAME = "opus-framed-v1"

    private const val SAMPLE_RATE_HZ   = 16_000
    private const val CHANNEL_COUNT    = 1
    private const val BIT_RATE         = 32_000   // bps; voice-quality default
    private const val DEQUEUE_TIMEOUT  = 10_000L  // µs
    private const val MAX_ENCODED_SIZE = 256 * 1024  // matches state.cpp cap

    /**
     * Install the Opus encoder against the SDK. Idempotent — replaces
     * any previously-registered encoder.
     *
     * Throws [IllegalStateException] when MediaCodec Opus encoding
     * isn't available (pre-API-29 device). Integrators wanting soft-
     * fail wrap in try/catch and skip the install — AudioClip
     * envelopes then ship the placeholder block.
     */
    @JvmStatic
    public fun install() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            throw IllegalStateException(
                "OpusAudioEncoder requires API 29+ (MediaCodec audio/opus " +
                "encoding); device is API ${Build.VERSION.SDK_INT}. " +
                "Bump minSdk, vendor libopus, or skip AudioClip projections.")
        }
        Sentlog.setAudioEncoder(
            encoder   = ::encodeOnce,
            codecName = CODEC_NAME)
    }

    /** Clear any previously-registered audio encoder. */
    @JvmStatic
    public fun uninstall() {
        Sentlog.setAudioEncoder(encoder = null)
    }

    // ------- internals -------

    /**
     * Encode a single PCM buffer to Opus bytes. Creates a fresh
     * MediaCodec instance per call — encoder construction is cheap
     * (~ms) compared to a 30 s encode, and keeping it stateless
     * avoids cross-evaluate state bleed.
     *
     * Returns null on any encoder error.
     */
    private fun encodeOnce(pcmSamples: FloatArray, sampleRateHz: Int): ByteArray? {
        if (sampleRateHz != SAMPLE_RATE_HZ || pcmSamples.isEmpty()) return null
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null

        val codec = try {
            MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
        } catch (_: Throwable) { return null }

        try {
            val format = MediaFormat.createAudioFormat(
                MediaFormat.MIMETYPE_AUDIO_OPUS,
                SAMPLE_RATE_HZ, CHANNEL_COUNT).apply {
                setInteger(MediaFormat.KEY_BIT_RATE, BIT_RATE)
                setInteger(MediaFormat.KEY_PCM_ENCODING,
                           android.media.AudioFormat.ENCODING_PCM_16BIT)
            }
            codec.configure(format, null, null,
                            MediaCodec.CONFIGURE_FLAG_ENCODE)
            codec.start()

            // Convert float PCM → 16-bit signed PCM. MediaCodec's
            // Opus encoder expects PCM_16BIT input.
            //
            // Scale by 32768 (NOT 32767) so the symmetric float range
            // [-1, 1] maps to the full signed-16 range [-32768, 32767]
            // without losing the negative full-scale bin: -1.0 → -32768
            // exactly, +1.0 → +32768 then coerced down to +32767 (the
            // standard one-LSB asymmetry that signed integers can't
            // avoid). Multiplying by 32767 — the previous code —
            // mapped -1.0 to -32767 and lost a bit of negative
            // dynamic range.
            val pcm16 = ByteArray(pcmSamples.size * 2)
            for (i in pcmSamples.indices) {
                val s = (pcmSamples[i].coerceIn(-1.0f, 1.0f) * 32768.0f).toInt()
                    .coerceIn(-32768, 32767)
                pcm16[i * 2]     = (s and 0xFF).toByte()
                pcm16[i * 2 + 1] = ((s shr 8) and 0xFF).toByte()
            }

            val encoded = java.io.ByteArrayOutputStream(64 * 1024)
            var inputPos = 0
            var sawInputEOS = false
            var sawOutputEOS = false
            val info = MediaCodec.BufferInfo()

            while (!sawOutputEOS && encoded.size() < MAX_ENCODED_SIZE) {
                if (!sawInputEOS) {
                    val inIdx = codec.dequeueInputBuffer(DEQUEUE_TIMEOUT)
                    if (inIdx >= 0) {
                        val inBuf: ByteBuffer = codec.getInputBuffer(inIdx)
                            ?: return null
                        inBuf.clear()
                        val chunk = minOf(inBuf.remaining(), pcm16.size - inputPos)
                        if (chunk > 0) {
                            inBuf.put(pcm16, inputPos, chunk)
                            inputPos += chunk
                        }
                        val eos = inputPos >= pcm16.size
                        codec.queueInputBuffer(
                            inIdx, 0, chunk,
                            (inputPos.toLong() * 1_000_000L) / SAMPLE_RATE_HZ / 2,
                            if (eos) MediaCodec.BUFFER_FLAG_END_OF_STREAM else 0)
                        if (eos) sawInputEOS = true
                    }
                }

                val outIdx = codec.dequeueOutputBuffer(info, DEQUEUE_TIMEOUT)
                if (outIdx >= 0) {
                    val outBuf: ByteBuffer = codec.getOutputBuffer(outIdx)
                        ?: continue
                    // Skip codec-specific data: it isn't an audio packet.
                    val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    if (info.size > 0 && !isConfig) {
                        val chunk = ByteArray(info.size)
                        outBuf.position(info.offset)
                        outBuf.get(chunk, 0, info.size)
                        // Opus packets don't delimit themselves.
                        encoded.write(info.size shr 8)
                        encoded.write(info.size and 0xff)
                        encoded.write(chunk)
                    }
                    codec.releaseOutputBuffer(outIdx, false)
                    if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        sawOutputEOS = true
                    }
                }
                // INFO_TRY_AGAIN_LATER / INFO_OUTPUT_FORMAT_CHANGED /
                // INFO_OUTPUT_BUFFERS_CHANGED: silently loop.
            }
            return if (encoded.size() > 0) encoded.toByteArray() else null
        } catch (_: Throwable) {
            return null
        } finally {
            try { codec.stop()    } catch (_: Throwable) {}
            try { codec.release() } catch (_: Throwable) {}
        }
    }
}
