package me.cooki.sentlog.internal

import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import java.nio.FloatBuffer
import java.nio.LongBuffer

/**
 * Streaming Silero VAD (v5) wrapper. Owns an ONNX Runtime session,
 * keeps the LSTM hidden state across frames, and exposes a single
 * `infer(frame)` call that returns the per-frame speech probability.
 *
 * Silero v5 requires exactly [FRAME_SAMPLES] samples per inference at
 * 16 kHz; callers feed it fixed-size chunks. The [Chunker] companion
 * accumulates variable-size audio reads into the right chunk size.
 *
 * Not part of the public API. Owned by [me.cooki.sentlog.Sentlog].
 *
 * Thread-safety: not safe to call [infer] concurrently with itself
 * (the hidden state mutates). Construct once per capture; close on
 * teardown.
 */
internal class SileroVadStream(modelBytes: ByteArray) : AutoCloseable {

    private val env: OrtEnvironment = OrtEnvironment.getEnvironment()
    private val session: OrtSession

    // Silero v5 hidden state shape: [2, batch=1, 128].
    private val state = FloatArray(STATE_SIZE)

    // Sample rate is an int64 scalar input to the model.
    private val sampleRateScalar = longArrayOf(SAMPLE_RATE_HZ.toLong())

    init {
        val opts = OrtSession.SessionOptions().apply {
            // Single-threaded — Silero is small and per-chunk inference
            // is fast enough that thread-pool overhead dominates.
            setIntraOpNumThreads(1)
            setInterOpNumThreads(1)
        }
        session = env.createSession(modelBytes, opts)
    }

    /**
     * Run one frame through Silero. [frame] must be exactly
     * [FRAME_SAMPLES] samples of mono 16 kHz float audio.
     * Returns the speech probability in [0,1].
     */
    fun infer(frame: FloatArray): Float {
        require(frame.size == FRAME_SAMPLES) {
            "Silero v5 expects $FRAME_SAMPLES samples per chunk, got ${frame.size}"
        }

        val inputTensor = OnnxTensor.createTensor(
            env,
            FloatBuffer.wrap(frame),
            longArrayOf(1L, FRAME_SAMPLES.toLong()),
        )
        val stateTensor = OnnxTensor.createTensor(
            env,
            FloatBuffer.wrap(state),
            longArrayOf(2L, 1L, STATE_HIDDEN_SIZE.toLong()),
        )
        val srTensor = OnnxTensor.createTensor(
            env,
            LongBuffer.wrap(sampleRateScalar),
            longArrayOf(),
        )

        val inputs = mapOf(
            "input" to inputTensor,
            "state" to stateTensor,
            "sr"    to srTensor,
        )
        val outputs = session.run(inputs)

        try {
            // output: float32 [1, 1] speech probability
            @Suppress("UNCHECKED_CAST")
            val probArr = outputs.get("output").get().value as Array<FloatArray>
            val prob = probArr[0][0]

            // stateN: float32 [2, 1, 128] — copy back into our state.
            @Suppress("UNCHECKED_CAST")
            val newState = outputs.get("stateN").get().value as Array<Array<FloatArray>>
            for (i in 0 until 2) {
                for (j in 0 until STATE_HIDDEN_SIZE) {
                    state[i * STATE_HIDDEN_SIZE + j] = newState[i][0][j]
                }
            }
            return prob
        } finally {
            outputs.close()
            inputs.values.forEach { it.close() }
        }
    }

    /** Reset the LSTM state. Call between captures. */
    fun reset() {
        state.fill(0f)
    }

    override fun close() {
        session.close()
    }

    /**
     * Accumulates variable-size audio reads into fixed-size Silero
     * chunks. Calls back on every full chunk with (start_sample,
     * num_samples, samples_array).
     */
    internal class Chunker(
        private val onChunk: (startSample: Long, samples: FloatArray) -> Unit,
    ) {
        private val buf = FloatArray(FRAME_SAMPLES)
        private var bufLen = 0
        private var samplesSeen: Long = 0L

        fun reset() {
            bufLen = 0
            samplesSeen = 0L
        }

        fun feed(samples: FloatArray, n: Int) {
            var srcIdx = 0
            while (srcIdx < n) {
                val take = minOf(FRAME_SAMPLES - bufLen, n - srcIdx)
                System.arraycopy(samples, srcIdx, buf, bufLen, take)
                bufLen += take
                srcIdx += take
                if (bufLen == FRAME_SAMPLES) {
                    val start = samplesSeen
                    onChunk(start, buf.copyOf())
                    samplesSeen += FRAME_SAMPLES
                    bufLen = 0
                }
            }
        }
    }

    companion object {
        const val SAMPLE_RATE_HZ: Int   = 16_000
        const val FRAME_SAMPLES: Int    = 512          // ~32 ms at 16 kHz
        const val STATE_HIDDEN_SIZE: Int = 128
        const val STATE_SIZE: Int       = 2 * STATE_HIDDEN_SIZE  // [2, 1, 128] flattened
    }
}
