import Foundation

/// Events emitted by the SDK. Fired only when the integrator supplied
/// a callback at ``Sentlog/initialize(config:onTelemetry:)``. The list
/// of cases is exhaustive — the SDK never emits anything not enumerated
/// here.
///
/// No case ever carries audio, transcript text, video frames, or any
/// user-identifying field.
///
/// Encoding follows the discriminator pattern: the JSON object carries
/// a `type` field matching one of the case names, plus the case's
/// payload at the top level. The decoder peeks at `type` to route to
/// the right case.
public enum TelemetryEvent: Sendable, Equatable {

    case pipelineTiming(stage: String, durationMs: Int)
    case scoreHistogram(field: String, bucket: String)
    case forwarderOutcome(forwarderName: String, outcome: ForwarderOutcomeKind, attempts: Int)
    case manifestVerifyFailed(reason: String, attemptedVersion: String)
    case modelLoadError(model: String, reason: String)
    case permissionDenied(permission: PermissionKind)
    case captureAborted(reason: String)
    case hardwareSigningUnavailable(reason: String, fellBackTo: String)
    case tsaRequestFailed(tsaUrl: String, reason: String)

    public enum ForwarderOutcomeKind: String, Codable, Sendable {
        case ok
        case retry
        case dropped
    }

    public enum PermissionKind: String, Codable, Sendable {
        case microphone
        case motion
        case camera
    }
}

extension TelemetryEvent: Codable {

    private enum DiscriminatorKey: String, CodingKey { case type }

    private enum PayloadKey: String, CodingKey {
        case stage, bucket, field, reason, model, permission, outcome
        case attempts
        case durationMs        = "duration_ms"
        case forwarderName     = "forwarder_name"
        case attemptedVersion  = "attempted_version"
        case fellBackTo        = "fell_back_to"
        case tsaUrl            = "tsa_url"
    }

    public init(from decoder: Decoder) throws {
        let d = try decoder.container(keyedBy: DiscriminatorKey.self)
        let kind = try d.decode(String.self, forKey: .type)
        let p = try decoder.container(keyedBy: PayloadKey.self)
        switch kind {
        case "PipelineTiming":
            self = .pipelineTiming(
                stage: try p.decode(String.self, forKey: .stage),
                durationMs: try p.decode(Int.self, forKey: .durationMs))
        case "ScoreHistogram":
            self = .scoreHistogram(
                field: try p.decode(String.self, forKey: .field),
                bucket: try p.decode(String.self, forKey: .bucket))
        case "ForwarderOutcome":
            self = .forwarderOutcome(
                forwarderName: try p.decode(String.self, forKey: .forwarderName),
                outcome: try p.decode(ForwarderOutcomeKind.self, forKey: .outcome),
                attempts: try p.decode(Int.self, forKey: .attempts))
        case "ManifestVerifyFailed":
            self = .manifestVerifyFailed(
                reason: try p.decode(String.self, forKey: .reason),
                attemptedVersion: try p.decode(String.self, forKey: .attemptedVersion))
        case "ModelLoadError":
            self = .modelLoadError(
                model: try p.decode(String.self, forKey: .model),
                reason: try p.decode(String.self, forKey: .reason))
        case "PermissionDenied":
            self = .permissionDenied(
                permission: try p.decode(PermissionKind.self, forKey: .permission))
        case "CaptureAborted":
            self = .captureAborted(reason: try p.decode(String.self, forKey: .reason))
        case "HardwareSigningUnavailable":
            self = .hardwareSigningUnavailable(
                reason: try p.decode(String.self, forKey: .reason),
                fellBackTo: try p.decode(String.self, forKey: .fellBackTo))
        case "TsaRequestFailed":
            self = .tsaRequestFailed(
                tsaUrl: try p.decode(String.self, forKey: .tsaUrl),
                reason: try p.decode(String.self, forKey: .reason))
        default:
            throw DecodingError.dataCorruptedError(
                forKey: .type, in: d,
                debugDescription: "Unknown telemetry event type: \(kind)")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var d = encoder.container(keyedBy: DiscriminatorKey.self)
        var p = encoder.container(keyedBy: PayloadKey.self)
        switch self {
        case .pipelineTiming(let stage, let durationMs):
            try d.encode("PipelineTiming", forKey: .type)
            try p.encode(stage, forKey: .stage)
            try p.encode(durationMs, forKey: .durationMs)
        case .scoreHistogram(let field, let bucket):
            try d.encode("ScoreHistogram", forKey: .type)
            try p.encode(field, forKey: .field)
            try p.encode(bucket, forKey: .bucket)
        case .forwarderOutcome(let name, let outcome, let attempts):
            try d.encode("ForwarderOutcome", forKey: .type)
            try p.encode(name, forKey: .forwarderName)
            try p.encode(outcome, forKey: .outcome)
            try p.encode(attempts, forKey: .attempts)
        case .manifestVerifyFailed(let reason, let version):
            try d.encode("ManifestVerifyFailed", forKey: .type)
            try p.encode(reason, forKey: .reason)
            try p.encode(version, forKey: .attemptedVersion)
        case .modelLoadError(let model, let reason):
            try d.encode("ModelLoadError", forKey: .type)
            try p.encode(model, forKey: .model)
            try p.encode(reason, forKey: .reason)
        case .permissionDenied(let kind):
            try d.encode("PermissionDenied", forKey: .type)
            try p.encode(kind, forKey: .permission)
        case .captureAborted(let reason):
            try d.encode("CaptureAborted", forKey: .type)
            try p.encode(reason, forKey: .reason)
        case .hardwareSigningUnavailable(let reason, let fellBackTo):
            try d.encode("HardwareSigningUnavailable", forKey: .type)
            try p.encode(reason, forKey: .reason)
            try p.encode(fellBackTo, forKey: .fellBackTo)
        case .tsaRequestFailed(let url, let reason):
            try d.encode("TsaRequestFailed", forKey: .type)
            try p.encode(url, forKey: .tsaUrl)
            try p.encode(reason, forKey: .reason)
        }
    }
}
