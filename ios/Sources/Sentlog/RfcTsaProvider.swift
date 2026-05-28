import CryptoKit
import Foundation
import SentlogCore

/// Phase 4h-ii helper: a stdlib-only RFC 3161 TSA provider ready to
/// plug into `Sentlog.setTsaProvider(...)`:
///
///     try RfcTsaProvider.install()
///
/// The helper handles:
///   - SHA-256 of the SDK-supplied bytes via CryptoKit
///   - ASN.1 DER encoding of a minimal RFC 3161 TimeStampReq —
///     inline impl, no extra dep
///   - HTTPS POST to the configured URL with Content-Type:
///     application/timestamp-query via URLSession
///   - Returns the response body unchanged
///
/// On any failure (network, non-2xx status, oversize response,
/// non-HTTPS URL) the helper returns nil; the SDK emits a
/// TsaRequestFailed telemetry event; the envelope ships without
/// the tsa_timestamp block (unless tsa_required = true, in which
/// case it's dropped).
public enum RfcTsaProvider {

    private static let maxTsaRespBytes = 16 * 1024   // matches state.cpp cap

    /// Install the TSA provider against the SDK. Idempotent.
    public static func install() throws {
        let cb: sentlog_tsa_provider_cb_t = { toHash, toHashLen, urlC,
                                               timeoutMs, out, outLen, _ in
            guard let toHash, let urlC, let out, let outLen else { return -1 }
            let buf  = UnsafeBufferPointer(start: toHash, count: toHashLen)
            let url  = String(cString: urlC)
            let resp = blockingRequest(
                toHash: Data(buffer: buf),
                url: url,
                timeoutMs: Int(timeoutMs))
            guard let resp, !resp.isEmpty else { return -2 }
            let capacity = outLen.pointee
            if resp.count > capacity { return -3 }
            resp.withUnsafeBytes { raw in
                if let src = raw.baseAddress {
                    out.update(from: src.assumingMemoryBound(to: UInt8.self),
                               count: resp.count)
                }
            }
            outLen.pointee = resp.count
            return 0
        }
        try Sentlog.setTsaProvider(cb)
    }

    /// Clear any previously-registered TSA provider.
    public static func uninstall() throws {
        try Sentlog.setTsaProvider(nil)
    }

    // ------- internals -------

    /// Blocking variant — the SDK invokes the C ABI callback
    /// synchronously from dispatch_envelopes, so we have to wait
    /// here. Bounded by `timeoutMs`.
    private static func blockingRequest(
        toHash: Data,
        url: String,
        timeoutMs: Int
    ) -> Data? {
        guard url.hasPrefix("https://"), let endpoint = URL(string: url) else {
            return nil
        }
        let digest = Data(SHA256.hash(data: toHash))
        let body   = encodeTimeStampReq(sha256Digest: digest)

        var req = URLRequest(url: endpoint)
        req.httpMethod = "POST"
        req.httpBody   = body
        req.timeoutInterval = TimeInterval(timeoutMs) / 1000.0
        req.setValue("application/timestamp-query",
                     forHTTPHeaderField: "Content-Type")
        req.setValue("application/timestamp-reply",
                     forHTTPHeaderField: "Accept")

        let sem      = DispatchSemaphore(value: 0)
        var respData: Data? = nil
        let task = URLSession.shared.dataTask(with: req) { data, response, _ in
            defer { sem.signal() }
            guard let http = response as? HTTPURLResponse,
                  (200..<300).contains(http.statusCode),
                  let data, data.count <= maxTsaRespBytes else { return }
            respData = data
        }
        task.resume()
        let timeout = DispatchTime.now() + .milliseconds(timeoutMs)
        if sem.wait(timeout: timeout) == .timedOut {
            task.cancel()
            return nil
        }
        return respData
    }

    /// Encode a minimal RFC 3161 TimeStampReq — matches the Kotlin
    /// counterpart byte-for-byte for the same input digest. ~51 bytes.
    internal static func encodeTimeStampReq(sha256Digest: Data) -> Data {
        precondition(sha256Digest.count == 32, "SHA-256 digest must be 32 bytes")

        // AlgorithmIdentifier for id-sha256
        // (OID 2.16.840.1.101.3.4.2.1).
        let sha256Oid: [UInt8] = [
            0x06, 0x09,
            0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
        ]
        let nullParams: [UInt8] = [0x05, 0x00]
        let algId = sequence(Data(sha256Oid) + Data(nullParams))

        var hashedMessage = Data([0x04, 32])
        hashedMessage.append(sha256Digest)

        let messageImprint = sequence(algId + hashedMessage)
        let version = Data([0x02, 0x01, 0x01])
        return sequence(version + messageImprint)
    }

    private static func sequence(_ body: Data) -> Data {
        precondition(body.count < 128, "DER long-form length not implemented")
        var out = Data([0x30, UInt8(body.count)])
        out.append(body)
        return out
    }
}
