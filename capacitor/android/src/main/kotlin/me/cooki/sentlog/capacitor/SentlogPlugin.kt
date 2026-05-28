package me.cooki.sentlog.capacitor

import android.util.Base64
import com.getcapacitor.JSObject
import com.getcapacitor.Plugin
import com.getcapacitor.PluginCall
import com.getcapacitor.PluginMethod
import com.getcapacitor.annotation.CapacitorPlugin
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
 * Capacitor plugin over the underlying `me.cooki.sentlog.Sentlog`
 * singleton. Method names + IPC shape match the JS shim in
 * capacitor/src/sentlog.ts; the JS side calls each method with an
 * object argument and parses the JSON strings the plugin returns
 * for typed fields.
 *
 * Events ride Capacitor's `notifyListeners`:
 *   - `SentlogEnvelope`        — per-forwarder envelope.
 *   - `SentlogDrainEnvelope`   — per-envelope inside drainQueueOnce.
 *   - `SentlogTelemetry`       — per native telemetry event.
 */
@CapacitorPlugin(name = "Sentlog")
class SentlogPlugin : Plugin() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val json  = Json {
        ignoreUnknownKeys = true
        encodeDefaults    = true
        explicitNulls     = false
    }

    private data class DrainReply(val keep: Boolean)
    private val drainReplies = ConcurrentHashMap<String, ConcurrentHashMap<Int, DrainReply>>()
    private val drainMutex = Mutex()

    @PluginMethod
    fun version(call: PluginCall) {
        val ret = JSObject()
        ret.put("value", Sentlog.version)
        call.resolve(ret)
    }

    @PluginMethod
    fun initialize(call: PluginCall) {
        val configJson = call.getString("configJson")
            ?: return call.reject("configJson required")
        scope.launch {
            try {
                val cfg = json.decodeFromString(SentlogConfig.serializer(), configJson)
                Sentlog.initialize(cfg, context.applicationContext) { ev ->
                    val encoded = json.encodeToString(
                        me.cooki.sentlog.TelemetryEvent.serializer(), ev)
                    notifyListeners("SentlogTelemetry",
                        JSObject().put("event", encoded))
                }
                call.resolve()
            } catch (t: Throwable) {
                call.reject(t.message ?: "initialize failed", t)
            }
        }
    }

    @PluginMethod
    fun startCapture(call: PluginCall) {
        try { Sentlog.startCapture(); call.resolve() }
        catch (t: Throwable) { call.reject(t.message ?: "startCapture failed", t) }
    }

    @PluginMethod
    fun evaluate(call: PluginCall) {
        scope.launch {
            try {
                val r = Sentlog.evaluate()
                val ret = JSObject().put("resultJson",
                    json.encodeToString(
                        me.cooki.sentlog.SentlogResult.serializer(), r))
                call.resolve(ret)
            } catch (t: Throwable) {
                call.reject(t.message ?: "evaluate failed", t)
            }
        }
    }

    @PluginMethod
    fun cancel(call: PluginCall) {
        Sentlog.cancel()
        call.resolve()
    }

    @PluginMethod
    fun destroy(call: PluginCall) {
        // setEnvelopeDispatcher(null) throws when no handle is live;
        // destroy is documented as idempotent (SentlogBridge.destroy)
        // so swallow the dispatcher-clear failure if Sentlog is
        // already destroyed.
        runCatching { Sentlog.setEnvelopeDispatcher(null) }
        try {
            Sentlog.destroy()
            call.resolve()
        } catch (t: Throwable) {
            call.reject(t.message ?: "destroy failed", t)
        }
    }

    @PluginMethod
    fun getPermissions(call: PluginCall) {
        try {
            val p = Sentlog.getPermissions()
            call.resolve(JSObject().put("json",
                json.encodeToString(me.cooki.sentlog.Permissions.serializer(), p)))
        } catch (t: Throwable) { call.reject(t.message ?: "getPermissions failed", t) }
    }

    @PluginMethod
    fun getCapabilities(call: PluginCall) {
        try {
            val c = Sentlog.getCapabilities()
            call.resolve(JSObject().put("json",
                json.encodeToString(me.cooki.sentlog.Capabilities.serializer(), c)))
        } catch (t: Throwable) { call.reject(t.message ?: "getCapabilities failed", t) }
    }

    @PluginMethod
    fun modelVersions(call: PluginCall) {
        try {
            val m = Sentlog.modelVersions()
            call.resolve(JSObject().put("json",
                json.encodeToString(me.cooki.sentlog.ModelVersions.serializer(), m)))
        } catch (t: Throwable) { call.reject(t.message ?: "modelVersions failed", t) }
    }

    @PluginMethod
    fun checkForUpdates(call: PluginCall) {
        val manifestJson = call.getString("manifestJson")
            ?: return call.reject("manifestJson required")
        scope.launch {
            try {
                val u = Sentlog.checkForUpdates(manifestJson)
                call.resolve(JSObject().put("json",
                    json.encodeToString(me.cooki.sentlog.UpdateResult.serializer(), u)))
            } catch (t: Throwable) {
                call.reject(t.message ?: "checkForUpdates failed", t)
            }
        }
    }

    @PluginMethod
    fun setSigningKey(call: PluginCall) {
        val sk  = call.getString("secretKeyBase64")
            ?: return call.reject("secretKeyBase64 required")
        val vm  = call.getString("verificationMethod")
            ?: return call.reject("verificationMethod required")
        try {
            val bytes = Base64.decode(sk, Base64.DEFAULT)
            Sentlog.setSigningKey(bytes, vm)
            call.resolve()
        } catch (t: Throwable) {
            call.reject(t.message ?: "setSigningKey failed", t)
        }
    }

    @PluginMethod
    fun pushFaceFrame(call: PluginCall) {
        // Capacitor numbers cross the bridge as Double; convert to Long
        // for the underlying SDK call (nanosecond timestamps are
        // Long-typed on the C ABI side).
        val ts = call.getDouble("timestampNs")
            ?: return call.reject("timestampNs required")
        val valence = call.getFloat("valence")
            ?: return call.reject("valence required")
        val arousal = call.getFloat("arousal")
            ?: return call.reject("arousal required")
        val csv = call.getString("dominantFauCsv")
        try {
            Sentlog.pushFaceFrame(ts.toLong(), valence, arousal, csv)
            call.resolve()
        } catch (t: Throwable) {
            call.reject(t.message ?: "pushFaceFrame failed", t)
        }
    }

    @PluginMethod
    fun setDeviceAttestation(call: PluginCall) {
        val tokenBase64 = call.getString("tokenBase64")
        val type        = call.getString("attestationType")
        try {
            val bytes = tokenBase64?.let {
                android.util.Base64.decode(it, android.util.Base64.DEFAULT)
            }
            Sentlog.setDeviceAttestation(bytes, type)
            call.resolve()
        } catch (t: Throwable) {
            call.reject(t.message ?: "setDeviceAttestation failed", t)
        }
    }

    @PluginMethod
    fun setEnvelopeDispatcherActive(call: PluginCall) {
        val active = call.getBoolean("active") ?: false
        try {
            if (active) {
                Sentlog.setEnvelopeDispatcher { name, kind, sj, ej ->
                    val payload = JSObject()
                        .put("forwarder_name", name)
                        .put("sink_kind",      kind)
                        .put("sink_json",      sj)
                        .put("envelope_json",  ej)
                    notifyListeners("SentlogEnvelope", payload)
                }
            } else {
                Sentlog.setEnvelopeDispatcher(null)
            }
            call.resolve()
        } catch (t: Throwable) {
            call.reject(t.message ?: "setEnvelopeDispatcherActive failed", t)
        }
    }

    @PluginMethod
    fun drainQueueOnce(call: PluginCall) {
        val path = call.getString("path")
            ?: return call.reject("path required")
        val drainRequestId = call.getString("drainRequestId")
            ?: return call.reject("drainRequestId required")
        scope.launch {
            drainMutex.withLock {
                val replies = ConcurrentHashMap<Int, DrainReply>()
                drainReplies[drainRequestId] = replies
                try {
                    var lineIdx = 0
                    val delivered = Sentlog.drainQueue(path) { envelopeJson ->
                        val idx = lineIdx++
                        val payload = JSObject()
                            .put("drainRequestId", drainRequestId)
                            .put("line",           idx)
                            .put("envelope_json",  envelopeJson)
                        notifyListeners("SentlogDrainEnvelope", payload)
                        val deadline = System.currentTimeMillis() + 30_000L
                        while (replies[idx] == null) {
                            if (System.currentTimeMillis() > deadline) return@drainQueue false
                            Thread.sleep(5)
                        }
                        replies.remove(idx)!!.keep.not()  // C ABI: true=delete
                    }
                    call.resolve(JSObject().put("delivered", delivered))
                } catch (t: Throwable) {
                    call.reject(t.message ?: "drainQueueOnce failed", t)
                } finally {
                    drainReplies.remove(drainRequestId)
                }
            }
        }
    }

    @PluginMethod
    fun drainQueueReply(call: PluginCall) {
        val id   = call.getString("drainRequestId")
            ?: return call.reject("drainRequestId required")
        val line = call.getInt("line")
            ?: return call.reject("line required")
        val keep = call.getBoolean("keep") ?: true
        drainReplies[id]?.put(line, DrainReply(keep))
        call.resolve()
    }
}
