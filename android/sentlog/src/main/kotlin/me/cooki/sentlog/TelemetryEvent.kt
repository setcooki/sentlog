package me.cooki.sentlog

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

/**
 * Telemetry events emitted by the SDK. Fired only when the integrator
 * supplied a callback at [Sentlog.initialize]. The set defined here is
 * exhaustive — the SDK never emits anything not listed.
 *
 * No event ever carries audio, transcript text, video frames, or any
 * user-identifying field.
 */
@Serializable
public sealed class TelemetryEvent {

    @Serializable
    @SerialName("PipelineTiming")
    public data class PipelineTiming(
        val stage:       String,
        @SerialName("duration_ms") val durationMs: Int,
    ) : TelemetryEvent()

    @Serializable
    @SerialName("ScoreHistogram")
    public data class ScoreHistogram(
        val field:  String,
        val bucket: String,
    ) : TelemetryEvent()

    @Serializable
    @SerialName("ForwarderOutcome")
    public data class ForwarderOutcome(
        @SerialName("forwarder_name") val forwarderName: String,
        val outcome:  Outcome,
        val attempts: Int,
    ) : TelemetryEvent() {
        @Serializable
        public enum class Outcome {
            @SerialName("ok")      Ok,
            @SerialName("retry")   Retry,
            @SerialName("dropped") Dropped,
        }
    }

    @Serializable
    @SerialName("ManifestVerifyFailed")
    public data class ManifestVerifyFailed(
        val reason:               String,
        @SerialName("attempted_version") val attemptedVersion: String,
    ) : TelemetryEvent()

    @Serializable
    @SerialName("ModelLoadError")
    public data class ModelLoadError(
        val model:  String,
        val reason: String,
    ) : TelemetryEvent()

    @Serializable
    @SerialName("PermissionDenied")
    public data class PermissionDenied(
        val permission: Permission,
    ) : TelemetryEvent() {
        @Serializable
        public enum class Permission {
            @SerialName("microphone") Microphone,
            @SerialName("motion")     Motion,
            @SerialName("camera")     Camera,
        }
    }

    @Serializable
    @SerialName("CaptureAborted")
    public data class CaptureAborted(
        val reason: String,
    ) : TelemetryEvent()

    @Serializable
    @SerialName("HardwareSigningUnavailable")
    public data class HardwareSigningUnavailable(
        val reason: String,
        @SerialName("fell_back_to") val fellBackTo: String,
    ) : TelemetryEvent()

    @Serializable
    @SerialName("TsaRequestFailed")
    public data class TsaRequestFailed(
        @SerialName("tsa_url") val tsaUrl: String,
        val reason: String,
    ) : TelemetryEvent()
}
