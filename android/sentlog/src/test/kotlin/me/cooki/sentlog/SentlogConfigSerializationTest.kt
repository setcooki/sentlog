package me.cooki.sentlog

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Round-trip tests for the SentlogConfig schema. The C++ core parses
 * the JSON the Kotlin layer emits; if the field names drift, native
 * init fails with INVALID_CONFIG. These tests pin the schema.
 */
class SentlogConfigSerializationTest {

    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
        explicitNulls = false
    }

    @Test
    fun `default config serialises with snake_case field names`() {
        val cfg = SentlogConfig()
        val serialised = json.encodeToString(SentlogConfig.serializer(), cfg)
        val parsed = json.parseToJsonElement(serialised) as JsonObject

        // Check snake_case schema markers that native depends on.
        assertTrue("liveness_threshold present", parsed.containsKey("liveness_threshold"))
        assertTrue("engagement_threshold present", parsed.containsKey("engagement_threshold"))
        assertTrue("direction_threshold present", parsed.containsKey("direction_threshold"))
        assertTrue("min_confidence present", parsed.containsKey("min_confidence"))
        assertTrue("challenge_enabled present", parsed.containsKey("challenge_enabled"))
        assertTrue("language_hint present", parsed.containsKey("language_hint"))
        assertTrue("min_duration_ms present", parsed.containsKey("min_duration_ms"))
        assertTrue("max_duration_ms present", parsed.containsKey("max_duration_ms"))
        assertTrue("use_motion_sensors present", parsed.containsKey("use_motion_sensors"))
        assertTrue("use_video_affect present", parsed.containsKey("use_video_affect"))
        assertTrue("use_device_integrity present", parsed.containsKey("use_device_integrity"))
        assertTrue("use_hardware_signing present", parsed.containsKey("use_hardware_signing"))
        assertTrue("hardware_signing_required present", parsed.containsKey("hardware_signing_required"))
        assertTrue("tsa_required present", parsed.containsKey("tsa_required"))
        assertTrue("companion_evidence_refs present", parsed.containsKey("companion_evidence_refs"))
        assertTrue("forwarders present", parsed.containsKey("forwarders"))
    }

    @Test
    fun `default thresholds match spec`() {
        val cfg = SentlogConfig()
        assertEquals(0.7f, cfg.livenessThreshold)
        assertEquals(0.5f, cfg.engagementThreshold)
        assertEquals(0.6f, cfg.directionThreshold)
        assertEquals(0.6f, cfg.minConfidence)
    }

    @Test
    fun `forwarder projection enum serialises with PascalCase plus operator`() {
        val cfg = SentlogConfig(
            issuer = "did:example:integrator",
            forwarders = listOf(
                Forwarder(
                    name = "audit",
                    projection = Projection.EvidenceFingerprint,
                    sink = buildJsonObject {
                        put("kind", "http")
                        put("url", "https://example.com/audit")
                    },
                ),
            ),
        )
        val serialised = json.encodeToString(SentlogConfig.serializer(), cfg)
        assertTrue("EvidenceFingerprint name preserved",
            serialised.contains("\"projection\":\"EvidenceFingerprint\""))
    }

    @Test
    fun `round trip preserves all fields`() {
        val cfg = SentlogConfig(
            livenessThreshold = 0.75f,
            engagementThreshold = 0.55f,
            issuer = "did:example:integrator",
            sessionId = "session-123",
            useMotionSensors = true,
            companionEvidenceRefs = listOf("ref-1", "ref-2"),
        )
        val serialised = json.encodeToString(SentlogConfig.serializer(), cfg)
        val deserialised = json.decodeFromString(SentlogConfig.serializer(), serialised)
        assertEquals(cfg, deserialised)
    }
}
