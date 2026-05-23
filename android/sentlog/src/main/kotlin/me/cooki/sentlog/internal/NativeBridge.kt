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
    @JvmStatic external fun nativeSetAudioRoute(handle: Long, route: Int): Int
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
