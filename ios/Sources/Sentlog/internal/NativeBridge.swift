import Foundation
import SentlogCore   // C ABI declared in shared/include/sentlog/sentlog.h

/// Thin Swift wrapper over the Sentlog C ABI. Translates C
/// status codes into Swift errors and bridges the C ABI's
/// callback-based async operations into `withCheckedThrowingContinuation`.
///
/// Not part of the public API. Integrators consume ``Sentlog``.
internal enum NativeBridge {

    // MARK: status

    static func mustSucceed(_ status: sentlog_status_t, _ context: String) throws {
        guard status == SENTLOG_OK else {
            throw SentlogError(status: status, context: context)
        }
    }

    // MARK: callbacks
    //
    // The C ABI passes a `void* userdata` opaque pointer to each
    // callback. We use it to round-trip a strong Swift reference. The
    // wrapper class boxes the continuation so it survives the C call.

    final class ResultBox {
        var continuation: CheckedContinuation<String, Error>?
        init(_ c: CheckedContinuation<String, Error>) { continuation = c }
    }

    final class UpdateBox {
        var continuation: CheckedContinuation<String, Error>?
        init(_ c: CheckedContinuation<String, Error>) { continuation = c }
    }

    /// A long-lived box for the telemetry callback. Owned by the SDK
    /// for the lifetime of the handle.
    final class TelemetryBox {
        let onEvent: (String) -> Void
        init(_ f: @escaping (String) -> Void) { onEvent = f }
    }

    /// A long-lived box for the envelope dispatcher (Phase 4d).
    /// Owned by Sentlog until the integrator replaces or clears the
    /// dispatcher; the C ABI side holds the matching Unmanaged
    /// pointer.
    final class DispatcherBox {
        let onEnvelope: (String, String, String, String) -> Void
        init(_ f: @escaping (String, String, String, String) -> Void) {
            onEnvelope = f
        }
    }

    /// A long-lived box for the Phase 7c hardware signer. The signer
    /// closure receives the JCS-canonical envelope bytes and returns
    /// the raw signature (≤72 bytes ECDSA-P256 DER, or 64 bytes
    /// Ed25519); nil signals signing failure → SDK falls back to
    /// software (or drops, when hardware_signing_required=true).
    final class HardwareSignerBox {
        let sign: (Data) -> Data?
        init(_ f: @escaping (Data) -> Data?) { sign = f }
    }
}

/// Strongly-typed error produced by the SDK. Maps to the
/// `sentlog_status_t` enum in the C ABI.
public struct SentlogError: Error, Equatable {
    public let status: sentlog_status_t
    public let context: String

    public init(status: sentlog_status_t, context: String) {
        self.status = status
        self.context = context
    }

    public var localizedDescription: String {
        "Sentlog \(context) failed (status=\(status.rawValue))"
    }
}
