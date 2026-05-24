import XCTest
@testable import Sentlog

/// Validates that the SentlogResult Swift model deserialises the exact
/// JSON shape the C++ core emits. The reference JSON below matches
/// `docs/technical-specification.md §3.2` field by field.
final class SentlogResultDeserializationTests: XCTestCase {

    func testFullResultDeserialises() throws {
        let refJson = """
        {
          "schema_version": "1",
          "transcript": {
            "text": "hello world", "language": "en", "confidence": 0.92,
            "word_timings": [
              {"word": "hello", "start_ms": 100, "end_ms": 500},
              {"word": "world", "start_ms": 600, "end_ms": 1000}
            ],
            "target_match": 0.95
          },
          "liveness": {"score": 0.88, "replay_risk": 0.05, "synthetic_risk": 0.07},
          "presence": {"summary": 0.9, "audio_source": 1.0, "motion": 0.85, "integrity": null},
          "direction": 0.91, "confidence": 0.87, "thresholds_met": true,
          "engagement": {"score": 0.82, "voice_score": 0.82, "face_score": null, "model_status": "training"},
          "affect": null,
          "fingerprint": {"algo": "sentlog-pcp-v1", "bytes": "Zm9vYmFyYmF6"},
          "model_versions": {"liveness": "0.0.0-phase1", "asr": "0.0.0-phase1", "prosody": "0.0.0-phase1", "vad": "0.0.0-phase1", "manifest": "0.0.0-phase1"},
          "diagnostics": {"duration_ms": 8500, "audio_quality": "moderate"},
          "evidence_grade": "DeviceUnattested",
          "extensions": {}
        }
        """
        let data = refJson.data(using: .utf8)!
        let result = try JSONDecoder().decode(SentlogResult.self, from: data)

        XCTAssertEqual(result.schemaVersion, "1")
        XCTAssertEqual(result.transcript.text, "hello world")
        XCTAssertEqual(result.transcript.wordTimings?.count, 2)
        XCTAssertEqual(result.evidenceGrade, .deviceUnattested)
        XCTAssertEqual(result.engagement.modelStatus, .training)
        XCTAssertEqual(result.diagnostics.audioQuality, .moderate)
        XCTAssertEqual(result.fingerprint.algo, "sentlog-pcp-v1")
        XCTAssertNotNil(result.presence.motion)
    }

    func testPermissionsDeserialiseWithSnakeCaseStateNames() throws {
        let data = """
        {"microphone": "granted", "motion": "not_requested", "camera": "not_requested"}
        """.data(using: .utf8)!
        let p = try JSONDecoder().decode(Permissions.self, from: data)
        XCTAssertEqual(p.microphone, .granted)
        XCTAssertEqual(p.motion, .notRequested)
        XCTAssertEqual(p.camera, .notRequested)
    }

    func testCapabilitiesDeserialiseWithAllRuntimeVariants() throws {
        for runtime in ["native", "capacitor", "react_native", "web_fallback"] {
            let data = """
            {
              "audio_capture": true, "motion_sensors": false, "video_face_mesh": false,
              "device_integrity_check": false, "ota_updates": true, "local_queue": true,
              "runtime": "\(runtime)"
            }
            """.data(using: .utf8)!
            _ = try JSONDecoder().decode(Capabilities.self, from: data)
        }
    }
}
