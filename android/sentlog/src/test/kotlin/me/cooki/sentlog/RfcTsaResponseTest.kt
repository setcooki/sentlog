package me.cooki.sentlog

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RfcTsaResponseTest {
    private val digest = ByteArray(32) { 0xAB.toByte() }
    private val token = byteArrayOf(0x30, 0x22, 0x04, 0x20) + digest

    private fun response(status: Byte, token: ByteArray?): ByteArray {
        val body = byteArrayOf(0x30, 0x03, 0x02, 0x01, status) + (token ?: ByteArray(0))
        return byteArrayOf(0x30, body.size.toByte()) + body
    }

    @Test fun acceptsGrantedResponseWithDigest() {
        assertTrue(RfcTsaProvider.isGrantedResponse(response(0, token), digest))
        assertTrue(RfcTsaProvider.isGrantedResponse(response(1, token), digest))
    }

    @Test fun rejectsPkiRejectionServedWithOk() {
        assertFalse(RfcTsaProvider.isGrantedResponse(response(2, token), digest))
    }

    @Test fun rejectsMissingOrWrongToken() {
        assertFalse(RfcTsaProvider.isGrantedResponse(response(0, null), digest))
        val other = byteArrayOf(0x30, 0x22, 0x04, 0x20) + ByteArray(32) { 0xCD.toByte() }
        assertFalse(RfcTsaProvider.isGrantedResponse(response(0, other), digest))
    }

    @Test fun rejectsMalformedResponse() {
        assertFalse(RfcTsaProvider.isGrantedResponse(byteArrayOf(0x30, 0x7F, 0x00), digest))
        assertFalse(RfcTsaProvider.isGrantedResponse(ByteArray(0), digest))
    }
}
