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
            // Fully-qualified static call — required by Swift's
            // @convention(c) closure rule (no implicit self capture).
            let resp = RfcTsaProvider.blockingRequest(
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

        // Heap-allocated holder so the URLSession callback can safely
        // write to it after the stack frame here returns. Lock guards
        // the late-write-vs-read race when the timeout fires
        // concurrently with the callback completing.
        final class Holder { var data: Data? }
        let holder = Holder()
        let lock = NSLock()
        let sem  = DispatchSemaphore(value: 0)
        BoundedFetch.run(req, maxBytes: maxTsaRespBytes) { result in
            defer { sem.signal() }
            guard case .success(let data) = result,
                  isGrantedResponse(data, digest: digest) else { return }
            lock.lock()
            holder.data = data
            lock.unlock()
        }
        let timeout = DispatchTime.now() + .milliseconds(timeoutMs)
        if sem.wait(timeout: timeout) == .timedOut {
            return nil
        }
        lock.lock()
        let result = holder.data
        lock.unlock()
        return result
    }

    /// TimeStampResp ::= SEQUENCE {
    ///     status          PKIStatusInfo { status INTEGER, ... },
    ///     timeStampToken  TimeStampToken OPTIONAL
    /// }
    /// Requires PKIStatus 0 (granted) or 1 (with modifications) and a
    /// token containing our digest, so a rejection served with HTTP 200
    /// doesn't count. The token's signature and chain are the verifier's
    /// job.
    internal static func isGrantedResponse(_ response: Data, digest: Data) -> Bool {
        let resp = [UInt8](response)
        // Reads a tag and DER length at `pos`; returns (bodyStart, bodyLength).
        func header(_ pos: Int, _ tag: UInt8) -> (Int, Int)? {
            guard pos + 2 <= resp.count, resp[pos] == tag else { return nil }
            let first = Int(resp[pos + 1])
            if first < 0x80 { return (pos + 2, first) }
            let count = first & 0x7f
            guard count > 0, count <= 3, pos + 2 + count <= resp.count else { return nil }
            var length = 0
            for i in 0..<count { length = (length << 8) | Int(resp[pos + 2 + i]) }
            return (pos + 2 + count, length)
        }
        guard let (outerStart, outerLen) = header(0, 0x30),
              outerStart + outerLen == resp.count,
              let (infoStart, infoLen) = header(outerStart, 0x30),
              let (statusStart, statusLen) = header(infoStart, 0x02),
              statusLen == 1, statusStart < resp.count,
              resp[statusStart] == 0 || resp[statusStart] == 1 else { return false }
        let tokenStart = infoStart + infoLen
        guard tokenStart < resp.count else { return false }
        return response.range(of: digest,
                              in: tokenStart..<resp.count) != nil
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
