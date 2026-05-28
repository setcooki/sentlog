package me.cooki.sentlog

/**
 * Phase 5b: contract for the platform-side face-affect head.
 *
 * The integrator runs face-mesh inference (MediaPipe Face Mesh, or
 * whatever they prefer) and hands the 468-landmark output to an
 * [FaceAffectHead] implementation, which returns the derived
 * (valence, arousal, FAU) tuple. The integrator then pushes that
 * tuple via [Sentlog.pushFaceFrame] — raw landmarks never reach the
 * SDK (spec §5.3).
 *
 * Why this lives behind an interface, not as a bundled MediaPipe
 * orchestrator: every app has its own camera preview surface,
 * lifecycle, and rate budget. The SDK doesn't try to own that.
 * What we do bundle is the affect-head MLP itself; that's the part
 * with a clear "given landmarks, predict valence/arousal/FAU"
 * contract.
 *
 * The bundled MLP isn't trained yet — that needs labelled affect
 * data which the rollout plan gates to months 7–12. Until then,
 * [PlaceholderFaceAffectHead] returns neutral values so integrators
 * can wire the full pipeline end-to-end against the real envelope
 * shape, then swap in the trained head when it lands without
 * changing application code.
 */
public interface FaceAffectHead {

    /**
     * Map a single frame of face landmarks to an [AffectFrame].
     * Landmarks are MediaPipe Face Mesh 468-point: each Float at
     * index `3*i` / `3*i+1` / `3*i+2` is x / y / z of landmark i.
     * The frame's wall-clock timestamp is the caller's responsibility
     * — pass it on to [Sentlog.pushFaceFrame].
     *
     * Return null when no inference was possible (no face detected,
     * inference failed). The caller skips the frame on null.
     */
    public fun infer(landmarks: FloatArray): AffectFrame?
}

/**
 * Single-frame affect output. Returned by [FaceAffectHead.infer];
 * fed into [Sentlog.pushFaceFrame] alongside the timestamp.
 */
public data class AffectFrame(
    /** [-1, 1] — negative-to-positive emotional valence. */
    public val valence: Float,
    /** [0, 1] — calm-to-excited arousal. */
    public val arousal: Float,
    /** Comma-separated FACS action units active in this frame
     *  (e.g. `"AU6,AU12"` for smile). Null when no dominant FAU. */
    public val dominantFauCsv: String? = null,
)

/**
 * Phase 5b deferral: the actual affect-head MLP needs training data
 * gated to months 7–12 per the rollout plan. Until that ships, this
 * placeholder returns neutral values (valence = 0, arousal = 0.5,
 * no dominant FAU) for every frame so:
 *
 *   - integrators can wire face capture + this head + the SDK
 *     end-to-end and verify the result/envelope shape today;
 *   - swapping in the real head later changes one line
 *     (`FaceAffectHead = PlaceholderFaceAffectHead()` →
 *     `FaceAffectHead = SentlogAffectHead(modelAsset)`) without
 *     touching capture or SDK code.
 *
 * The `model_versions.affect` field in the SentlogResult reports
 * `"placeholder-derived"` so downstream consumers can detect that
 * the affect values came from the stub rather than the real model.
 */
public class PlaceholderFaceAffectHead : FaceAffectHead {
    override fun infer(landmarks: FloatArray): AffectFrame? {
        if (landmarks.isEmpty()) return null
        // 468 landmarks * 3 coords = 1404 floats. We accept anything
        // non-empty to ease testing with simplified fixtures.
        return AffectFrame(valence = 0.0f, arousal = 0.5f,
                           dominantFauCsv = null)
    }
}
