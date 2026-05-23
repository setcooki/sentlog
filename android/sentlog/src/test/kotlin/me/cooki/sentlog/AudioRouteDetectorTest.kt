package me.cooki.sentlog

import me.cooki.sentlog.internal.NativeBridge
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests for the suspicion-rank ordering encoded in
 * AudioRouteDetector. Validates the route constants match the C ABI
 * enum and the priorities are correctly ordered.
 *
 * The Android-specific device-type → route-code mapping is tested
 * on-device via instrumented tests (Phase 1g benchmark gate) — it
 * needs a real Context.
 */
class AudioRouteDetectorTest {

    @Test
    fun `route constants match sentlog_audio_route_t enum ordering`() {
        // Mirrors the enum in shared/include/sentlog/sentlog.h.
        assertEquals(0, NativeBridge.AUDIO_ROUTE_UNKNOWN)
        assertEquals(1, NativeBridge.AUDIO_ROUTE_BUILT_IN)
        assertEquals(2, NativeBridge.AUDIO_ROUTE_USB)
        assertEquals(3, NativeBridge.AUDIO_ROUTE_BLUETOOTH)
        assertEquals(4, NativeBridge.AUDIO_ROUTE_LOOPBACK)
    }

    @Test
    fun `built_in_mic is the most-trusted route`() {
        // BUILT_IN should be the route the C++ side maps to
        // audio_source = 1.0 — the only "trusted" route in the
        // current threat model.
        // The pipeline test for AudioSourceFromBuiltInMicIsOne in
        // shared/tests/pipeline_test.cpp pins the 1.0 mapping. Here
        // we just sanity-check the constant.
        assertEquals(1, NativeBridge.AUDIO_ROUTE_BUILT_IN)
    }

    @Test
    fun `usb and bluetooth are equally suspicious (same audio_source)`() {
        // The C++ pipeline maps both to 0.2 — they're equally
        // suspicious from a "device farm wiring" perspective. The
        // pipeline tests pin the exact value; here we just check the
        // constants differ (so both reach native).
        assertTrue(NativeBridge.AUDIO_ROUTE_USB != NativeBridge.AUDIO_ROUTE_BLUETOOTH)
    }
}
