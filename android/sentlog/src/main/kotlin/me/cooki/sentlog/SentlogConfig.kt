package me.cooki.sentlog

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

/**
 * SentlogConfig — passed once to [Sentlog.initialize].
 *
 * Mirrors the schema in `docs/technical-specification.md §3.3`. Every
 * default leaves the SDK inert (no transmission, no autonomous
 * behaviour). Capabilities are opt-in.
 *
 * When at least one [Forwarder] is configured, [issuer] becomes
 * mandatory — the C++ core rejects the config at init time otherwise.
 */
@Serializable
public data class SentlogConfig(
    // --- thresholds ---
    @SerialName("liveness_threshold")    val livenessThreshold:    Float = 0.7f,
    @SerialName("engagement_threshold")  val engagementThreshold:  Float = 0.5f,
    @SerialName("direction_threshold")   val directionThreshold:   Float = 0.6f,
    @SerialName("min_confidence")        val minConfidence:        Float = 0.6f,

    // --- pipeline ---
    @SerialName("challenge_enabled")     val challengeEnabled:     Boolean = true,
    @SerialName("language_hint")         val languageHint:         String  = "en",
    @SerialName("min_duration_ms")       val minDurationMs:        Int     = 5_000,
    @SerialName("max_duration_ms")       val maxDurationMs:        Int     = 60_000,
    @SerialName("target_phrase")         val targetPhrase:         String? = null,

    // --- multi-modal (all opt-in) ---
    @SerialName("use_motion_sensors")    val useMotionSensors:     Boolean = false,
    @SerialName("use_video_affect")      val useVideoAffect:       Boolean = false,
    @SerialName("use_device_integrity")  val useDeviceIntegrity:   Boolean = false,

    // --- evidence envelope (Verifiable Credentials) ---
    @SerialName("issuer")                       val issuer:                  String  = "",
    @SerialName("signing_key_id")               val signingKeyId:            String? = null,
    @SerialName("use_hardware_signing")         val useHardwareSigning:      Boolean = false,
    @SerialName("hardware_signing_required")    val hardwareSigningRequired: Boolean = false,

    // --- optional RFC 3161 TSA ---
    @SerialName("tsa_endpoint")          val tsaEndpoint:    String? = null,
    @SerialName("tsa_timeout_ms")        val tsaTimeoutMs:   Int     = 5_000,
    @SerialName("tsa_required")          val tsaRequired:    Boolean = false,

    // --- session metadata ---
    @SerialName("session_id")               val sessionId:              String?      = null,
    @SerialName("companion_evidence_refs")  val companionEvidenceRefs:  List<String> = emptyList(),

    // --- output pipeline ---
    @SerialName("forwarders")            val forwarders: List<Forwarder> = emptyList(),
)

@Serializable
public data class Forwarder(
    val name: String,
    val projection: Projection,
    @SerialName("sample_rate") val sampleRate: Float = 1.0f,
    val sink: JsonElement,                  // opaque to the SDK; sink kind/url etc. live in the JSON
    val retry: RetryPolicy = RetryPolicy(),
)

@Serializable
public enum class Projection {
    @SerialName("Full")                Full,
    @SerialName("Full+AudioClip")      FullWithAudioClip,
    @SerialName("ScoresOnly")          ScoresOnly,
    @SerialName("TranscriptOnly")      TranscriptOnly,
    @SerialName("EvidenceFingerprint") EvidenceFingerprint,
    @SerialName("EvidenceClip")        EvidenceClip,
    @SerialName("Custom")              Custom,
}

@Serializable
public data class RetryPolicy(
    @SerialName("max_attempts")     val maxAttempts:    Int = 3,
    @SerialName("initial_delay_ms") val initialDelayMs: Int = 1_000,
    @SerialName("max_delay_ms")     val maxDelayMs:     Int = 60_000,
    val backoff: Backoff = Backoff.Exponential,
)

@Serializable
public enum class Backoff {
    @SerialName("exponential") Exponential,
    @SerialName("linear")      Linear,
}
