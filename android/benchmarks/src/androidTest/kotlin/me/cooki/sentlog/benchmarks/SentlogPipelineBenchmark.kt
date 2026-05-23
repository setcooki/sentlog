package me.cooki.sentlog.benchmarks

import android.os.Debug
import androidx.benchmark.junit4.BenchmarkRule
import androidx.benchmark.junit4.measureRepeated
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import me.cooki.sentlog.Sentlog
import me.cooki.sentlog.SentlogConfig
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Phase 1 exit-gate benchmark. Measures the warm and cold latency of
 * `Sentlog.evaluate()` against the thresholds in
 * `docs/rollout-plan.md §2.3` (warm p50 ≤ 6 s, warm p95 ≤ 9 s, cold
 * model load ≤ 4 s, peak memory ≤ 350 MB).
 *
 * Workload mirrors §2.2: 15 s of synthetic mono 16 kHz Float32 audio,
 * full pipeline (VAD → ASR placeholder → liveness if model bundled →
 * scoring → fingerprint placeholder). Audio is pushed directly via
 * the C ABI rather than captured from a real mic — that keeps the
 * benchmark deterministic and avoids permission UX from confounding
 * timing.
 *
 * Run on each of the three target devices in `docs/rollout-plan.md
 * §2.1`. The JSON output from androidx.benchmark goes through
 * `tools/benchmark/check-thresholds.py` to produce the pass/fail
 * verdict that gets recorded in `docs/benchmarks/wk2.md`.
 */
@RunWith(AndroidJUnit4::class)
class SentlogPipelineBenchmark {

    @get:Rule
    val benchmarkRule = BenchmarkRule()

    private lateinit var audio: FloatArray

    @Before
    fun setUp() {
        audio = ReferenceAudio.synthesize()
    }

    /**
     * Warm latency — the SDK is initialised once outside the
     * timed block; only evaluate() and the push-audio loop are
     * measured. androidx.benchmark runs many iterations and
     * reports min / median / p95 / max in the JSON output.
     */
    @Test
    fun warmEvaluate() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        Sentlog.initialize(
            config = SentlogConfig(issuer = "did:example:bench"),
            context = ctx,
        )
        try {
            benchmarkRule.measureRepeated {
                Sentlog.startCapture()
                Sentlog.pushAllAudio(audio)
                runBlocking { Sentlog.evaluate() }
            }
        } finally {
            Sentlog.destroy()
        }
    }

    /**
     * Cold model load — measures the cost of initialize() including
     * model bundle reads. Iterations destroy+reinit so each tick is a
     * cold start.
     */
    @Test
    fun coldInitialize() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        benchmarkRule.measureRepeated {
            Sentlog.initialize(
                config = SentlogConfig(issuer = "did:example:bench"),
                context = ctx,
            )
            Sentlog.destroy()
        }
    }

    /**
     * Peak native heap after a single evaluate. Not a timed
     * measurement — captured from `android.os.Debug` and surfaced
     * via the benchmark's `traceLog` so the threshold checker can
     * parse it from the JSON output.
     */
    @Test
    fun peakNativeHeapAfterEvaluate() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        Sentlog.initialize(
            config = SentlogConfig(issuer = "did:example:bench"),
            context = ctx,
        )
        try {
            Sentlog.startCapture()
            Sentlog.pushAllAudio(audio)
            runBlocking { Sentlog.evaluate() }

            val nativeHeapBytes = Debug.getNativeHeapAllocatedSize()
            // The benchmark library doesn't expose a "report a number"
            // hook; instead we tag the test name with the measurement
            // so check-thresholds.py can extract it from the JSON.
            println("SENTLOG_BENCH_PEAK_NATIVE_HEAP_BYTES=$nativeHeapBytes")
        } finally {
            Sentlog.destroy()
        }
    }
}

/**
 * Test-only extension that splits the reference audio into 320-sample
 * (20 ms) chunks and pushes via the public Sentlog API. The benchmark
 * doesn't have a real AudioRecord pipeline running.
 */
private fun Sentlog.pushAllAudio(audio: FloatArray) {
    // The push happens on the test thread — not via AudioRecord. This
    // measures the pipeline cost, not capture latency. Capture
    // latency is a separate concern; the benchmark gate is about
    // post-capture work.
    val bridge = Class.forName("me.cooki.sentlog.internal.NativeBridge")
    val handleField = Sentlog::class.java.getDeclaredField("handle").apply { isAccessible = true }
    val handle = handleField.getLong(this)
    val pushMethod = bridge.getMethod(
        "nativePushAudioFrame", Long::class.java, FloatArray::class.java, Int::class.java
    )
    val frame = FloatArray(320)
    var i = 0
    while (i + frame.size <= audio.size) {
        System.arraycopy(audio, i, frame, 0, frame.size)
        pushMethod.invoke(null, handle, frame, frame.size)
        i += frame.size
    }
}
