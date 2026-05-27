package me.cooki.sentlog.internal

/**
 * Thin JNI surface. Every function here maps 1:1 to a C ABI entry in
 * `shared/include/sentlog/sentlog.h`. Status codes mirror the
 * `sentlog_status_t` enum.
 *
 * This class is intentionally not part of the public API. Integrators
 * use [me.cooki.sentlog.Sentlog].
 */
internal object NativeBridge {

    init {
        System.loadLibrary("sentlog_jni")
    }

    /** No-op call other internal modules use to force the JNI library
     *  to load before they invoke their own external functions (e.g.
     *  WhisperRunner declares native methods in the same .so). */
    fun ensureLoaded() = Unit

    // Status code constants — keep in sync with sentlog_status_t.
    const val STATUS_OK                    = 0
    const val STATUS_ERR_INVALID_CONFIG    = 1
    const val STATUS_ERR_INVALID_STATE     = 2
    const val STATUS_ERR_PERMISSION_DENIED = 3
    const val STATUS_ERR_CAPTURE_FAILED    = 4
    const val STATUS_ERR_MODEL_LOAD        = 5
    const val STATUS_ERR_MANIFEST_VERIFY   = 6
    const val STATUS_ERR_CANCELLED         = 7
    const val STATUS_ERR_HARDWARE_SIGNING  = 8
    const val STATUS_ERR_TSA               = 9
    const val STATUS_ERR_INTERNAL          = 99

    @JvmStatic external fun nativeVersion(): String

    /**
     * Create an SDK handle. Returns the opaque handle as a Long (0 on
     * failure; [statusOut[0]] holds the status code).
     */
    @JvmStatic external fun nativeCreate(
        configJson: String,
        telemetryCb: TelemetryCallback?,
        statusOut: IntArray,
    ): Long

    @JvmStatic external fun nativeDestroy(handle: Long)
    @JvmStatic external fun nativeStartCapture(handle: Long): Int
    @JvmStatic external fun nativePushAudioFrame(handle: Long, samples: FloatArray, numSamples: Int): Int
    @JvmStatic external fun nativePushVadFrame(handle: Long, startSample: Long, numSamples: Int, probability: Float): Int
    @JvmStatic external fun nativePushLivenessResult(handle: Long, score: Float, replayRisk: Float, syntheticRisk: Float): Int
    @JvmStatic external fun nativePushTranscript(handle: Long, transcriptJson: String): Int
    @JvmStatic external fun nativePushMotionFrame(
        handle: Long,
        timestampNs: Long,
        ax: Float, ay: Float, az: Float,
        gx: Float, gy: Float, gz: Float,
    ): Int
    @JvmStatic external fun nativeSetAudioRoute(handle: Long, route: Int): Int
    /** Non-destructive snapshot. Returns the number of samples written into [out]. */
    @JvmStatic external fun nativeSnapshotAudio(handle: Long, out: FloatArray): Int
    @JvmStatic external fun nativeEvaluate(handle: Long, resultCb: ResultCallback): Int

    // Mirrors sentlog_audio_route_t in sentlog.h.
    const val AUDIO_ROUTE_UNKNOWN   = 0
    const val AUDIO_ROUTE_BUILT_IN  = 1
    const val AUDIO_ROUTE_USB       = 2
    const val AUDIO_ROUTE_BLUETOOTH = 3
    const val AUDIO_ROUTE_LOOPBACK  = 4
    @JvmStatic external fun nativeCancel(handle: Long)
    @JvmStatic external fun nativeGetPermissions(handle: Long): String?
    @JvmStatic external fun nativeGetCapabilities(handle: Long): String?
    @JvmStatic external fun nativeModelVersions(handle: Long): String?
    @JvmStatic external fun nativeCheckForUpdates(handle: Long, updateCb: UpdateCallback): Int

    /** Phase 4i: drain a local-queue NDJSON file at [path]. Returns
     *  the number of envelopes delivered to [cb]. The cb returns
     *  true to delete the envelope from the queue, false to keep
     *  it. Handle may be 0 — drain is file-only and doesn't need a
     *  live SDK. */
    @JvmStatic external fun nativeDrainQueue(
        handle: Long, path: String, cb: DrainCallback,
    ): Int

    /** Phase 4d: install (or replace, when [secretKey] != null) the
     *  Ed25519 secret key the SDK uses to sign envelopes. The key is
     *  the 64-byte RFC 8032 seed||pubkey concatenation; obtain it
     *  from libsodium / NaCl / a hardware-backed keystore. */
    @JvmStatic external fun nativeSetSigningKey(
        handle: Long, secretKey: ByteArray, verificationMethod: String,
    ): Int

    /** Phase 4d: install (or clear, when [dispatcher] == null) the
     *  per-handle envelope dispatcher invoked once per non-local-queue
     *  forwarder after evaluate. */
    @JvmStatic external fun nativeSetEnvelopeDispatcher(
        handle: Long, dispatcher: EnvelopeDispatcher?,
    ): Int
}

/** Invoked by JNI on the SDK's I/O thread. */
internal abstract class TelemetryCallback {
    abstract fun onTelemetry(eventJson: String?)
}

/** Invoked by JNI once per [NativeBridge.nativeEvaluate] call. */
internal abstract class ResultCallback {
    abstract fun onResult(status: Int, resultJson: String?)
}

/** Invoked by JNI once per [NativeBridge.nativeCheckForUpdates] call. */
internal abstract class UpdateCallback {
    abstract fun onUpdate(status: Int, updateJson: String?)
}

/** Invoked by JNI once per envelope inside [NativeBridge.nativeDrainQueue].
 *  Return true to delete the envelope from the queue, false to keep
 *  it for a later drain. */
internal abstract class DrainCallback {
    abstract fun onEnvelope(envelopeJson: String): Boolean
}

/** Invoked by JNI once per non-local-queue forwarder after evaluate.
 *  Long-lived: the SDK never releases the GlobalRef so the dispatcher
 *  remains callable until [NativeBridge.nativeSetEnvelopeDispatcher]
 *  is invoked with null. */
internal abstract class EnvelopeDispatcher {
    abstract fun onEnvelope(
        forwarderName: String,
        sinkKind: String,
        sinkJson: String,
        envelopeJson: String,
    )
}
