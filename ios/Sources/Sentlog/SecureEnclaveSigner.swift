import Foundation
import Security
import SentlogCore

/// Phase 7.5 helper: a Secure-Enclave-backed signer ready to plug into
/// ``Sentlog/setHardwareSigner(_:cryptosuite:verificationMethod:)``.
///
/// The private key is generated on first use inside the Secure Enclave
/// (`kSecAttrTokenIDSecureEnclave`) and never leaves it. On subsequent
/// launches the key is loaded from the keychain by `keyTag`. The
/// algorithm is ECDSA-P256; the produced signatures are ASN.1/DER
/// and match the W3C Data Integrity `ecdsa-jcs-2019` cryptosuite that
/// the SDK attaches as the envelope's proof.
///
/// Typical use:
///
///     let signer = try SecureEnclaveSigner.create(
///         keyTag: "me.example.myapp.sentlog")
///     try Sentlog.setHardwareSigner(signer)
///
/// On simulator (no Secure Enclave) `create` throws
/// `SentlogError(status: SENTLOG_ERR_HARDWARE_SIGNING)`. Integrators
/// that want soft-fail behaviour wrap the call in `try?` and skip
/// `setHardwareSigner` — the SDK then uses the software signing key.
public struct SecureEnclaveSigner: Sendable {

    /// The raw uncompressed P-256 public key (65 bytes, 0x04 || X || Y).
    public let publicKeyRaw: Data

    /// `did:key:` URI derived from the public key using the
    /// `did:key` method specification for P-256 (multicodec 0x1200).
    /// Pass this as `setHardwareSigner(... verificationMethod:)` so
    /// the envelope's `proof.verificationMethod` is self-describing
    /// and the verifier can resolve to the public key without an
    /// external key directory.
    public let verificationMethod: String

    /// The closure to hand to `Sentlog.setHardwareSigner`. Captures
    /// the SecKey reference; safe to call from any thread (CryptoKit
    /// / SecKeyCreateSignature are thread-safe). Returns nil on
    /// signing failure so the SDK can fall back per
    /// `hardware_signing_required`.
    public let sign: @Sendable (Data) -> Data?

    /// Create (or load if already present) a Secure-Enclave ECDSA-P256
    /// key under the given keychain tag. `accessControl` defaults to
    /// `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly` with
    /// `.privateKeyUsage` — change to require biometrics by passing a
    /// custom `SecAccessControl`.
    ///
    /// Throws `SentlogError(SENTLOG_ERR_HARDWARE_SIGNING)` when the
    /// Secure Enclave is unavailable (simulator, older devices that
    /// shouldn't get here in practice) or when key creation fails.
    public static func create(
        keyTag: String,
        accessControl: SecAccessControl? = nil
    ) throws -> SecureEnclaveSigner {

        let tagData = keyTag.data(using: .utf8) ?? Data()

        // Try to load an existing key first; persistence-by-tag is the
        // common path after first launch.
        let privateKey: SecKey = try {
            if let existing = loadKey(tag: tagData) { return existing }
            return try createKey(tag: tagData, accessControl: accessControl)
        }()

        guard let publicKey = SecKeyCopyPublicKey(privateKey) else {
            throw SentlogError(
                status: SENTLOG_ERR_HARDWARE_SIGNING,
                context: "SecureEnclaveSigner: SecKeyCopyPublicKey failed")
        }
        var err: Unmanaged<CFError>?
        guard let pubKeyData = SecKeyCopyExternalRepresentation(
                publicKey, &err) as Data? else {
            throw SentlogError(
                status: SENTLOG_ERR_HARDWARE_SIGNING,
                context: "SecureEnclaveSigner: pubkey export failed")
        }

        let vm = didKeyP256(from: pubKeyData)

        // Capture the SecKey by-value in the closure. SecKey is a
        // CFType — moved into the closure by ARC, released when the
        // signer is dropped.
        let signClosure: @Sendable (Data) -> Data? = { canonical in
            var signErr: Unmanaged<CFError>?
            guard let sig = SecKeyCreateSignature(
                    privateKey,
                    .ecdsaSignatureMessageX962SHA256,
                    canonical as CFData,
                    &signErr) as Data? else {
                return nil
            }
            return sig
        }

        return SecureEnclaveSigner(
            publicKeyRaw: pubKeyData,
            verificationMethod: vm,
            sign: signClosure)
    }

    // MARK: internals

    private static func loadKey(tag: Data) -> SecKey? {
        let query: [String: Any] = [
            kSecClass as String:               kSecClassKey,
            kSecAttrKeyType as String:         kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrApplicationTag as String:  tag,
            kSecReturnRef as String:           true,
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecSuccess, let item, CFGetTypeID(item) == SecKeyGetTypeID() {
            // The cast is safe — SecKey IS a CFType under the hood, and
            // `as!` here goes through CFTypeID which we checked above.
            return (item as! SecKey)
        }
        return nil
    }

    private static func createKey(
        tag: Data,
        accessControl: SecAccessControl?
    ) throws -> SecKey {
        // Default access: after-first-unlock, on this device only,
        // with .privateKeyUsage so signing operations are permitted.
        // Integrators wanting biometric gating pass their own.
        let access: SecAccessControl
        if let accessControl {
            access = accessControl
        } else {
            var aerr: Unmanaged<CFError>?
            guard let a = SecAccessControlCreateWithFlags(
                    nil,
                    kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
                    .privateKeyUsage,
                    &aerr) else {
                throw SentlogError(
                    status: SENTLOG_ERR_HARDWARE_SIGNING,
                    context: "SecureEnclaveSigner: SecAccessControlCreateWithFlags failed")
            }
            access = a
        }

        let attrs: [String: Any] = [
            kSecAttrKeyType as String:           kSecAttrKeyTypeECSECPrimeRandom,
            kSecAttrKeySizeInBits as String:     256,
            kSecAttrTokenID as String:           kSecAttrTokenIDSecureEnclave,
            kSecPrivateKeyAttrs as String: [
                kSecAttrIsPermanent as String:   true,
                kSecAttrApplicationTag as String: tag,
                kSecAttrAccessControl as String: access,
            ],
        ]
        var err: Unmanaged<CFError>?
        guard let key = SecKeyCreateRandomKey(attrs as CFDictionary, &err) else {
            // SE unavailable (simulator), or hardware-policy block.
            // Surface as HARDWARE_SIGNING so the caller can decide
            // whether to fall back to software signing.
            throw SentlogError(
                status: SENTLOG_ERR_HARDWARE_SIGNING,
                context: "SecureEnclaveSigner: SecKeyCreateRandomKey failed")
        }
        return key
    }

    /// Derive a `did:key:` URI for a P-256 public key per the
    /// did:key Method Specification (multicodec 0x1200 +
    /// base58btc with the "z" multibase prefix).
    ///
    /// `pubKeyData` is the raw uncompressed P-256 key (65 bytes:
    /// 0x04 || X || Y) — the format SecKeyCopyExternalRepresentation
    /// returns for kSecAttrKeyTypeECSECPrimeRandom.
    ///
    /// The did:key spec expects the COMPRESSED form (33 bytes:
    /// 0x02/0x03 || X) to follow the multicodec prefix. We compress
    /// inline: parity bit from Y.
    private static func didKeyP256(from pubKeyData: Data) -> String {
        // Compress 65 -> 33. If anything is off (length mismatch),
        // fall back to a "did:key:?" sentinel so the integrator notices
        // — better than silently emitting a malformed VM.
        guard pubKeyData.count == 65, pubKeyData[0] == 0x04 else {
            return "did:key:unknown-p256-format"
        }
        let x = pubKeyData[1..<33]
        let yLast = pubKeyData[64]
        let prefix: UInt8 = (yLast & 1) == 0 ? 0x02 : 0x03
        var compressed = Data([prefix])
        compressed.append(contentsOf: x)

        // multicodec prefix for P-256: 0x1200 (varint = 0x80 0x24).
        var multicodec = Data([0x80, 0x24])
        multicodec.append(compressed)

        return "did:key:z" + base58btc(multicodec)
    }

    /// Bitcoin-style base58 encoding. Implemented inline (no
    /// dependency) — the input is always 35 bytes, output ~48 chars.
    private static func base58btc(_ data: Data) -> String {
        let alphabet = Array(
            "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz")
        // Count leading zero bytes — these become leading '1's.
        var zeros = 0
        for b in data {
            if b == 0 { zeros += 1 } else { break }
        }
        // Convert big-endian bytes to base58 by repeated division.
        var input = Array(data)
        var digits: [Int] = []
        while !input.isEmpty {
            var remainder = 0
            var newInput: [UInt8] = []
            for b in input {
                let acc = remainder * 256 + Int(b)
                let q = acc / 58
                remainder = acc % 58
                if !(newInput.isEmpty && q == 0) {
                    newInput.append(UInt8(q))
                }
            }
            digits.append(remainder)
            input = newInput
        }
        let leading = String(repeating: "1", count: zeros)
        let tail = String(digits.reversed().map { alphabet[$0] })
        return leading + tail
    }
}

/// Convenience overload so callers don't have to spell out the
/// signer's `.sign`, `cryptosuite`, and `verificationMethod` fields
/// when wiring a `SecureEnclaveSigner`.
public extension Sentlog {
    static func setHardwareSigner(_ signer: SecureEnclaveSigner) throws {
        try setHardwareSigner(
            signer.sign,
            cryptosuite: "ecdsa-jcs-2019",
            verificationMethod: signer.verificationMethod)
    }
}
