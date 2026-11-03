#if canImport(DeviceCheck)
import DeviceCheck
#endif
import CryptoKit
import Foundation
import SentlogCore

/// Phase 5e-ii helper: an App-Attest-backed attester that wraps the
/// DeviceCheck `DCAppAttestService` boilerplate the
/// `docs/integration/device-attestation.md` example spells out by
/// hand. Integrators using it wire device attestation in three calls:
///
///     let attester = try await AppAttestAttester.create()
///     let token    = try await attester.attest(challenge: serverChallenge)
///     try Sentlog.setDeviceAttestation(token, attestationType: "app_attest")
///
/// The key ID is generated on first use and persisted in
/// `UserDefaults` under the standard key
/// `me.cooki.sentlog.appattest.keyId`; subsequent calls reuse it.
/// The integrator can override the keychain key via `keyIdStoreKey`.
///
/// On hosts without App Attest (simulator, older iOS, devices where
/// Apple has disabled it temporarily) `create()` throws
/// `SentlogError(SENTLOG_ERR_HARDWARE_SIGNING)`. Integrators wanting
/// soft-fail wrap the call in `try?` and skip
/// `setDeviceAttestation` — the SDK then leaves
/// `evidence_grade` at `DeviceUnattested`.
public struct AppAttestAttester {

    /// The persistent key ID created by `DCAppAttestService.generateKey`.
    /// Save this for verification on the server side (Apple's verifier
    /// needs the keyId to validate the attestation).
    public let keyId: String

    private let storeKey: String

    /// Create (or load) the App Attest keypair. The private key never
    /// leaves the Secure Enclave; the SDK only holds the public keyId.
    ///
    /// `keyIdStoreKey` is the UserDefaults key under which the keyId
    /// is persisted; defaults to a Sentlog-namespaced key. Custom apps
    /// with multiple attestation contexts should pass distinct keys.
    /// Serialises first-time key generation so two tasks racing on app
    /// startup converge on one keyId. A lock can't be held across `await`;
    /// the actor shares one in-flight Task instead.
    private actor KeyGeneration {
        private var inFlight: [String: Task<String, Error>] = [:]

        func keyId(storeKey: String, generate: @escaping @Sendable () async throws -> String)
            async throws -> String {
            if let existing = UserDefaults.standard.string(forKey: storeKey) { return existing }
            if let task = inFlight[storeKey] { return try await task.value }
            let task = Task { () throws -> String in
                let id = try await generate()
                UserDefaults.standard.set(id, forKey: storeKey)
                return id
            }
            inFlight[storeKey] = task
            defer { inFlight[storeKey] = nil }
            return try await task.value
        }
    }
    private static let keyGeneration = KeyGeneration()

    public static func create(
        keyIdStoreKey: String = "me.cooki.sentlog.appattest.keyId"
    ) async throws -> AppAttestAttester {
        #if !canImport(DeviceCheck)
        throw SentlogError(status: SENTLOG_ERR_HARDWARE_SIGNING,
                           context: "AppAttestAttester: DeviceCheck not available on this platform")
        #else
        guard DCAppAttestService.shared.isSupported else {
            throw SentlogError(status: SENTLOG_ERR_HARDWARE_SIGNING,
                               context: "AppAttestAttester: DCAppAttestService.isSupported == false")
        }
        // Fast path outside the lock: existing keyId already persisted.
        if let existing = UserDefaults.standard.string(forKey: keyIdStoreKey) {
            return AppAttestAttester(keyId: existing, storeKey: keyIdStoreKey)
        }
        let keyId = try await keyGeneration.keyId(storeKey: keyIdStoreKey) {
            try await withCheckedThrowingContinuation { cont in
                DCAppAttestService.shared.generateKey { id, err in
                    if let err {
                        cont.resume(throwing: SentlogError(
                            status: SENTLOG_ERR_HARDWARE_SIGNING,
                            context: "AppAttestAttester: generateKey failed: \(err.localizedDescription)"))
                        return
                    }
                    guard let id else {
                        cont.resume(throwing: SentlogError(
                            status: SENTLOG_ERR_HARDWARE_SIGNING,
                            context: "AppAttestAttester: generateKey returned nil"))
                        return
                    }
                    cont.resume(returning: id)
                }
            }
        }
        return AppAttestAttester(keyId: keyId, storeKey: keyIdStoreKey)
        #endif
    }

    /// Produce an attestation over `challenge`. Hashes the challenge
    /// with SHA-256 (App Attest's `clientDataHash`) and asks the
    /// Secure Enclave to attest the keypair against it. The returned
    /// bytes are CBOR-encoded per Apple's App Attest spec — pass to
    /// `Sentlog.setDeviceAttestation(token, attestationType: "app_attest")`.
    ///
    /// `challenge` should be server-issued + single-use per
    /// attestation; reusing nonces undermines the freshness guarantee
    /// the attestation provides.
    public func attest(challenge: Data) async throws -> Data {
        #if !canImport(DeviceCheck)
        throw SentlogError(status: SENTLOG_ERR_HARDWARE_SIGNING,
                           context: "AppAttestAttester.attest: DeviceCheck not available")
        #else
        let hash = Data(SHA256.hash(data: challenge))
        return try await withCheckedThrowingContinuation { cont in
            DCAppAttestService.shared.attestKey(keyId,
                                                 clientDataHash: hash) { att, err in
                if let err {
                    cont.resume(throwing: SentlogError(
                        status: SENTLOG_ERR_HARDWARE_SIGNING,
                        context: "AppAttestAttester.attest: \(err.localizedDescription)"))
                    return
                }
                guard let att else {
                    cont.resume(throwing: SentlogError(
                        status: SENTLOG_ERR_HARDWARE_SIGNING,
                        context: "AppAttestAttester.attest: attestKey returned nil"))
                    return
                }
                cont.resume(returning: att)
            }
        }
        #endif
    }

    /// Produce an assertion over `challenge`. App Attest attests a key
    /// only once: use `attest(challenge:)` the first time, have your server
    /// verify and store the key, and use this for every later request.
    public func assertion(challenge: Data) async throws -> Data {
        #if !canImport(DeviceCheck)
        throw SentlogError(status: SENTLOG_ERR_HARDWARE_SIGNING,
                           context: "AppAttestAttester.assertion: DeviceCheck not available")
        #else
        let hash = Data(SHA256.hash(data: challenge))
        return try await withCheckedThrowingContinuation { cont in
            DCAppAttestService.shared.generateAssertion(keyId,
                                                        clientDataHash: hash) { assertion, err in
                if let assertion {
                    cont.resume(returning: assertion)
                } else {
                    cont.resume(throwing: SentlogError(
                        status: SENTLOG_ERR_HARDWARE_SIGNING,
                        context: "AppAttestAttester.assertion: \(err?.localizedDescription ?? "no assertion")"))
                }
            }
        }
        #endif
    }

    /// Clear the persisted keyId. Use this on logout / data-erasure
    /// flows; the next `create()` will generate a fresh key.
    public func clearPersistedKeyId() {
        UserDefaults.standard.removeObject(forKey: storeKey)
    }
}

/// Convenience on `Sentlog`: attest + push in one call. Pulls a
/// fresh attestation from the attester and registers it with the
/// SDK as type `"app_attest"`.
public extension Sentlog {
    static func attestAndRegister(
        _ attester: AppAttestAttester,
        challenge: Data
    ) async throws {
        let token = try await attester.attest(challenge: challenge)
        try Sentlog.setDeviceAttestation(token, attestationType: "app_attest")
    }
}
