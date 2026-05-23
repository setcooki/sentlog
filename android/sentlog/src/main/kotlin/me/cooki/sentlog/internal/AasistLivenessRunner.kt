package me.cooki.sentlog.internal

import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import java.nio.FloatBuffer
import kotlin.math.exp

/**
 * AASIST-L liveness inference. Utterance-level: takes the full
 * captured audio and emits a `LivenessResult` the binding pushes via
 * `sentlog_push_liveness_result`.
 *
 * AASIST-L doesn't distinguish replay-vs-synthetic attack types; the
 * binary spoof probability is reported in both `replayRisk` and
 * `syntheticRisk`. If/when a separate attack-type classifier ships,
 * the runner splits the probability appropriately.
 *
 * Construct once per capture and dispose at evaluate / cancel / destroy.
 * Audio length is padded or truncated to [targetSamples] before
 * inference; the ONNX graph supports dynamic length so the truncation
 * is purely for inference-cost predictability.
 */
internal class AasistLivenessRunner(modelBytes: ByteArray) : AutoCloseable {

    private val env: OrtEnvironment = OrtEnvironment.getEnvironment()
    private val session: OrtSession

    init {
        val opts = OrtSession.SessionOptions().apply {
            setIntraOpNumThreads(1)
            setInterOpNumThreads(1)
        }
        session = env.createSession(modelBytes, opts)
    }

    /**
     * Run AASIST-L on [audio]. Pads with zeros or truncates (keeping
     * the most recent samples — the last 4 s of the utterance carries
     * the most evidence) to [targetSamples] before inference.
     */
    fun infer(audio: FloatArray): LivenessResult {
        val prepared = prepareAudio(audio)

        val inputTensor = OnnxTensor.createTensor(
            env,
            FloatBuffer.wrap(prepared),
            longArrayOf(1L, prepared.size.toLong()),
        )

        val outputs = try {
            session.run(mapOf("audio" to inputTensor))
        } finally {
            inputTensor.close()
        }

        try {
            // logits: float32 [1, 2] — index 0 = genuine, 1 = spoof.
            @Suppress("UNCHECKED_CAST")
            val logits = (outputs.get("logits").get().value as Array<FloatArray>)[0]
            val probs = softmax2(logits)
            val genuine = probs[0]
            val spoof   = probs[1]
            return LivenessResult(
                score          = genuine,
                replayRisk     = spoof,
                syntheticRisk  = spoof,
            )
        } finally {
            outputs.close()
        }
    }

    override fun close() {
        session.close()
    }

    private fun prepareAudio(input: FloatArray): FloatArray {
        if (input.size == TARGET_SAMPLES) return input
        if (input.size > TARGET_SAMPLES) {
            // Keep the most recent TARGET_SAMPLES.
            return input.copyOfRange(input.size - TARGET_SAMPLES, input.size)
        }
        val padded = FloatArray(TARGET_SAMPLES)
        System.arraycopy(input, 0, padded, 0, input.size)
        return padded
    }

    /** Numerically-stable 2-class softmax. */
    private fun softmax2(logits: FloatArray): FloatArray {
        require(logits.size == 2)
        val max = if (logits[0] > logits[1]) logits[0] else logits[1]
        val e0 = exp((logits[0] - max).toDouble())
        val e1 = exp((logits[1] - max).toDouble())
        val sum = e0 + e1
        return floatArrayOf((e0 / sum).toFloat(), (e1 / sum).toFloat())
    }

    companion object {
        const val SAMPLE_RATE_HZ:  Int = 16_000
        const val TARGET_SECONDS:  Int = 4
        const val TARGET_SAMPLES:  Int = SAMPLE_RATE_HZ * TARGET_SECONDS
    }
}

/** What AasistLivenessRunner.infer returns. Mirrors `Liveness` on the
 *  C++ side and what `sentlog_push_liveness_result` accepts. */
internal data class LivenessResult(
    val score:         Float,
    val replayRisk:    Float,
    val syntheticRisk: Float,
)
