import Foundation

/// Phase 5c: contract for the platform-side face-affect head.
///
/// The integrator runs face-landmark detection (typically
/// `VNDetectFaceLandmarksRequest` on iOS) and hands the landmark
/// array to a `FaceAffectHead` implementation, which returns the
/// derived (valence, arousal, FAU) tuple. The integrator then pushes
/// that tuple via `Sentlog.pushFaceFrame(...)` — raw landmarks never
/// reach the SDK (spec §5.3).
///
/// Why this lives behind a protocol, not as a bundled Vision
/// orchestrator: every app has its own camera preview surface,
/// AVCaptureSession lifecycle, and rate budget. The SDK doesn't try
/// to own that. What we bundle is the affect-head MLP itself; that's
/// the part with a clear "given landmarks, predict valence/arousal/
/// FAU" contract.
///
/// The bundled MLP isn't trained yet — that needs labelled affect
/// data which the rollout plan gates to months 7–12. Until then,
/// `PlaceholderFaceAffectHead` returns neutral values so integrators
/// can wire the full pipeline end-to-end against the real envelope
/// shape, then swap in the trained head when it lands without
/// changing application code.
public protocol FaceAffectHead {

    /// Map a single frame of face landmarks to an `AffectFrame`.
    /// Landmarks may be normalised face-bounding-box coordinates
    /// (Vision's `VNFaceLandmarkRegion2D`) or any other layout the
    /// concrete head expects — implementations document their
    /// contract. The frame's wall-clock timestamp is the caller's
    /// responsibility; pass it on to `Sentlog.pushFaceFrame`.
    ///
    /// Return nil when no inference was possible (no face detected,
    /// inference failed). The caller skips the frame on nil.
    func infer(landmarks: [Float]) -> AffectFrame?
}

/// Single-frame affect output. Returned by `FaceAffectHead.infer`;
/// fed into `Sentlog.pushFaceFrame` alongside the timestamp.
public struct AffectFrame: Sendable, Equatable {
    /// [-1, 1] — negative-to-positive emotional valence.
    public let valence: Float
    /// [0, 1] — calm-to-excited arousal.
    public let arousal: Float
    /// Comma-separated FACS action units active in this frame
    /// (e.g. `"AU6,AU12"` for smile). Nil when no dominant FAU.
    public let dominantFauCsv: String?

    public init(valence: Float, arousal: Float, dominantFauCsv: String? = nil) {
        self.valence        = valence
        self.arousal        = arousal
        self.dominantFauCsv = dominantFauCsv
    }
}

/// Phase 5c deferral: the actual affect-head MLP needs training data
/// gated to months 7–12 per the rollout plan. Until that ships, this
/// placeholder returns neutral values (valence = 0, arousal = 0.5,
/// no dominant FAU) for every frame so:
///
///   - integrators can wire face capture + this head + the SDK
///     end-to-end and verify the result/envelope shape today;
///   - swapping in the real head later changes one line
///     (`FaceAffectHead = PlaceholderFaceAffectHead()` →
///     `FaceAffectHead = SentlogAffectHead(modelAsset: ...)`)
///     without touching capture or SDK code.
///
/// The `model_versions.affect` field in the SentlogResult reports
/// `"placeholder-derived"` so downstream consumers can detect that
/// the affect values came from the stub rather than the real model.
public struct PlaceholderFaceAffectHead: FaceAffectHead, Sendable {
    public init() {}
    public func infer(landmarks: [Float]) -> AffectFrame? {
        guard !landmarks.isEmpty else { return nil }
        return AffectFrame(valence: 0.0, arousal: 0.5, dominantFauCsv: nil)
    }
}
