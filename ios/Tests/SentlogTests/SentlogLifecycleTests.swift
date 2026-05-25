import XCTest
import SentlogCore
@testable import Sentlog

/// End-to-end exercise of the Swift wrapper against the real C++
/// shared core. Validates that the Swift API + C ABI work as a unit:
///   - initialize succeeds with a valid config
///   - initialize rejects a malformed-shape config (config is
///     well-formed JSON but fails cross-field validation)
///   - getCapabilities returns a populated Capabilities
///   - destroy is idempotent
///
/// These tests do NOT touch AVAudioEngine because SwiftPM test runs
/// on macOS CI typically have no microphone permission and the
/// engine startup would fail.
final class SentlogLifecycleTests: XCTestCase {

    override func tearDown() {
        Sentlog.destroy()
    }

    func testInitializeWithMinimalConfig() throws {
        let cfg = SentlogConfig()
        try Sentlog.initialize(config: cfg)
        XCTAssertFalse(Sentlog.version.isEmpty)
    }

    func testInitializeRejectsHardwareSigningRequiredWithoutFlag() {
        var cfg = SentlogConfig()
        cfg.hardwareSigningRequired = true
        cfg.useHardwareSigning = false
        XCTAssertThrowsError(try Sentlog.initialize(config: cfg)) { error in
            guard let e = error as? SentlogError else {
                XCTFail("expected SentlogError, got \(error)")
                return
            }
            XCTAssertEqual(e.status, SENTLOG_ERR_INVALID_CONFIG)
        }
    }

    func testGetCapabilitiesReturnsRuntime() throws {
        try Sentlog.initialize(config: SentlogConfig())
        let caps = try Sentlog.getCapabilities()
        XCTAssertEqual(caps.runtime, .native)
        XCTAssertTrue(caps.audioCapture)
    }

    func testModelVersionsAreSchemaCorrect() throws {
        try Sentlog.initialize(config: SentlogConfig())
        let mv = try Sentlog.modelVersions()
        XCTAssertFalse(mv.liveness.isEmpty)
        XCTAssertFalse(mv.asr.isEmpty)
        XCTAssertFalse(mv.prosody.isEmpty)
        XCTAssertFalse(mv.manifest.isEmpty)
    }

    func testDoubleDestroyIsSafe() throws {
        try Sentlog.initialize(config: SentlogConfig())
        Sentlog.destroy()
        Sentlog.destroy()
    }

    func testReinitializeReplacesHandle() throws {
        try Sentlog.initialize(config: SentlogConfig())
        try Sentlog.initialize(config: SentlogConfig())  // destroys previous, creates new
        let caps = try Sentlog.getCapabilities()
        XCTAssertEqual(caps.runtime, .native)
    }

    // --- Phase 4d API parity (iOS Swift) ---

    func testSetSigningKeyAcceptsValid64ByteKey() throws {
        try Sentlog.initialize(config: SentlogConfig())
        // RFC 8032 §7.1 test vector 1 seed||pubkey — must accept.
        var sk = Data(repeating: 0, count: 64)
        let bytes: [UInt8] = [
            0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
            0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
            0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
            0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
            0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
            0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
            0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
            0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
        ]
        sk.replaceSubrange(0..<64, with: bytes)
        try Sentlog.setSigningKey(sk, verificationMethod: "did:key:zTest")
    }

    func testSetSigningKeyRejectsShortKey() throws {
        try Sentlog.initialize(config: SentlogConfig())
        let sk = Data(repeating: 0, count: 32)
        XCTAssertThrowsError(
            try Sentlog.setSigningKey(sk, verificationMethod: "did:key:x")
        ) { error in
            guard let e = error as? SentlogError else {
                XCTFail("expected SentlogError, got \(error)")
                return
            }
            XCTAssertEqual(e.status, SENTLOG_ERR_INVALID_CONFIG)
        }
    }

    func testSetEnvelopeDispatcherRoundTrips() throws {
        try Sentlog.initialize(config: SentlogConfig())
        // Register, replace, clear — exercises the retain/release
        // dance against the C ABI without hitting the live evaluate
        // path (which would need a real audio capture).
        try Sentlog.setEnvelopeDispatcher { _, _, _, _ in /* no-op */ }
        try Sentlog.setEnvelopeDispatcher { _, _, _, _ in /* replace */ }
        try Sentlog.setEnvelopeDispatcher(nil)        // clear
        try Sentlog.setEnvelopeDispatcher(nil)        // clear-when-cleared is OK
    }
}
