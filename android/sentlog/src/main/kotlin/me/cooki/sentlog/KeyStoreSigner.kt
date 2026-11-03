package me.cooki.sentlog

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.ProviderException
import java.security.Signature
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec

/**
 * Phase 7.5 helper: an AndroidKeyStore-backed signer ready to plug
 * into [Sentlog.setHardwareSigner].
 *
 * The private key is generated on first use inside the AndroidKeyStore
 * — StrongBox-backed when the device supports it, regular hardware-
 * backed otherwise. The algorithm is ECDSA-P256; signatures are
 * ASN.1/DER and match the W3C Data Integrity `ecdsa-jcs-2019`
 * cryptosuite the SDK attaches as the envelope's proof.
 *
 * Typical use:
 *
 * ```kotlin
 * val signer = KeyStoreSigner.create(keyAlias = "me.example.myapp.sentlog")
 * Sentlog.setHardwareSigner(signer)
 * ```
 *
 * Ed25519 in AndroidKeyStore is API 33+ only; this helper sticks to
 * ECDSA-P256 across the board so the integration story is uniform.
 * Integrators on API 33+ wanting Ed25519 can wire their own signer
 * via the lower-level [Sentlog.setHardwareSigner] overload.
 *
 * If StrongBox isn't available the helper transparently falls back
 * to non-StrongBox hardware backing — both surfaces satisfy
 * `EnclaveSigned` evidence-grade promotion. Genuine key-creation
 * failure (no hardware backing at all on an emulator without
 * security hardware) raises [IllegalStateException], matching the
 * iOS [SecureEnclaveSigner.create] contract.
 */
public class KeyStoreSigner private constructor(
    /** Raw uncompressed P-256 public key (65 bytes, 0x04 || X || Y). */
    public val publicKeyRaw: ByteArray,
    /** `did:key:` URI derived from the public key. Pass as the
     *  `verificationMethod` when wiring [Sentlog.setHardwareSigner]
     *  so the envelope's proof is self-describing. */
    public val verificationMethod: String,
    /** True if the key actually lives in StrongBox; false if the
     *  device dropped the StrongBox request and we fell back to
     *  regular hardware backing. Either way the key is hw-backed. */
    public val strongBoxBacked: Boolean,
    /** The signer closure to hand to [Sentlog.setHardwareSigner].
     *  Returns null on signing failure (KeyStore unavailable,
     *  biometric prompt denied, etc.). */
    public val sign: (ByteArray) -> ByteArray?,
) {
    public companion object {

        /**
         * Create (or load) an ECDSA-P256 AndroidKeyStore key under
         * [keyAlias]. StrongBox-backed when available, regular
         * hardware-backed otherwise.
         *
         * Throws [IllegalStateException] when no hardware-backed key
         * can be created at all — i.e. the device has no security
         * hardware. Integrators wanting soft-fail wrap the call in
         * try/catch and skip [Sentlog.setHardwareSigner]; the SDK
         * then uses the software signing path with a
         * `HardwareSigningUnavailable` telemetry event.
         */
        @JvmStatic
        @Synchronized
        public fun create(keyAlias: String): KeyStoreSigner {
            val keyStore = KeyStore.getInstance("AndroidKeyStore").apply {
                load(null)
            }

            // containsAlias, not a failed load: a transient KeyStore error
            // must surface rather than replace the key and change identity.
            val privateKey =
                if (keyStore.containsAlias(keyAlias)) {
                    keyStore.getKey(keyAlias, null) as java.security.PrivateKey
                } else {
                    createKey(keyAlias, keyStore)
                }
            // EnclaveSigned promises hardware backing. AndroidKeyStore can
            // silently fall back to software on devices without it.
            check(isInsideSecureHardware(privateKey)) {
                "KeyStoreSigner: key $keyAlias is not hardware-backed"
            }

            val cert = keyStore.getCertificate(keyAlias)
                ?: error("KeyStoreSigner: cert missing for $keyAlias")
            val pubEC = cert.publicKey as? ECPublicKey
                ?: error("KeyStoreSigner: public key is not EC")

            val rawPub = encodeUncompressedP256(pubEC)
            val vm = didKeyP256(rawPub)
            val strongBox = isKeyStrongBox(privateKey)

            // Capture the alias in the closure rather than the key
            // itself — Signature.initSign with a freshly-fetched key
            // each call avoids edge cases around KeyStore key
            // invalidation (biometric re-enrollment, etc.).
            val signClosure: (ByteArray) -> ByteArray? = { canonical ->
                try {
                    val sig = Signature.getInstance("SHA256withECDSA")
                    val keyAtSign = keyStore.getKey(keyAlias, null) as java.security.PrivateKey
                    sig.initSign(keyAtSign)
                    sig.update(canonical)
                    sig.sign()
                } catch (_: Throwable) {
                    null
                }
            }

            return KeyStoreSigner(
                publicKeyRaw       = rawPub,
                verificationMethod = vm,
                strongBoxBacked    = strongBox,
                sign               = signClosure,
            )
        }

        // ----- internals --------------------------------------------

        private fun keyInfo(key: java.security.PrivateKey): android.security.keystore.KeyInfo =
            java.security.KeyFactory.getInstance(key.algorithm, "AndroidKeyStore")
                .getKeySpec(key, android.security.keystore.KeyInfo::class.java)

        @Suppress("DEPRECATION")  // the only signal below API 31
        private fun isInsideSecureHardware(key: java.security.PrivateKey): Boolean {
            val info = keyInfo(key)
            if (android.os.Build.VERSION.SDK_INT < 31) return info.isInsideSecureHardware
            return info.securityLevel ==
                android.security.keystore.KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT ||
                info.securityLevel ==
                android.security.keystore.KeyProperties.SECURITY_LEVEL_STRONGBOX
        }

        private fun createKey(alias: String, keyStore: KeyStore): java.security.PrivateKey {
            val kpg = KeyPairGenerator.getInstance(
                KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore")

            fun specBuilder(strongBox: Boolean): KeyGenParameterSpec {
                val b = KeyGenParameterSpec.Builder(
                        alias,
                        KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY)
                    .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
                    .setDigests(KeyProperties.DIGEST_SHA256)
                if (strongBox && android.os.Build.VERSION.SDK_INT >= 28) {
                    b.setIsStrongBoxBacked(true)
                }
                return b.build()
            }

            // Try StrongBox first on API 28+; fall back to regular
            // hardware backing on the two known StrongBox-unavailable
            // failure modes:
            //   - StrongBoxUnavailableException: the documented one
            //   - ProviderException: some Samsung devices on early
            //     API 28 raise this instead, with cause = SBox failure.
            // Any other exception is a genuine error (no security
            // hardware, bad params, OOM) — surface it unchanged
            // rather than silently degrading.
            try {
                kpg.initialize(specBuilder(strongBox = true))
                kpg.generateKeyPair()
            } catch (e: Exception) {
                val isStrongBoxFailure =
                    e is StrongBoxUnavailableException ||
                    e is ProviderException
                if (!isStrongBoxFailure) {
                    throw IllegalStateException(
                        "KeyStoreSigner: could not create hardware-backed key", e)
                }
                try {
                    kpg.initialize(specBuilder(strongBox = false))
                    kpg.generateKeyPair()
                } catch (e2: Exception) {
                    throw IllegalStateException(
                        "KeyStoreSigner: could not create hardware-backed key " +
                        "(StrongBox + fallback both failed)", e2)
                }
            }

            return keyStore.getKey(alias, null) as java.security.PrivateKey
        }

        private fun isKeyStrongBox(key: java.security.PrivateKey): Boolean {
            if (android.os.Build.VERSION.SDK_INT < 28) return false
            return try {
                val info = keyInfo(key)
                // StrongBox specifically: SDK 31+ surfaces it via
                // getSecurityLevel.
                if (android.os.Build.VERSION.SDK_INT >= 31) {
                    info.securityLevel ==
                        android.security.keystore.KeyProperties.SECURITY_LEVEL_STRONGBOX
                } else {
                    // Pre-31: no reliable runtime signal. Assume false.
                    false
                }
            } catch (_: Throwable) {
                false
            }
        }

        /** Encode an EC public key as raw 65-byte 0x04 || X || Y. */
        private fun encodeUncompressedP256(pub: ECPublicKey): ByteArray {
            val point = pub.w
            val x = point.affineX.toByteArray()
            val y = point.affineY.toByteArray()
            // BigInteger.toByteArray may include a leading 0x00 sign byte
            // and may be shorter than 32. Normalise to exactly 32 bytes.
            fun pad32(b: ByteArray): ByteArray {
                val src = if (b.size > 32) b.copyOfRange(b.size - 32, b.size) else b
                val out = ByteArray(32)
                System.arraycopy(src, 0, out, 32 - src.size, src.size)
                return out
            }
            val out = ByteArray(65)
            out[0] = 0x04
            System.arraycopy(pad32(x), 0, out, 1, 32)
            System.arraycopy(pad32(y), 0, out, 33, 32)
            return out
        }

        /**
         * did:key URI for a P-256 public key per the did:key Method
         * Specification: multicodec 0x1200 (P-256) + compressed
         * pubkey + base58btc + "z" multibase prefix.
         */
        private fun didKeyP256(rawUncompressed: ByteArray): String {
            require(rawUncompressed.size == 65 && rawUncompressed[0] == 0x04.toByte())
            val x = rawUncompressed.copyOfRange(1, 33)
            val yLast = rawUncompressed[64]
            val prefix: Byte = if ((yLast.toInt() and 1) == 0) 0x02 else 0x03
            val compressed = ByteArray(33).also {
                it[0] = prefix
                System.arraycopy(x, 0, it, 1, 32)
            }
            // multicodec 0x1200 as varint: 0x80 0x24
            val multicodec = ByteArray(2 + compressed.size).also {
                it[0] = 0x80.toByte()
                it[1] = 0x24
                System.arraycopy(compressed, 0, it, 2, compressed.size)
            }
            return "did:key:z" + base58btc(multicodec)
        }

        /** Bitcoin-style base58 encoding; ~35 bytes in, ~48 chars out. */
        private fun base58btc(data: ByteArray): String {
            val alphabet =
                "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
            var zeros = 0
            for (b in data) {
                if (b.toInt() == 0) zeros++ else break
            }
            var input = data.map { it.toInt() and 0xff }.toMutableList()
            val digits = mutableListOf<Int>()
            while (input.any { it != 0 }) {
                var remainder = 0
                val newInput = mutableListOf<Int>()
                for (b in input) {
                    val acc = remainder * 256 + b
                    val q = acc / 58
                    remainder = acc % 58
                    if (!(newInput.isEmpty() && q == 0)) {
                        newInput.add(q)
                    }
                }
                digits.add(remainder)
                input = newInput
            }
            val sb = StringBuilder()
            repeat(zeros) { sb.append('1') }
            for (d in digits.reversed()) sb.append(alphabet[d])
            return sb.toString()
        }
    }
}
