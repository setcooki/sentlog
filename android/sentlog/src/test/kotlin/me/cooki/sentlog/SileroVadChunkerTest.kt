package me.cooki.sentlog

import me.cooki.sentlog.internal.SileroVadStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests for SileroVadStream.Chunker — the variable-input → fixed-Silero-
 * frame re-chunker that sits between AudioRecord's 320-sample frames
 * and Silero's required 512-sample chunks.
 *
 * Does NOT exercise the ONNX inference itself; ONNX Runtime needs an
 * Android runtime context. The model invocation is verified on-device
 * via instrumented tests (Phase 1g benchmark gate).
 */
class SileroVadChunkerTest {

    @Test
    fun `chunker emits one full chunk after exactly 512 samples`() {
        val emitted = mutableListOf<Pair<Long, Int>>()
        val chunker = SileroVadStream.Chunker { startSample, samples ->
            emitted.add(startSample to samples.size)
        }
        chunker.feed(FloatArray(512), 512)
        assertEquals(1, emitted.size)
        assertEquals(0L, emitted[0].first)
        assertEquals(512, emitted[0].second)
    }

    @Test
    fun `chunker holds partial input and emits on completion`() {
        val emitted = mutableListOf<Pair<Long, Int>>()
        val chunker = SileroVadStream.Chunker { startSample, samples ->
            emitted.add(startSample to samples.size)
        }
        chunker.feed(FloatArray(300), 300)
        assertEquals(0, emitted.size)
        chunker.feed(FloatArray(212), 212)
        assertEquals(1, emitted.size)
        assertEquals(0L, emitted[0].first)
    }

    @Test
    fun `chunker emits multiple chunks from a long input`() {
        val emitted = mutableListOf<Pair<Long, Int>>()
        val chunker = SileroVadStream.Chunker { startSample, samples ->
            emitted.add(startSample to samples.size)
        }
        // 16000 samples (= 1 s @ 16 kHz) → 31 full chunks + 128 sample
        // residual. 31 × 512 = 15872.
        chunker.feed(FloatArray(16000), 16000)
        assertEquals(31, emitted.size)
        for ((i, e) in emitted.withIndex()) {
            assertEquals("chunk $i start sample", i * 512L, e.first)
            assertEquals(512, e.second)
        }
    }

    @Test
    fun `chunker tracks cumulative sample offsets across many small feeds`() {
        val emitted = mutableListOf<Long>()
        val chunker = SileroVadStream.Chunker { startSample, _ ->
            emitted.add(startSample)
        }
        // Feed 100 chunks of 320 samples (typical AudioRecord frame
        // size). Total 32000 samples → 62 full chunks (62 × 512 = 31744).
        val frame = FloatArray(320)
        repeat(100) { chunker.feed(frame, 320) }
        assertEquals(62, emitted.size)
        assertEquals(0L, emitted.first())
        assertEquals(62L * 512L - 512L, emitted.last())
    }

    @Test
    fun `reset clears partial buffer and resets sample counter`() {
        val emitted = mutableListOf<Pair<Long, Int>>()
        val chunker = SileroVadStream.Chunker { startSample, samples ->
            emitted.add(startSample to samples.size)
        }
        chunker.feed(FloatArray(300), 300)
        chunker.reset()
        chunker.feed(FloatArray(512), 512)
        assertEquals(1, emitted.size)
        assertEquals(0L, emitted[0].first)
    }

    @Test
    fun `each emitted chunk is a copy, safe to retain past callback`() {
        val emittedChunks = mutableListOf<FloatArray>()
        val chunker = SileroVadStream.Chunker { _, samples ->
            emittedChunks.add(samples)
        }
        val buf = FloatArray(512) { it.toFloat() }
        chunker.feed(buf, 512)
        // Mutate the source — the emitted chunk must NOT change.
        buf.fill(99f)
        assertTrue("emitted chunk was not copied",
            emittedChunks[0].any { it != 99f })
    }
}
