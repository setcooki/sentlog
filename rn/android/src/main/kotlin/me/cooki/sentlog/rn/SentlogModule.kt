package me.cooki.sentlog.rn

import com.facebook.react.bridge.Arguments
import com.facebook.react.bridge.Promise
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.bridge.ReactContextBaseJavaModule
import com.facebook.react.bridge.ReactMethod
import com.facebook.react.modules.core.DeviceEventManagerModule
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.serialization.json.Json
import me.cooki.sentlog.Sentlog
import me.cooki.sentlog.SentlogConfig
import java.util.concurrent.ConcurrentHashMap

/**
 * React Native bridge over the underlying `me.cooki.sentlog.Sentlog`
 * singleton. Every method matches the [SentlogBridge] interface in
 * the JS-side `rn/src/types.ts`; the wire format is JSON strings for
 * the result/config types (the JS shim does `JSON.parse`).
 *
 * Asynchronous methods (evaluate, checkForUpdates, drainQueueOnce,
 * setEnvelopeDispatcher) hand work to a per-module coroutine scope so
 * the JS bridge thread is never blocked.
 *
 * Events:
 *   - `SentlogEnvelope` fires once per non-local-queue forwarder after
 *     `evaluate` while [setEnvelopeDispatcherActive] is true.
 *   - `SentlogDrainEnvelope` fires once per envelope during
 *     [drainQueueOnce]; the JS side replies via [drainQueueReply] to
 *     drive the keep-vs-delete decision.
 *   - `SentlogTelemetry` fires per native telemetry event when the
 *     integrator opted in at initialize (a future config flag).
 */
class SentlogModule(private val reactContext: ReactApplicationContext)
    : ReactContextBaseJavaModule(reactContext) {

    override fun getName(): String = "Sentlog"

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val json  = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

    // Drain reply coordination — see drainQueueOnce / drainQueueReply.
    private data class DrainReply(val keep: Boolean)
    private val drainReplies = ConcurrentHashMap<String, ConcurrentHashMap<Int, DrainReply>>()
    private val drainMutex = Mutex()  // serialises drain calls per process

    // --- Lifecycle -----------------------------------------------

    @ReactMethod
    fun version(promise: Promise) {
        runCatching { promise.resolve(Sentlog.version) }
            .onFailure { promise.reject("version", it) }
    }

    @ReactMethod
    fun initialize(configJson: String, promise: Promise) {
        scope.launch {
            try {
                val cfg = json.decodeFromString(SentlogConfig.serializer(), configJson)
                Sentlog.initialize(cfg, reactContext.applicationContext) { ev ->
                    // Telemetry forwarded as JSON; JS side parses.
                    val encoded = json.encodeToString(
                        me.cooki.sentlog.TelemetryEvent.serializer(), ev)
                    sendEvent("SentlogTelemetry", encoded)
                }
                promise.resolve(null)
            } catch (t: Throwable) {
                promise.reject("initialize", t.message, t)
            }
        }
    }

    @ReactMethod
    fun startCapture(promise: Promise) {
        runCatching { Sentlog.startCapture(); promise.resolve(null) }
            .onFailure { promise.reject("startCapture", it.message, it) }
    }

    @ReactMethod
    fun evaluate(promise: Promise) {
        scope.launch {
            try {
                val res = Sentlog.evaluate()
                promise.resolve(json.encodeToString(
                    me.cooki.sentlog.SentlogResult.serializer(), res))
            } catch (t: Throwable) {
                promise.reject("evaluate", t.message, t)
            }
        }
    }

    @ReactMethod
    fun cancel(promise: Promise) {
        runCatching { Sentlog.cancel(); promise.resolve(null) }
            .onFailure { promise.reject("cancel", it.message, it) }
    }

    @ReactMethod
    fun destroy(promise: Promise) {
        // setEnvelopeDispatcher(null) throws when no handle is live;
        // destroy is documented as idempotent (SentlogBridge.destroy)
        // so swallow the dispatcher-clear failure if Sentlog is
        // already destroyed.
        runCatching { Sentlog.setEnvelopeDispatcher(null) }
        runCatching {
            Sentlog.destroy()
            promise.resolve(null)
        }.onFailure { promise.reject("destroy", it.message, it) }
    }

    // --- Getters (JSON strings; JS side parses) ------------------

    @ReactMethod
    fun getPermissions(promise: Promise) {
        runCatching {
            promise.resolve(json.encodeToString(
                me.cooki.sentlog.Permissions.serializer(),
                Sentlog.getPermissions()))
        }.onFailure { promise.reject("getPermissions", it.message, it) }
    }

    @ReactMethod
    fun getCapabilities(promise: Promise) {
        runCatching {
            promise.resolve(json.encodeToString(
                me.cooki.sentlog.Capabilities.serializer(),
                Sentlog.getCapabilities()))
        }.onFailure { promise.reject("getCapabilities", it.message, it) }
    }

    @ReactMethod
    fun modelVersions(promise: Promise) {
        runCatching {
            promise.resolve(json.encodeToString(
                me.cooki.sentlog.ModelVersions.serializer(),
                Sentlog.modelVersions()))
        }.onFailure { promise.reject("modelVersions", it.message, it) }
    }

    @ReactMethod
    fun pushFaceFrame(
        timestampNs: Double,
        valence: Float,
        arousal: Float,
        dominantFauCsv: String?,
        promise: Promise,
    ) {
        try {
            Sentlog.pushFaceFrame(
                timestampNs.toLong(),
                valence, arousal, dominantFauCsv)
            promise.resolve(null)
        } catch (t: Throwable) {
            promise.reject("pushFaceFrame", t.message, t)
        }
    }

    @ReactMethod
    fun setDeviceAttestation(
        tokenBase64: String?,
        attestationType: String?,
        promise: Promise,
    ) {
        val bytes = if (tokenBase64 != null) {
            try {
                decodeBase64Strict(tokenBase64)
            } catch (e: IllegalArgumentException) {
                return promise.reject("setDeviceAttestation",
                    "tokenBase64 not valid base64", e)
            }
        } else null
        try {
            Sentlog.setDeviceAttestation(bytes, attestationType)
            promise.resolve(null)
        } catch (t: Throwable) {
            promise.reject("setDeviceAttestation", t.message, t)
        }
    }

    @ReactMethod
    fun checkForUpdates(manifestJson: String, promise: Promise) {
        scope.launch {
            try {
                val r = Sentlog.checkForUpdates(manifestJson)
                promise.resolve(json.encodeToString(
                    me.cooki.sentlog.UpdateResult.serializer(), r))
            } catch (t: Throwable) {
                promise.reject("checkForUpdates", t.message, t)
            }
        }
    }

    // --- Signing + dispatcher ------------------------------------

    @ReactMethod
    fun setSigningKey(secretKeyBase64: String,
                      verificationMethod: String,
                      promise: Promise) {
        runCatching {
            val bytes = decodeBase64Strict(secretKeyBase64)
            Sentlog.setSigningKey(bytes, verificationMethod)
            promise.resolve(null)
        }.onFailure { promise.reject("setSigningKey", it.message, it) }
    }

    @ReactMethod
    fun setEnvelopeDispatcherActive(active: Boolean, promise: Promise) {
        runCatching {
            if (active) {
                Sentlog.setEnvelopeDispatcher { name, kind, sj, ej ->
                    val map = Arguments.createMap().apply {
                        putString("forwarder_name", name)
                        putString("sink_kind",      kind)
                        putString("sink_json",      sj)
                        putString("envelope_json",  ej)
                    }
                    sendEventMap("SentlogEnvelope", map)
                }
            } else {
                Sentlog.setEnvelopeDispatcher(null)
            }
            promise.resolve(null)
        }.onFailure { promise.reject("setEnvelopeDispatcherActive", it.message, it) }
    }

    // --- drainQueue: event-bus protocol --------------------------

    @ReactMethod
    fun drainQueueOnce(path: String, drainRequestId: String, promise: Promise) {
        scope.launch {
            drainMutex.withLock {
                val replies = ConcurrentHashMap<Int, DrainReply>()
                drainReplies[drainRequestId] = replies
                try {
                    var lineIdx = 0
                    val delivered = Sentlog.drainQueue(path) { envelopeJson ->
                        val idx = lineIdx++
                        val map = Arguments.createMap().apply {
                            putString("drainRequestId", drainRequestId)
                            putInt("line",              idx)
                            putString("envelope_json",  envelopeJson)
                        }
                        sendEventMap("SentlogDrainEnvelope", map)
                        // Spin-wait for the JS reply. Bounded by the
                        // per-envelope handler latency on the JS side.
                        // The wait is cheap because drain calls are
                        // rare (app-foreground sync, not a hot path).
                        val deadline = System.currentTimeMillis() + 30_000L
                        while (replies[idx] == null) {
                            if (System.currentTimeMillis() > deadline) {
                                // Treat as "keep" on timeout — safer than dropping.
                                return@drainQueue false
                            }
                            Thread.sleep(5)
                        }
                        replies.remove(idx)!!.keep.not()  // C ABI: true=delete
                    }
                    promise.resolve(delivered)
                } catch (t: Throwable) {
                    promise.reject("drainQueueOnce", t.message, t)
                } finally {
                    drainReplies.remove(drainRequestId)
                }
            }
        }
    }

    @ReactMethod
    fun drainQueueReply(drainRequestId: String,
                        line: Int,
                        keep: Boolean,
                        promise: Promise) {
        drainReplies[drainRequestId]?.put(line, DrainReply(keep))
        promise.resolve(null)
    }

    // --- Event emitter helpers -----------------------------------
    // android.util.Base64 skips invalid characters, so "@@@@" would decode
    // to nothing and silently clear an attestation.
    private fun decodeBase64Strict(text: String): ByteArray {
        require(text.isNotEmpty() && text.length % 4 == 0 &&
                Regex("^[A-Za-z0-9+/]+={0,2}$").matches(text)) { "not valid base64" }
        return android.util.Base64.decode(text, android.util.Base64.NO_WRAP)
    }


    private fun sendEvent(eventName: String, payload: String) {
        reactContext
            .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
            .emit(eventName, payload)
    }

    private fun sendEventMap(eventName: String, payload: com.facebook.react.bridge.WritableMap) {
        reactContext
            .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter::class.java)
            .emit(eventName, payload)
    }

    // RN ≥0.65 wants these no-op methods on event-emitting modules.
    @ReactMethod fun addListener(eventName: String) = Unit
    @ReactMethod fun removeListeners(count: Int) = Unit
}
