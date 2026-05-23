import Foundation

/// The result returned by ``Sentlog/evaluate()``. Mirrors
/// `docs/technical-specification.md §3.2`.
///
/// All score fields are raw [0,1] probability outputs. The SDK applies
/// no calibration or capping. ``thresholdsMet`` is the only field
/// that consults the threshold values in ``SentlogConfig``.
public struct SentlogResult: Codable, Sendable, Equatable {
    public let schemaVersion:  String
    public let transcript:     Transcript
    public let liveness:       Liveness
    public let presence:       Presence
    public let direction:      Float
    public let confidence:     Float
    public let thresholdsMet:  Bool
    public let engagement:     Engagement
    public let affect:         Affect?
    public let fingerprint:    AudioFingerprint
    public let modelVersions:  ModelVersions
    public let diagnostics:    Diagnostics
    public let evidenceGrade:  EvidenceGrade
    public let extensions:     [String: JSONValue]

    private enum CodingKeys: String, CodingKey {
        case transcript, liveness, presence, direction, confidence
        case engagement, affect, fingerprint, diagnostics, extensions
        case schemaVersion  = "schema_version"
        case thresholdsMet  = "thresholds_met"
        case modelVersions  = "model_versions"
        case evidenceGrade  = "evidence_grade"
    }
}

public enum EvidenceGrade: String, Codable, Sendable {
    case deviceUnattested = "DeviceUnattested"
    case deviceAttested   = "DeviceAttested"
    case enclaveSigned    = "EnclaveSigned"
}

public struct Transcript: Codable, Sendable, Equatable {
    public let text:         String
    public let language:     String
    public let confidence:   Float
    public let wordTimings:  [WordTiming]?
    public let targetMatch:  Float?

    private enum CodingKeys: String, CodingKey {
        case text, language, confidence
        case wordTimings = "word_timings"
        case targetMatch = "target_match"
    }
}

public struct WordTiming: Codable, Sendable, Equatable {
    public let word:    String
    public let startMs: Int
    public let endMs:   Int

    private enum CodingKeys: String, CodingKey {
        case word
        case startMs = "start_ms"
        case endMs   = "end_ms"
    }
}

public struct Liveness: Codable, Sendable, Equatable {
    public let score:         Float
    public let replayRisk:    Float
    public let syntheticRisk: Float

    private enum CodingKeys: String, CodingKey {
        case score
        case replayRisk    = "replay_risk"
        case syntheticRisk = "synthetic_risk"
    }
}

public struct Presence: Codable, Sendable, Equatable {
    public let summary:      Float
    public let audioSource:  Float
    public let motion:       Float?
    public let integrity:    Float?

    private enum CodingKeys: String, CodingKey {
        case summary, motion, integrity
        case audioSource = "audio_source"
    }
}

public struct Engagement: Codable, Sendable, Equatable {
    public let score:        Float
    public let voiceScore:   Float
    public let faceScore:    Float?
    public let modelStatus:  EngagementModelStatus

    private enum CodingKeys: String, CodingKey {
        case score
        case voiceScore  = "voice_score"
        case faceScore   = "face_score"
        case modelStatus = "model_status"
    }
}

public enum EngagementModelStatus: String, Codable, Sendable {
    case placeholder
    case training
    case trained
}

public struct Affect: Codable, Sendable, Equatable {
    public let valence:      Float
    public let arousal:      Float
    public let dominantFau:  [String]?

    private enum CodingKeys: String, CodingKey {
        case valence, arousal
        case dominantFau = "dominant_fau"
    }
}

public struct AudioFingerprint: Codable, Sendable, Equatable {
    public let algo:   String
    public let bytes:  String   // base64 of the raw fingerprint bytes
}

public struct Diagnostics: Codable, Sendable, Equatable {
    public let failureReason:  FailureReason?
    public let snrDb:          Float?
    public let durationMs:     Int
    public let audioQuality:   AudioQuality

    private enum CodingKeys: String, CodingKey {
        case failureReason = "failure_reason"
        case snrDb         = "snr_db"
        case durationMs    = "duration_ms"
        case audioQuality  = "audio_quality"
    }
}

public enum FailureReason: String, Codable, Sendable {
    case tooShort         = "TooShort"
    case tooNoisy         = "TooNoisy"
    case noSpeechDetected = "NoSpeechDetected"
    case languageMismatch = "LanguageMismatch"
    case interrupted      = "Interrupted"
    case internalError    = "InternalError"
}

public enum AudioQuality: String, Codable, Sendable {
    case quiet
    case moderate
    case noisy
    case veryNoisy = "very_noisy"
}

public struct ModelVersions: Codable, Sendable, Equatable {
    public let liveness: String
    public let asr:      String
    public let prosody:  String
    public let affect:   String?
    public let manifest: String
}

public struct UpdateResult: Codable, Sendable, Equatable {
    public let checkedAtMs:     Int64
    public let updateFound:     Bool
    public let previousVersion: String
    public let newVersion:      String?
    public let applied:         Bool
    public let error:           String?

    private enum CodingKeys: String, CodingKey {
        case applied, error
        case checkedAtMs     = "checked_at_ms"
        case updateFound     = "update_found"
        case previousVersion = "previous_version"
        case newVersion      = "new_version"
    }
}

public struct Permissions: Codable, Sendable, Equatable {
    public let microphone: PermissionState
    public let motion:     PermissionState
    public let camera:     PermissionState
}

public enum PermissionState: String, Codable, Sendable {
    case granted
    case denied
    case prompt
    case notRequested = "not_requested"
}

public struct Capabilities: Codable, Sendable, Equatable {
    public let audioCapture:         Bool
    public let motionSensors:        Bool
    public let videoFaceMesh:        Bool
    public let deviceIntegrityCheck: Bool
    public let otaUpdates:           Bool
    public let localQueue:           Bool
    public let runtime:              Runtime

    private enum CodingKeys: String, CodingKey {
        case runtime
        case audioCapture         = "audio_capture"
        case motionSensors        = "motion_sensors"
        case videoFaceMesh        = "video_face_mesh"
        case deviceIntegrityCheck = "device_integrity_check"
        case otaUpdates           = "ota_updates"
        case localQueue           = "local_queue"
    }
}

public enum Runtime: String, Codable, Sendable {
    case native
    case capacitor
    case reactNative = "react_native"
    case webFallback = "web_fallback"
}
