# ADR 0002 — whisper.cpp tiny Q5_1 transcription stack

**Status:** Accepted
**Date:** 2026-05-22
**Spec reference:** [§1.2](../technical-specification.md#1-architectural-decisions)

## Context

On-device transcription is required for direction scoring (matching
spoken content against `target_phrase`), for transcript exposure
(first-class output), and as a feature extractor for the engagement
head (a linear probe over the encoder's hidden state).

Two transcription stacks were considered:

1. **whisper.cpp tiny Q5_1 on both platforms.** Single quantisation
   pipeline, single set of golden fixtures, C++ across the board.
2. **whisper.cpp on Android + an Apple-platform-native alternative on
   iOS** that pushes both encoder and decoder through Core ML. Faster
   on modern A-series but doubles the ASR codepaths.

## Decision

**whisper.cpp tiny Q5_1 on both platforms.** On iOS we use the
official `whisper.cpp` Core ML build (`-DWHISPER_COREML=1`), which
pushes the encoder through Core ML / Apple Neural Engine while the
decoder runs on CPU/Metal via ggml. On Android, whisper.cpp uses
ggml's CPU NEON path (and Vulkan delegate where supported) — it does
**not** use TFLite/NNAPI.

Quantisation: **Q5_1.** Sweet spot for tiny — near-F16 WER at ~40%
size, modest speed gain on ARM.

Encoder context: **variable-length encoder context** (ACFT-style).
The largest single performance lever for mid-range Android — roughly
2× speedup on a 15-second clip versus the default 30s padded encoder.

An Apple-platform-native ASR alternative remains documented as an
iOS-only escape valve if iOS performance complaints surface in
production (see the §9.4 escape valves in the rollout plan).

## Consequences

**Enabled:**

- One quantisation pipeline, one set of golden fixtures.
- The encoder's hidden state is available as a feature stream for the
  engagement probe (ADR 0001 + the engagement architecture decision).
- WASM compile path (whisper.cpp has mature WASM builds) keeps Phase 9
  realistic.

**Constrained:**

- Multi-language quality is bounded by whisper tiny — fine for English
  in clean conditions; degrades on heavily accented or noisy audio.
  Phase 2 includes per-language gating with reduced confidence as a
  guardrail.
- The Android benchmark gate (Phase 1) is the empirical check that
  this stack hits the latency budget on Dimensity 700-class hardware.

## Open follow-ups

- Validate the iOS performance margin against an Apple-platform-native
  ASR alternative once the iOS build is real, so we know how much
  headroom we'd have by switching iOS if forced.
- whisper.cpp, Silero VAD, AASIST-L all run via the same ONNX Runtime
  Mobile / ggml integration. Distribute the C++ runtime once and
  amortise across all three. The Phase 1 chunk that integrates
  ONNX Runtime (Silero + AASIST-L) lands the cross-platform binary
  distribution that whisper.cpp will then build on top of.
