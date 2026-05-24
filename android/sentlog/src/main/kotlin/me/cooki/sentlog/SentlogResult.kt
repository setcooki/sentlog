package me.cooki.sentlog

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

/**
 * The result returned by [Sentlog.evaluate]. Mirrors the schema in
 * `docs/technical-specification.md §3.2`.
 *
 * All score fields are raw [0,1] probability outputs. The SDK applies
 * no calibration or capping. [thresholdsMet] is the only field that
 * consults the threshold values in [SentlogConfig].
 */
@Serializable
public data class SentlogResult(
    @SerialName("schema_version")  val schemaVersion:  String,
    val transcript:                 Transcript,
    val liveness:                   Liveness,
    val presence:                   Presence,
    val direction:                  Float,
    val confidence:                 Float,
    @SerialName("thresholds_met")  val thresholdsMet:  Boolean,
    val engagement:                 Engagement,
    val affect:                     Affect? = null,
    val fingerprint:                AudioFingerprint,
    @SerialName("model_versions")  val modelVersions:  ModelVersions,
    val diagnostics:                Diagnostics,
    @SerialName("evidence_grade")  val evidenceGrade:  EvidenceGrade,
    val extensions:                 Map<String, JsonElement> = emptyMap(),
)

@Serializable
public enum class EvidenceGrade {
    @SerialName("DeviceUnattested") DeviceUnattested,
    @SerialName("DeviceAttested")   DeviceAttested,
    @SerialName("EnclaveSigned")    EnclaveSigned,
}

@Serializable
public data class Transcript(
    val text:        String,
    val language:    String,
    val confidence:  Float,
    @SerialName("word_timings")  val wordTimings:  List<WordTiming>? = null,
    @SerialName("target_match")  val targetMatch:  Float? = null,
)

@Serializable
public data class WordTiming(
    val word:     String,
    @SerialName("start_ms") val startMs: Int,
    @SerialName("end_ms")   val endMs:   Int,
)

@Serializable
public data class Liveness(
    val score:          Float,
    @SerialName("replay_risk")    val replayRisk:    Float,
    @SerialName("synthetic_risk") val syntheticRisk: Float,
)

@Serializable
public data class Presence(
    val summary:      Float,
    @SerialName("audio_source") val audioSource: Float,
    val motion:       Float? = null,
    val integrity:    Float? = null,
)

@Serializable
public data class Engagement(
    val score:        Float,
    @SerialName("voice_score") val voiceScore: Float,
    @SerialName("face_score")  val faceScore:  Float? = null,
    @SerialName("model_status") val modelStatus: EngagementModelStatus,
)

@Serializable
public enum class EngagementModelStatus {
    @SerialName("placeholder") Placeholder,
    @SerialName("training")    Training,
    @SerialName("trained")     Trained,
}

@Serializable
public data class Affect(
    val valence:  Float,
    val arousal:  Float,
    @SerialName("dominant_fau") val dominantFau: List<String>? = null,
)

@Serializable
public data class AudioFingerprint(
    val algo:  String,
    val bytes: String,   // base64 of the raw fingerprint bytes
)

@Serializable
public data class Diagnostics(
    @SerialName("failure_reason")     val failureReason:     FailureReason? = null,
    @SerialName("snr_db")             val snrDb:             Float? = null,
    @SerialName("duration_ms")        val durationMs:        Int,
    @SerialName("audio_quality")      val audioQuality:      AudioQuality,
    @SerialName("detected_language")  val detectedLanguage:  String? = null,
    @SerialName("language_mismatch")  val languageMismatch:  Boolean? = null,
)

@Serializable
public enum class FailureReason {
    @SerialName("TooShort")          TooShort,
    @SerialName("TooNoisy")          TooNoisy,
    @SerialName("NoSpeechDetected")  NoSpeechDetected,
    @SerialName("LanguageMismatch")  LanguageMismatch,
    @SerialName("Interrupted")       Interrupted,
    @SerialName("InternalError")     InternalError,
}

@Serializable
public enum class AudioQuality {
    @SerialName("quiet")      Quiet,
    @SerialName("moderate")   Moderate,
    @SerialName("noisy")      Noisy,
    @SerialName("very_noisy") VeryNoisy,
}

@Serializable
public data class ModelVersions(
    val liveness: String,
    val asr:      String,
    val prosody:  String,
    val vad:      String,
    val affect:   String? = null,
    val manifest: String,
)

@Serializable
public data class UpdateResult(
    @SerialName("checked_at_ms")    val checkedAtMs:    Long,
    @SerialName("update_found")     val updateFound:    Boolean,
    @SerialName("previous_version") val previousVersion: String,
    @SerialName("new_version")      val newVersion:     String? = null,
    val applied: Boolean,
    val error:   String? = null,
)

@Serializable
public data class Permissions(
    val microphone:  PermissionState,
    val motion:      PermissionState,
    val camera:      PermissionState,
)

@Serializable
public enum class PermissionState {
    @SerialName("granted")        Granted,
    @SerialName("denied")         Denied,
    @SerialName("prompt")         Prompt,
    @SerialName("not_requested")  NotRequested,
}

@Serializable
public data class Capabilities(
    @SerialName("audio_capture")          val audioCapture:         Boolean,
    @SerialName("motion_sensors")         val motionSensors:        Boolean,
    @SerialName("video_face_mesh")        val videoFaceMesh:        Boolean,
    @SerialName("device_integrity_check") val deviceIntegrityCheck: Boolean,
    @SerialName("ota_updates")            val otaUpdates:           Boolean,
    @SerialName("local_queue")            val localQueue:           Boolean,
    val runtime: Runtime,
)

@Serializable
public enum class Runtime {
    @SerialName("native")        Native,
    @SerialName("capacitor")     Capacitor,
    @SerialName("react_native")  ReactNative,
    @SerialName("web_fallback")  WebFallback,
}
