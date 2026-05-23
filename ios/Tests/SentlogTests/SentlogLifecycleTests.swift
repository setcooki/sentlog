import XCTest
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
}
