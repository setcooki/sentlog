# Scoring weights

`SentlogConfig.scoring_weights` is an optional `Map<String, Float>?`
that lets integrators override the per-component weights the pipeline
uses to assemble composite scores. Each value is a real number in
`[0.0, 10.0]`. Defaults are published below; the implementation lives
in [`shared/src/internal/config.cpp`](../shared/src/internal/config.cpp).

Unrecognised keys are rejected at `Sentlog.initialize(...)` — typos
surface there, not silently later. See [decision row
1.21](technical-specification.md) in the spec.

## Defaults

| Key | Default | What it weights | Rationale |
|---|---|---|---|
| `liveness.replay` | `1.0` | AASIST-L's per-utterance replay-attack risk | Surfaced as-is; AASIST-L's outputs are already calibrated. |
| `liveness.synthetic` | `1.0` | AASIST-L's per-utterance synthetic/TTS risk | Same reasoning. A future replay-vs-synthetic split lands here. |
| `engagement.voice` | `1.0` | Voice-side engagement (prosody + whisper-encoder probe) | Only signal source today; inner ensembling happens before this weight. |
| `engagement.face` | `0.5` | Video-affect engagement (Phase 5+) | Video is noisier across lighting / camera angles than voice; integrators with controlled capture can crank to 1.0. |
| `presence.audio_source` | `1.0` | Audio input route (built-in mic / USB / Bluetooth / loopback) | Most direct signal of where the audio came from. |
| `presence.motion` | `0.7` | Motion-coherence (Phase 3c+) | Informative but easier to spoof (record-and-shake) than the audio route. |
| `presence.integrity` | `1.0` | Play Integrity / DeviceCheck / AppAttest (Phase 7+) | When present, hardware-attested integrity is very high-signal. |

## Override examples

Crank up motion-coherence on a controlled deployment where you trust
the capture environment:

```json
{
  "issuer": "did:example:org",
  "scoring_weights": {
    "presence.motion": 1.0,
    "engagement.face": 0.9
  }
}
```

Treat synthetic-voice attacks as a harder fail than replay (e.g.
deepfake-driven fraud is the dominant concern):

```json
{
  "scoring_weights": {
    "liveness.synthetic": 1.5
  }
}
```

## What `thresholds_met` actually checks

`thresholds_met` is the only field that consumes threshold values
(per the spec's "raw scores in, no SDK-side calibration" rule). The
scoring weights affect the composite `presence.summary` and
`engagement.score` fields directly — they do not alter individual
score components, which always stay as the model's raw probability
output.

## When to override

- **Don't** as a way to dial down a model whose precision you're not
  happy with. The fix is a better model or a confidence threshold,
  not a weight kludge.
- **Do** when your capture environment changes the trust profile of
  a signal — controlled-environment integrators can weight face
  affect higher; field deployments may weight motion lower.
- **Do** for A/B tests of ensemble shape. Different weight sets can
  be deployed via the integrator's normal config-rollout machinery.

## Bumping the defaults

When the published defaults change:

1. Update `kPublishedWeights()` in
   [`shared/src/internal/config.cpp`](../shared/src/internal/config.cpp).
2. Update the table above in the same commit.
3. Note the change in the next release's changelog so integrators
   relying on the old defaults can re-tune.

Defaults can only change at a major version of the published
weights set; we treat them as part of the public API.
