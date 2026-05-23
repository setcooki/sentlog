package me.cooki.sentlog.internal

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Process

/**
 * Real-time audio capture on a dedicated thread. Captures
 * 16 kHz mono PCM_FLOAT and pushes each frame to [pushFrame].
 *
 * Owned by [me.cooki.sentlog.Sentlog]; not part of the public API.
 *
 * Threading: [start] spawns a background thread, [stop] joins it.
 * [pushFrame] is invoked from the background thread.
 */
internal class AudioCapture(
    private val pushFrame: (FloatArray, Int) -> Int,
) {

    @Volatile private var running: Boolean = false
    private var thread: Thread? = null
    private var record: AudioRecord? = null

    fun start() {
        if (running) return

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
