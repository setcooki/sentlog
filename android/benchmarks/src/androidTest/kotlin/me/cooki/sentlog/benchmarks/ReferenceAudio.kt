package me.cooki.sentlog.benchmarks

import kotlin.math.PI
import kotlin.math.sin

/**
 * Synthetic speech-like audio used by the benchmark workload.
 * Matches the spec in `docs/rollout-plan.md §2.2` — 15 s of mono
 * 16 kHz Float32. The content is a sum of three sines in the
 * speech-band frequencies, modulated by a 4 Hz amplitude envelope
 * so the energy VAD picks it up reliably (and Silero scores it as
 * speech with high probability).
 *
 * We synthesise rather than ship a real audio fixture so the repo
 * stays small (15 s of 16 kHz Float32 = 960 KB; a recorded clip
 * would also need licensing).
 *
 * For accuracy-style benchmarks (Phase 8) the harness will load
 * real labelled clips from a curated corpus.
 */
internal object ReferenceAudio {

    const val SAMPLE_RATE_HZ: Int = 16_000
    const val DURATION_SECONDS: Int = 15
    const val TOTAL_SAMPLES: Int = SAMPLE_RATE_HZ * DURATION_SECONDS

    fun synthesize(): FloatArray {
        val out = FloatArray(TOTAL_SAMPLES)
        val twoPi = (2.0 * PI).toFloat()
        for (i in 0 until TOTAL_SAMPLES) {
            val t = i.toFloat() / SAMPLE_RATE_HZ.toFloat()
            val env = sin(twoPi * 4f * t) * 0.5f + 0.5f
            out[i] = 0.30f * env * (
                sin(twoPi *  300f * t) +
                sin(twoPi *  700f * t) * 0.6f +
                sin(twoPi * 1500f * t) * 0.3f
            )
        }
        return out
    }
}
