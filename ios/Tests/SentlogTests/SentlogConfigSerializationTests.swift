import XCTest
@testable import Sentlog

/// Round-trip tests for the SentlogConfig schema. The C++ core parses
/// the JSON the Swift layer emits; if the field names drift, native
/// init fails with INVALID_CONFIG. These tests pin the schema.
final class SentlogConfigSerializationTests: XCTestCase {

    func testDefaultConfigSerialisesWithSnakeCaseFieldNames() throws {
        let cfg = SentlogConfig()
        let data = try JSONEncoder().encode(cfg)
        let json = try XCTUnwrap(try JSONSerialization.jsonObject(with: data) as? [String: Any])

        for key in [
            "liveness_threshold", "engagement_threshold", "direction_threshold",
            "min_confidence",
            "challenge_enabled", "language_hint",
            "min_duration_ms", "max_duration_ms",
            "use_motion_sensors", "use_video_affect", "use_device_integrity",
            "use_hardware_signing", "hardware_signing_required",
            "tsa_required",
            "companion_evidence_refs", "forwarders",
        ] {
            XCTAssertNotNil(json[key], "Expected snake_case key '\(key)' in serialised config")
        }
    }

    func testDefaultThresholdsMatchSpec() {
        let cfg = SentlogConfig()
        XCTAssertEqual(cfg.livenessThreshold,   0.7)
        XCTAssertEqual(cfg.engagementThreshold, 0.5)
        XCTAssertEqual(cfg.directionThreshold,  0.6)
        XCTAssertEqual(cfg.minConfidence,       0.6)
    }

    func testProjectionEnumSerialisesWithPascalCaseAndPlusOperator() throws {
        var cfg = SentlogConfig()
        cfg.issuer = "did:example:integrator"
        cfg.forwarders = [
            Forwarder(
                name: "audit",
                projection: .evidenceFingerprint,
                sink: ["kind": "http", "url": "https://example.com/audit"]
            ),
        ]
        let data = try JSONEncoder().encode(cfg)
        let json = String(data: data, encoding: .utf8) ?? ""
        XCTAssertTrue(json.contains("\"projection\":\"EvidenceFingerprint\""))
    }

    func testFullWithAudioClipProjectionPreservesPlusOperator() throws {
        let forwarder = Forwarder(
            name: "full-audit",
            projection: .fullWithAudioClip,
            sink: ["kind": "http"]
        )
        let data = try JSONEncoder().encode(forwarder)
        let json = String(data: data, encoding: .utf8) ?? ""
        XCTAssertTrue(json.contains("\"Full+AudioClip\""))
    }

    func testRoundTripPreservesAllFields() throws {
        var cfg = SentlogConfig()
        cfg.livenessThreshold = 0.75
        cfg.engagementThreshold = 0.55
        cfg.issuer = "did:example:integrator"
        cfg.sessionId = "session-123"
        cfg.useMotionSensors = true
        cfg.companionEvidenceRefs = ["ref-1", "ref-2"]

        let encoded = try JSONEncoder().encode(cfg)
        let decoded = try JSONDecoder().decode(SentlogConfig.self, from: encoded)
        XCTAssertEqual(cfg, decoded)
    }
}
