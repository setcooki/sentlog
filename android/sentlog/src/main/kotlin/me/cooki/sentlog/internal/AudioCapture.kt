package me.cooki.sentlog.internal

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Process

/**
 * Real-time audio capture on a dedicated thread. Captures
 * 16 kHz mono PCM_FLOAT and pushes each frame to [pushFrame].
 *
 * If [vad] is non-null, captured audio is also fed into a Silero VAD
 * chunker; full Silero chunks (512 samples) trigger inference and the
 * resulting speech probability is pushed via [pushVad]. Silero
 * inference runs inline on this thread — measured at ~1 ms per chunk
 * on mid-range Android hardware (validated in the Phase 1g benchmark
 * gate). If inference time becomes a problem, move it to a worker
 * thread via an SPSC queue.
 *
 * Owned by [me.cooki.sentlog.Sentlog]; not part of the public API.
 *
 * Threading: [start] spawns a background thread, [stop] joins it.
 * [pushFrame] and [pushVad] are invoked from the background thread.
 */
internal class AudioCapture(
    private val pushFrame: (FloatArray, Int) -> Int,
    private val vad: SileroVadStream? = null,
    private val pushVad: ((startSample: Long, numSamples: Int, probability: Float) -> Int)? = null,
) {

    @Volatile private var running: Boolean = false
    private var thread: Thread? = null
    private var record: AudioRecord? = null
    private val vadChunker: SileroVadStream.Chunker? = vad?.let { v ->
        SileroVadStream.Chunker { startSample, chunk ->
            val prob = try {
                v.infer(chunk)
            } catch (t: Throwable) {
                // Silero failure on a single chunk shouldn't take down
                // capture. Fall back to energy VAD by not pushing.
                return@Chunker
            }
            pushVad?.invoke(startSample, chunk.size, prob)
        }
    }

    fun start() {
        if (running) return
        vadChunker?.reset()
        vad?.reset()

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE_HZ,
            CHANNEL_CONFIG,
            AUDIO_FORMAT_FLOAT,
        )
        if (minBuf <= 0) {
            throw IllegalStateException(
                "AudioRecord.getMinBufferSize returned $minBuf — capture unsupported"
            )
        }
        // Bytes per float * 2x headroom so we don't xrun under typical
        // scheduling jitter.
        val bufferSizeBytes = (minBuf * 2).coerceAtLeast(
            FRAME_SIZE_SAMPLES * BYTES_PER_FLOAT * 8
        )

        val rec = AudioRecord(
            MediaRecorder.AudioSource.VOICE_RECOGNITION,
            SAMPLE_RATE_HZ,
            CHANNEL_CONFIG,
            AUDIO_FORMAT_FLOAT,
            bufferSizeBytes,
        )
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            throw IllegalStateException(
                "AudioRecord failed to initialise (state=${rec.state})"
            )
        }
        record = rec

        running = true
        thread = Thread({ readLoop(rec) }, "sentlog-audio-capture").apply {
            isDaemon = true
            start()
        }
        rec.startRecording()
    }

    fun stop() {
        if (!running) return
        running = false
        thread?.join(STOP_JOIN_TIMEOUT_MS)
        thread = null
        record?.let {
            try {
                it.stop()
            } catch (_: IllegalStateException) {
                // already stopped
            }
            it.release()
        }
        record = null
    }

    private fun readLoop(rec: AudioRecord) {
        // Pin this thread to Android's urgent-audio priority so we
        // stay scheduled even under load. THREAD_PRIORITY_URGENT_AUDIO
        // maps to Linux nice -19 with the audio cgroup, which is what
        // AudioFlinger uses for real-time capture threads.
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)

        val buf = FloatArray(FRAME_SIZE_SAMPLES)
        while (running) {
            val n = rec.read(buf, 0, buf.size, AudioRecord.READ_BLOCKING)
            if (n <= 0) {
                // ERROR_INVALID_OPERATION / ERROR_BAD_VALUE / ERROR_DEAD_OBJECT —
                // bail out of the loop; lifecycle layer surfaces the
                // failure on the next call.
                break
            }
            pushFrame(buf, n)
            // VAD path runs inline; failures inside the chunker callback
            // are caught and swallowed there (energy VAD still runs in
            // the C++ pipeline as a fallback).
            vadChunker?.feed(buf, n)
        }
    }

    private companion object {
        const val SAMPLE_RATE_HZ      = 16_000
        const val FRAME_SIZE_SAMPLES  = 320            // 20 ms @ 16 kHz
        const val BYTES_PER_FLOAT     = 4
        const val STOP_JOIN_TIMEOUT_MS = 500L
        val CHANNEL_CONFIG: Int = AudioFormat.CHANNEL_IN_MONO
        val AUDIO_FORMAT_FLOAT: Int = AudioFormat.ENCODING_PCM_FLOAT
    }
}
