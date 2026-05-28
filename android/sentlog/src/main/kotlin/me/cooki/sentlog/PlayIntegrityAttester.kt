package me.cooki.sentlog

import android.content.Context
import com.google.android.play.core.integrity.IntegrityManagerFactory
import com.google.android.play.core.integrity.IntegrityTokenRequest
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/**
 * Phase 5e-ii helper: a Play-Integrity-backed attester that wraps
 * the `IntegrityManager` boilerplate the
 * `docs/integration/device-attestation.md` example spells out by
 * hand. Integrators using it wire device attestation in two calls:
 *
 * ```kotlin
 * val attester = PlayIntegrityAttester.create(applicationContext)
 * val token    = attester.attest(nonce = serverNonce)
 * Sentlog.setDeviceAttestation(
 *     tokenBytes      = token.toByteArray(Charsets.UTF_8),
 *     attestationType = "play_integrity")
 * ```
 *
 * Play Integrity is shipped as `compileOnly` in the SDK's
 * build.gradle so apps that don't use device attestation don't have
 * to ship Play Services. If the dep isn't on the integrator's
 * runtime classpath, `create()` throws
 * [NoClassDefFoundError] — wrap in try/catch or check
 * `PlayIntegrityAttester::class.java.classLoader.loadClass(...)`
 * before calling.
 *
 * The Sentlog SDK never calls Play Integrity itself — this helper is
 * pure boilerplate reduction. The integrator owns nonce strategy,
 * quota management, and when to refresh tokens.
 */
public class PlayIntegrityAttester private constructor(
    private val applicationContext: Context,
) {
    public companion object {
        /**
         * Create an attester bound to the given application context.
         * Probes the runtime classpath for `IntegrityManagerFactory`
         * eagerly so missing-dep errors surface at `create()`
         * instead of the first `attest()` call.
         *
         * Throws [IllegalStateException] when the Play Integrity dep
         * isn't on the runtime classpath; throws
         * [IllegalArgumentException] when `context` isn't an
         * application context (passing an Activity here leaks).
         */
        @JvmStatic
        public fun create(applicationContext: Context): PlayIntegrityAttester {
            require(applicationContext === applicationContext.applicationContext) {
                "PlayIntegrityAttester.create: pass applicationContext, not an Activity"
            }
            // Eager classpath probe — matches the docstring claim.
            // Without this, the missing-dep failure would surface
            // only on the first attest() call, which integrators
            // building dep-detection logic on top of create() would
            // miss.
            try {
                Class.forName(
                    "com.google.android.play.core.integrity.IntegrityManagerFactory",
                    /* initialize = */ false,
                    PlayIntegrityAttester::class.java.classLoader)
            } catch (e: ClassNotFoundException) {
                throw IllegalStateException(
                    "PlayIntegrityAttester.create: Play Integrity dep not on " +
                    "classpath. Add `implementation \"com.google.android.play:integrity:1.3.0\"` " +
                    "to your app's build.gradle (the SDK declares it compileOnly).", e)
            }
            return PlayIntegrityAttester(applicationContext)
        }
    }

    /**
     * Request an integrity verdict bound to `nonce`. Returns the
     * JWS-encoded token string Play Integrity issued. Pass the
     * UTF-8 bytes of this token to [Sentlog.setDeviceAttestation]
     * with `attestationType = "play_integrity"`.
     *
     * `nonce` is integrator-supplied + single-use; typically the
     * server's challenge for this attestation round.
     *
     * Throws on any Play Integrity failure (quota, network, signing).
     * Wrap in try/catch and decide whether to push a stale token,
     * skip attestation (envelope ships at DeviceUnattested), or
     * surface to the user.
     */
    public suspend fun attest(nonce: String): String {
        val manager = IntegrityManagerFactory.create(applicationContext)
        val request = IntegrityTokenRequest.builder()
            .setNonce(nonce)
            .build()
        return suspendCancellableCoroutine { cont ->
            manager.requestIntegrityToken(request)
                .addOnSuccessListener { resp ->
                    cont.resume(resp.token())
                }
                .addOnFailureListener { err ->
                    cont.resumeWithException(err)
                }
        }
    }
}

/**
 * Convenience on [Sentlog]: attest + push in one call. Equivalent to
 *
 *     val token = attester.attest(nonce)
 *     Sentlog.setDeviceAttestation(
 *         token.toByteArray(Charsets.UTF_8), "play_integrity")
 *
 * Pass `nonce = null` to call the attester with an empty nonce
 * (some integrators use a separate freshness channel and don't need
 * Play Integrity's nonce binding).
 */
public suspend fun Sentlog.attestAndRegister(
    attester: PlayIntegrityAttester,
    nonce: String,
) {
    val token = attester.attest(nonce)
    setDeviceAttestation(
        tokenBytes      = token.toByteArray(Charsets.UTF_8),
        attestationType = "play_integrity")
}
