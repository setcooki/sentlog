# Sentlog benchmark harness

Offline runner that walks a labelled corpus of WAV clips, invokes a
runner-of-your-choice per clip, and emits per-attack-family
aggregates (score histograms, FAR/FRR estimates, latency p50/p95).

Phase 8 scaffolding: the actual 50k+ clip corpus is gated to
months 7–12 per the rollout plan, but the data-plumbing +
aggregation logic ships now so when the corpus arrives the numbers
come fast.

## Corpus layout

```
corpus_root/
├── manifest.csv          # one row per clip, header required
└── clips/
    ├── 0001.wav
    └── ...
```

`manifest.csv` columns:

| column            | meaning |
|-------------------|---------|
| `clip_id`         | opaque stable identifier |
| `path`            | relative to corpus_root |
| `family`          | one of `genuine`, `replay`, `tts`, `voice_clone`, `voice_clone_telephony`, `other` |
| `ground_truth_live` | `1` if genuine, `0` if spoof |
| `source`          | provenance tag (cohort, dataset, etc.) |

## Running

```bash
# Synthetic mode (no --runner): deterministic stub returns synthetic
# scores from the manifest's labels. Used by selftest + for testing
# the aggregation math before a real runner exists.
python3 tools/benchmark-harness/harness.py path/to/corpus_root

# Real mode: --runner is an executable that takes the WAV path on
# argv and writes a SentlogResult JSON to stdout.
python3 tools/benchmark-harness/harness.py \
    --runner ./my-sentlog-runner \
    --threshold 0.7 \
    --json-out report.json \
    path/to/corpus_root
```

A reference runner that wraps the C++ shared core via the C ABI is
planned for Phase 8 alongside the corpus; today the harness expects
the integrator to supply one. The runner contract is intentionally
minimal: one process per clip, single argv = WAV path, stdout = JSON
SentlogResult. This lets you point it at any execution environment
(SDK binary, Python ctypes wrapper, even a remote service).

## Output

- **stdout**: human-readable markdown report (per-family histograms
  + latency summary + headline FAR/FRR).
- **`--json-out PATH`**: structured aggregates as JSON, suitable for
  diffing in CI to spot regressions across model versions.

## FAR / FRR

Computed against the `--threshold` value (default 0.7):

- **FAR** (False Accept Rate) = spoof clips that scored at or above
  threshold on `liveness.score`.
- **FRR** (False Reject Rate) = genuine clips that scored below
  threshold.

These are corpus-wide; per-family FAR is the conventional metric for
per-attack reporting and can be derived from the per-family
histograms.

## Self-test

```bash
python3 tools/benchmark-harness/selftest.py
```

Builds a synthetic 13-clip corpus in a temp dir, runs the harness
end-to-end with the synthetic stub, and asserts on the aggregation
output. Stdlib-only; runs in <1s. CI runs this on every PR.
