package me.cooki.sentlog

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test

/**
 * Validates that the SentlogResult Kotlin model deserialises the
 * exact JSON shape the C++ core emits. The reference JSON below
 * matches `docs/technical-specification.md §3.2` field by field.
 */
class SentlogResultDeserializationTest {

    private val json = Json {
        ignoreUnknownKeys = true
        explicitNulls = false
    }

    @Test
    fun `full result deserialises`() {
        val refJson = """
        {
          "schema_version": "1",
          "transcript": {
            "text": "hello world",
            "language": "en",
            "confidence": 0.92,
            "word_timings": [
              {"word": "hello", "start_ms": 100, "end_ms": 500},
              {"word": "world", "start_ms": 600, "end_ms": 1000}
            ],
            "target_match": 0.95
          },
          "liveness": {
            "score": 0.88,
            "replay_risk": 0.05,
            "synthetic_risk": 0.07
          },
          "presence": {
            "summary": 0.9,
            "audio_source": 1.0,
            "motion": 0.85,
            "integrity": null
          },
          "direction": 0.91,
          "confidence": 0.87,
          "thresholds_met": true,
          "engagement": {
            "score": 0.82,
            "voice_score": 0.82,
            "face_score": null,
            "model_status": "training"
          },
          "affect": null,
          "fingerprint": {
            "algo": "chromaprint-v2",
            "bytes": "Zm9vYmFyYmF6"
          },
          "model_versions": {
            "liveness": "0.0.0-phase1",
            "asr": "0.0.0-phase1",
            "prosody": "0.0.0-phase1",
            "vad": "0.0.0-phase1",
            "manifest": "0.0.0-phase1"
          },
          "diagnostics": {
            "duration_ms": 8500,
            "audio_quality": "moderate"
          },
          "evidence_grade": "DeviceUnattested",
          "extensions": {}
        }
        """.trimIndent()

        val result = json.decodeFromString(SentlogResult.serializer(), refJson)
        assertEquals("1", result.schemaVersion)
        assertEquals("hello world", result.transcript.text)
        assertEquals(2, result.transcript.wordTimings?.size)
        assertEquals(EvidenceGrade.DeviceUnattested, result.evidenceGrade)
        assertEquals(EngagementModelStatus.Training, result.engagement.modelStatus)
        assertEquals(AudioQuality.Moderate, result.diagnostics.audioQuality)
        assertEquals("chromaprint-v2", result.fingerprint.algo)
        assertNotNull(result.presence.motion)
    }

    @Test
    fun `unknown fields are ignored`() {
        val refJson = """
        {
          "schema_version": "1",
          "transcript": {"text": "x", "language": "en", "confidence": 0.5},
          "liveness": {"score": 0.5, "replay_risk": 0.5, "synthetic_risk": 0.5},
          "presence": {"summary": 0.5, "audio_source": 1.0},
          "direction": 0.5,
          "confidence": 0.5,
          "thresholds_met": false,
          "engagement": {"score": 0.5, "voice_score": 0.5, "model_status": "placeholder"},
          "fingerprint": {"algo": "chromaprint-v2", "bytes": ""},
          "model_versions": {"liveness": "x", "asr": "x", "prosody": "x", "vad": "x", "manifest": "x"},
          "diagnostics": {"duration_ms": 0, "audio_quality": "quiet"},
          "evidence_grade": "DeviceUnattested",
          "extensions": {},
          "future_field_we_dont_know_about": 42
        }
        """.trimIndent()
        // Must not throw.
        json.decodeFromString(SentlogResult.serializer(), refJson)
    }

    @Test
    fun `permissions deserialise with snake_case state names`() {
        val refJson = """
        {"microphone": "granted", "motion": "not_requested", "camera": "not_requested"}
        """.trimIndent()
        val p = json.decodeFromString(Permissions.serializer(), refJson)
        assertEquals(PermissionState.Granted, p.microphone)
        assertEquals(PermissionState.NotRequested, p.motion)
        assertEquals(PermissionState.NotRequested, p.camera)
    }

    @Test
    fun `capabilities deserialise with all runtime variants`() {
        for (runtime in listOf("native", "capacitor", "react_native", "web_fallback")) {
            val refJson = """
            {
              "audio_capture": true, "motion_sensors": false, "video_face_mesh": false,
              "device_integrity_check": false, "ota_updates": true, "local_queue": true,
              "runtime": "$runtime"
            }
            """.trimIndent()
            val c = json.decodeFromString(Capabilities.serializer(), refJson)
            assertNotNull(c.runtime)
        }
    }
}
