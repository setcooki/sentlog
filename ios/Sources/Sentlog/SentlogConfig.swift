import Foundation
import SentlogCore

/// SentlogConfig — passed once to ``Sentlog/initialize(config:onTelemetry:)``.
///
/// Mirrors `docs/technical-specification.md §3.3` and the Kotlin
/// equivalent in `android/sentlog/.../SentlogConfig.kt`. The JSON wire
/// shape uses snake_case so the C++ core's parser (the same one both
/// platforms target) recognises every field.
///
/// When at least one ``Forwarder`` is configured, ``issuer`` becomes
/// mandatory — the C++ core rejects the config at init time otherwise.
public struct SentlogConfig: Codable, Sendable, Equatable {

    // MARK: thresholds

    public var livenessThreshold:   Float = 0.7
    public var engagementThreshold: Float = 0.5
    public var directionThreshold:  Float = 0.6
    public var minConfidence:       Float = 0.6

    // MARK: pipeline

    public var challengeEnabled: Bool   = true
    public var languageHint:     String = "en"
    public var minDurationMs:    Int    = 5_000
    public var maxDurationMs:    Int    = 60_000
    public var targetPhrase:     String?

    // MARK: multi-modal (all opt-in)

    public var useMotionSensors:   Bool = false
    public var useVideoAffect:     Bool = false
    public var useDeviceIntegrity: Bool = false

    // MARK: evidence envelope (Verifiable Credentials)

    public var issuer:                   String = ""
    public var signingKeyId:             String?
    public var useHardwareSigning:       Bool   = false
    public var hardwareSigningRequired:  Bool   = false

    // MARK: optional RFC 3161 TSA

    public var tsaEndpoint:  String?
    public var tsaTimeoutMs: Int  = 5_000
    public var tsaRequired:  Bool = false

    // MARK: session metadata

    public var sessionId:              String?
    public var companionEvidenceRefs:  [String] = []

    // MARK: output pipeline

    public var forwarders: [Forwarder] = []

    // MARK: OTA model updates (spec §1.20)
    //
    // Defaults to the published Sentlog manifest. Overriding requires
    // overriding BOTH together; the C++ core rejects mixed-source
    // configuration at init. `manifestPubkeys` is `[active, standby]`
    // — exactly two entries, each the base64-encoded raw 32-byte
    // Ed25519 pubkey. Validated at `Sentlog.initialize` time via
    // `validate()` below.
    public var manifestUrl:     String?
    public var manifestPubkeys: [String]?

    // MARK: scoring (docs/scoring-weights.md)

    /// Overrides for the published defaults; nil keeps them all.
    public var scoringWeights: [String: Float]?

    public init() {}

    /// Absent keys keep the defaults above. Synthesized Decodable would
    /// require every key, but JS bridge configs are mostly optional.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func read<T: Decodable>(_ key: CodingKeys, _ fallback: T) throws -> T {
            try c.decodeIfPresent(T.self, forKey: key) ?? fallback
        }
        livenessThreshold       = try read(.livenessThreshold, livenessThreshold)
        engagementThreshold     = try read(.engagementThreshold, engagementThreshold)
        directionThreshold      = try read(.directionThreshold, directionThreshold)
        minConfidence           = try read(.minConfidence, minConfidence)
        challengeEnabled        = try read(.challengeEnabled, challengeEnabled)
        languageHint            = try read(.languageHint, languageHint)
        minDurationMs           = try read(.minDurationMs, minDurationMs)
        maxDurationMs           = try read(.maxDurationMs, maxDurationMs)
        targetPhrase            = try c.decodeIfPresent(String.self, forKey: .targetPhrase)
        useMotionSensors        = try read(.useMotionSensors, useMotionSensors)
        useVideoAffect          = try read(.useVideoAffect, useVideoAffect)
        useDeviceIntegrity      = try read(.useDeviceIntegrity, useDeviceIntegrity)
        issuer                  = try read(.issuer, issuer)
        signingKeyId            = try c.decodeIfPresent(String.self, forKey: .signingKeyId)
        useHardwareSigning      = try read(.useHardwareSigning, useHardwareSigning)
        hardwareSigningRequired = try read(.hardwareSigningRequired, hardwareSigningRequired)
        tsaEndpoint             = try c.decodeIfPresent(String.self, forKey: .tsaEndpoint)
        tsaTimeoutMs            = try read(.tsaTimeoutMs, tsaTimeoutMs)
        tsaRequired             = try read(.tsaRequired, tsaRequired)
        sessionId               = try c.decodeIfPresent(String.self, forKey: .sessionId)
        companionEvidenceRefs   = try read(.companionEvidenceRefs, companionEvidenceRefs)
        forwarders              = try read(.forwarders, forwarders)
        manifestUrl             = try c.decodeIfPresent(String.self, forKey: .manifestUrl)
        manifestPubkeys         = try c.decodeIfPresent([String].self, forKey: .manifestPubkeys)
        scoringWeights          = try c.decodeIfPresent([String: Float].self, forKey: .scoringWeights)
    }

    /// Throws if any field is in a structurally invalid state. Called
    /// from `Sentlog.initialize` so the integrator gets the error at
    /// initialization, not when reaching the C++ parser. Cross-field
    /// + business-logic validation stays in the C++ layer (single
    /// source of truth).
    internal func validate() throws {
        if let pks = manifestPubkeys, pks.count != 2 {
            throw SentlogError(
                status: SENTLOG_ERR_INVALID_CONFIG,
                context: "manifestPubkeys must have exactly 2 entries (active, standby); got \(pks.count)")
        }
    }

    private enum CodingKeys: String, CodingKey {
        case livenessThreshold       = "liveness_threshold"
        case engagementThreshold     = "engagement_threshold"
        case directionThreshold      = "direction_threshold"
        case minConfidence           = "min_confidence"
        case challengeEnabled        = "challenge_enabled"
        case languageHint            = "language_hint"
        case minDurationMs           = "min_duration_ms"
        case maxDurationMs           = "max_duration_ms"
        case targetPhrase            = "target_phrase"
        case useMotionSensors        = "use_motion_sensors"
        case useVideoAffect          = "use_video_affect"
        case useDeviceIntegrity      = "use_device_integrity"
        case issuer
        case signingKeyId            = "signing_key_id"
        case useHardwareSigning      = "use_hardware_signing"
        case hardwareSigningRequired = "hardware_signing_required"
        case tsaEndpoint             = "tsa_endpoint"
        case tsaTimeoutMs            = "tsa_timeout_ms"
        case tsaRequired             = "tsa_required"
        case sessionId               = "session_id"
        case companionEvidenceRefs   = "companion_evidence_refs"
        case forwarders
        case manifestUrl             = "manifest_url"
        case manifestPubkeys         = "manifest_pubkeys"
        case scoringWeights          = "scoring_weights"
    }
}

public struct Forwarder: Codable, Sendable, Equatable {
    public var name:        String
    public var projection:  Projection
    public var sampleRate:  Float = 1.0
    public var sink:        JSONValue           // sink-kind + sink-specific fields, opaque
    public var retry:       RetryPolicy = RetryPolicy()
    /// Dotted result paths. Required for `.custom`, rejected otherwise.
    public var customFields: [String]?

    private enum CodingKeys: String, CodingKey {
        case name, projection, sink, retry
        case sampleRate   = "sample_rate"
        case customFields = "custom_fields"
    }

    public init(name: String, projection: Projection, sink: JSONValue,
                sampleRate: Float = 1.0, retry: RetryPolicy = RetryPolicy(),
                customFields: [String]? = nil) {
        self.name = name
        self.projection = projection
        self.sink = sink
        self.sampleRate = sampleRate
        self.retry = retry
        self.customFields = customFields
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name         = try c.decode(String.self, forKey: .name)
        projection   = try c.decode(Projection.self, forKey: .projection)
        sink         = try c.decode(JSONValue.self, forKey: .sink)
        sampleRate   = try c.decodeIfPresent(Float.self, forKey: .sampleRate) ?? 1.0
        retry        = try c.decodeIfPresent(RetryPolicy.self, forKey: .retry) ?? RetryPolicy()
        customFields = try c.decodeIfPresent([String].self, forKey: .customFields)
    }
}

public enum Projection: String, Codable, Sendable {
    case full                 = "Full"
    case fullWithAudioClip    = "Full+AudioClip"
    case scoresOnly           = "ScoresOnly"
    case transcriptOnly       = "TranscriptOnly"
    case evidenceFingerprint  = "EvidenceFingerprint"
    case evidenceClip         = "EvidenceClip"
    case custom               = "Custom"
}

public struct RetryPolicy: Codable, Sendable, Equatable {
    public var maxAttempts:    Int     = 3
    public var initialDelayMs: Int     = 1_000
    public var maxDelayMs:     Int     = 60_000
    public var backoff:        Backoff = .exponential

    public init(maxAttempts: Int = 3, initialDelayMs: Int = 1_000,
                maxDelayMs: Int = 60_000, backoff: Backoff = .exponential) {
        self.maxAttempts = maxAttempts
        self.initialDelayMs = initialDelayMs
        self.maxDelayMs = maxDelayMs
        self.backoff = backoff
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        maxAttempts    = try c.decodeIfPresent(Int.self, forKey: .maxAttempts) ?? 3
        initialDelayMs = try c.decodeIfPresent(Int.self, forKey: .initialDelayMs) ?? 1_000
        maxDelayMs     = try c.decodeIfPresent(Int.self, forKey: .maxDelayMs) ?? 60_000
        backoff        = try c.decodeIfPresent(Backoff.self, forKey: .backoff) ?? .exponential
    }

    private enum CodingKeys: String, CodingKey {
        case backoff
        case maxAttempts    = "max_attempts"
        case initialDelayMs = "initial_delay_ms"
        case maxDelayMs     = "max_delay_ms"
    }
}

public enum Backoff: String, Codable, Sendable {
    case exponential
    case linear
}
