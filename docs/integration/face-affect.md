# Face affect (video)

Optional capability: in addition to voice, the SDK can incorporate
**video facial-affect** signals into the engagement score. Set
`SentlogConfig.use_video_affect = true` and push one
`AffectFrame{valence, arousal, dominantFauCsv}` per camera frame
via `Sentlog.pushFaceFrame(...)`. The SDK aggregates over the
capture window into `Affect{valence, arousal, dominant_fau}` on
the result + contributes to `engagement.score` via the
`engagement.face` weight.

Voice-only is the default + remains a first-class citizen. There is
no permission inflation: adding face affect requires the camera
permission, and only then. Face landmarks **never reach the SDK** —
only the (valence, arousal, FAU) tuple crosses the boundary, per
spec §5.3.

## Pipeline shape

```
camera frame  ──►  face-landmark detector  ──►  468 landmarks
                  (MediaPipe Face Mesh on Android,
                   Vision face-landmark request on iOS)

468 landmarks  ──►  FaceAffectHead.infer()  ──►  AffectFrame
                  (the platform-side affect MLP)

AffectFrame  ──►  Sentlog.pushFaceFrame(ts, v, a, faus)
                  (raw landmarks discarded; only the derived
                   tuple crosses the SDK boundary)
```

The SDK ships the `FaceAffectHead` interface + a
`PlaceholderFaceAffectHead` returning neutral values
(`valence = 0, arousal = 0.5, FAU = null`) so integrators can wire
the pipeline end-to-end today; the trained head lands in months
7–12 alongside the labelled affect corpus and is swapped in by
changing one line.

## What the SDK does *not* own

By design:

- **Camera lifecycle.** Every app has its own preview surface,
  rate budget, foreground/background handling. The SDK doesn't try
  to abstract that — the integrator drives the camera with whatever
  framework they're already using (CameraX on Android, AVFoundation
  on iOS).
- **Face-landmark detection.** MediaPipe Face Mesh is the
  reference choice on Android; the iOS Vision framework's
  `VNDetectFaceLandmarksRequest` covers the iOS case. Both run
  on-device; the integrator picks the rate (10–15 Hz typical),
  ROI, and lifecycle policy.
- **Bundling MediaPipe / Vision as a hard dep.** Neither library
  is in the SDK's Gradle / Podfile. Integrators add them to their
  own app if they want face affect; the SDK exposes the boundary,
  not the dependency footprint.

## Android worked example

```kotlin
import me.cooki.sentlog.AffectFrame
import me.cooki.sentlog.FaceAffectHead
import me.cooki.sentlog.PlaceholderFaceAffectHead
import me.cooki.sentlog.Sentlog

// At app init, pick an affect head. Today: placeholder. Later: the
// trained model with no other code changes.
val affectHead: FaceAffectHead = PlaceholderFaceAffectHead()

// In your CameraX ImageAnalysis callback, after MediaPipe inference:
fun onFaceMeshLandmarks(timestampNs: Long, landmarks: FloatArray) {
    val frame = affectHead.infer(landmarks) ?: return
    Sentlog.pushFaceFrame(
        timestampNs   = timestampNs,
        valence       = frame.valence,
        arousal       = frame.arousal,
        dominantFauCsv = frame.dominantFauCsv,
    )
}
```

The landmark array is MediaPipe's standard 468-point layout
(1404 floats: x, y, z per landmark).

## iOS worked example

```swift
import Sentlog

let affectHead: FaceAffectHead = PlaceholderFaceAffectHead()

// In your Vision request handler, after
// VNDetectFaceLandmarksRequest completes:
func onFaceLandmarks(timestampNs: Int64, landmarks: [Float]) {
    guard let frame = affectHead.infer(landmarks: landmarks) else { return }
    try? Sentlog.pushFaceFrame(
        timestampNs:    timestampNs,
        valence:        frame.valence,
        arousal:        frame.arousal,
        dominantFauCsv: frame.dominantFauCsv)
}
```

Vision returns landmarks in normalised face-bounding-box
coordinates; map to whatever input layout your `FaceAffectHead`
implementation expects. The bundled
`PlaceholderFaceAffectHead` accepts any non-empty array.

## Rate budget

10–15 Hz is the recommended target — high enough to catch microbursts
of expression, low enough to leave headroom for the rest of the
capture pipeline on mid-tier devices. The SDK caps the per-evaluate
face frame buffer at 6000 frames (60 s @ 100 Hz upper bound); excess
frames are dropped silently with an overflow counter.

## What appears in the result

When `use_video_affect = true` AND face frames were pushed during
capture:

```jsonc
{
  "affect": {
    "valence":      0.21,       // mean across captured frames
    "arousal":      0.58,
    "dominant_fau": ["AU6", "AU12"]   // FAUs in ≥25 % of frames
  },
  "engagement": {
    "face_score":   0.42,       // derived from valence + arousal
    "voice_score":  0.51,
    "score":        0.47        // weighted by scoring_weights.engagement.face
  }
}
```

When the gate is off OR no frames were pushed, `affect` is `null`
and `engagement.face_score` is absent.
