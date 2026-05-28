package me.cooki.sentlog

import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Phase 4h-ii helper: a stdlib-only RFC 3161 TSA provider ready to
 * plug into [Sentlog.setTsaProvider]:
 *
 * ```kotlin
 * RfcTsaProvider.install()
 * ```
 *
 * The helper handles:
 *   - SHA-256 of the SDK-supplied bytes (proof.proofValue) via
 *     `java.security.MessageDigest`
 *   - ASN.1 DER encoding of a minimal RFC 3161 TimeStampReq
 *     (Version=1, MessageImprint(SHA-256, digest), no nonce, no
 *     certReq) — inline impl, no extra dep
 *   - HTTPS POST to the integrator-supplied URL with
 *     Content-Type: application/timestamp-query
 *   - Returns the response body unchanged (the SDK records it as
 *     `tsa_timestamp.token_b64` on every envelope)
 *
 * On any failure (network, non-2xx status, oversize response) the
 * helper returns null and the SDK emits a TsaRequestFailed
 * telemetry event; the envelope ships without the tsa_timestamp
 * block (unless `tsa_required = true`, in which case it's dropped).
 */
public object RfcTsaProvider {

    private const val MAX_TSA_RESP_BYTES = 16 * 1024   // matches state.cpp cap

    /** Install the TSA provider against the SDK. Idempotent. */
    @JvmStatic
    public fun install() {
        Sentlog.setTsaProvider(::request)
    }

    /** Clear any previously-registered TSA provider. */
    @JvmStatic
    public fun uninstall() {
        Sentlog.setTsaProvider(null)
    }

    // ------- internals -------

    private fun request(
        toHash: ByteArray,
        url: String,
        timeoutMs: Int,
    ): ByteArray? {
        if (!url.startsWith("https://")) return null
        val digest: ByteArray = try {
            MessageDigest.getInstance("SHA-256").digest(toHash)
        } catch (_: Throwable) { return null }

        val req: ByteArray = encodeTimeStampReq(digest)

        return try {
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = timeoutMs
                readTimeout    = timeoutMs
                doInput        = true
                doOutput       = true
                requestMethod  = "POST"
                instanceFollowRedirects = false
                setRequestProperty("Content-Type", "application/timestamp-query")
                setRequestProperty("Accept",       "application/timestamp-reply")
                setFixedLengthStreamingMode(req.size)
            }
            try {
                conn.outputStream.use { it.write(req) }
                val code = conn.responseCode
                if (code !in 200..299) return null
                conn.inputStream.use { stream ->
                    val out = ByteArrayOutputStream()
                    val buf = ByteArray(4 * 1024)
                    while (true) {
                        val n = stream.read(buf)
                        if (n < 0) break
                        if (out.size() + n > MAX_TSA_RESP_BYTES) return null
                        out.write(buf, 0, n)
                    }
                    out.toByteArray()
                }
            } finally {
                conn.disconnect()
            }
        } catch (_: Throwable) {
            null
        }
    }

    /**
     * Encode a minimal RFC 3161 TimeStampReq:
     *
     *   TimeStampReq ::= SEQUENCE {
     *       version          INTEGER (v1),
     *       messageImprint   MessageImprint {
     *           hashAlgorithm  AlgorithmIdentifier (SHA-256),
     *           hashedMessage  OCTET STRING (digest)
     *       }
     *   }
     *
     * We omit `reqPolicy`, `nonce`, `certReq`, `extensions` — all
     * optional. The encoded payload is ~51 bytes; well within any
     * TSA's request-size limit.
     */
    internal fun encodeTimeStampReq(sha256Digest: ByteArray): ByteArray {
        require(sha256Digest.size == 32) { "SHA-256 digest must be 32 bytes" }

        // AlgorithmIdentifier for id-sha256 (OID 2.16.840.1.101.3.4.2.1).
        // DER OID encoding: 0x60 0x86 0x48 0x01 0x65 0x03 0x04 0x02 0x01
        // (9 bytes; first byte 0x60 = 40*2 + 16). Wrapped in SEQUENCE
        // with NULL parameters (per RFC 5754).
        val sha256Oid = byteArrayOf(
            0x06, 0x09,
            0x60, 0x86.toByte(), 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
        )
        val nullParams = byteArrayOf(0x05, 0x00)
        val algId = sequence(sha256Oid + nullParams)

        // hashedMessage OCTET STRING
        val hashedMessage = byteArrayOf(0x04, 32) + sha256Digest

        // MessageImprint SEQUENCE
        val messageImprint = sequence(algId + hashedMessage)

        // Version INTEGER (v1)
        val version = byteArrayOf(0x02, 0x01, 0x01)

        return sequence(version + messageImprint)
    }

    /** Wrap `body` in a DER SEQUENCE (tag 0x30). For our 51-byte
     *  output, length always fits in a single byte. */
    private fun sequence(body: ByteArray): ByteArray {
        require(body.size < 128) { "DER long-form length not implemented" }
        return byteArrayOf(0x30, body.size.toByte()) + body
    }
}
