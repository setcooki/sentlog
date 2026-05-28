#!/usr/bin/env python3
"""Sentlog benchmark harness — Phase 8 scaffolding.

Runs the SDK against a labelled corpus of WAV clips and emits
per-attack-family aggregates (score histograms, FAR/FRR estimates,
latency p50/p95). The actual >=50k-clip corpus is gated to months
7–12 per the rollout plan; what ships here is the data-plumbing +
aggregation logic, so when the corpus arrives the numbers come fast.

Corpus layout expected:

    corpus_root/
    ├── manifest.csv              # one row per clip
    └── clips/
        ├── 0001.wav
        ├── 0002.wav
        └── ...

manifest.csv columns (header row required):

    clip_id, path, family, ground_truth_live, source

  family            — one of "genuine", "replay", "tts", "voice_clone",
                      "voice_clone_telephony", "other"
  ground_truth_live — "1" if the clip is a live speaker, "0" if spoofed
  source            — opaque tag for cohort/source provenance

Each clip is handed to a `--runner` executable that reads the WAV
path on its single argv and writes a JSON `SentlogResult` to stdout.
The harness time-tracks the runner call, parses the result, and
aggregates. When `--runner` is unset, a built-in synthetic stub
produces deterministic results from the manifest's labels — used by
the selftest and by integrators wanting to validate the aggregation
math before plugging in a real runner.

Output:
  - stdout: human-readable report (markdown table per family)
  - --json-out PATH: structured aggregates (one JSON object)

Exit codes:
  0 — corpus processed, report emitted
  1 — corpus missing / unreadable
  2 — runner errors past --max-errors threshold
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Iterable


# --- types --------------------------------------------------------

@dataclass
class Clip:
    clip_id:           str
    path:              Path
    family:            str
    ground_truth_live: bool
    source:            str


@dataclass
class Outcome:
    clip:           Clip
    liveness_score: float
    presence_sum:   float
    engagement_sc:  float
    duration_ms:    float
    error:          str | None = None


@dataclass
class FamilyStats:
    family:        str
    n:             int             = 0
    n_errors:      int             = 0
    # Score buckets: 0.0..0.1, 0.1..0.2, ... 0.9..1.0
    liveness_hist: list[int]       = field(default_factory=lambda: [0] * 10)
    presence_hist: list[int]       = field(default_factory=lambda: [0] * 10)
    durations_ms:  list[float]     = field(default_factory=list)


# --- corpus loading -----------------------------------------------

def load_manifest(corpus_root: Path) -> list[Clip]:
    manifest = corpus_root / "manifest.csv"
    if not manifest.exists():
        raise FileNotFoundError(f"missing manifest: {manifest}")
    clips: list[Clip] = []
    with open(manifest, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            clip_path = corpus_root / row["path"]
            clips.append(Clip(
                clip_id           = row["clip_id"],
                path              = clip_path,
                family            = row["family"],
                ground_truth_live = row["ground_truth_live"].strip() in ("1", "true", "True"),
                source            = row.get("source", ""),
            ))
    return clips


# --- runner -------------------------------------------------------

def run_one(clip: Clip, runner_cmd: list[str] | None,
            timeout_sec: float) -> Outcome:
    """Either invoke --runner CLIP_PATH and parse JSON SentlogResult
    from stdout, or — when runner_cmd is None — produce a deterministic
    synthetic result from the clip's labels for selftest purposes."""
    if runner_cmd is None:
        return _synthetic_outcome(clip)

    t0 = time.perf_counter()
    try:
        result = subprocess.run(
            runner_cmd + [str(clip.path)],
            capture_output=True, text=True, timeout=timeout_sec,
            check=True)
    except subprocess.TimeoutExpired:
        return Outcome(clip, 0.0, 0.0, 0.0, 0.0, error="timeout")
    except subprocess.CalledProcessError as e:
        return Outcome(clip, 0.0, 0.0, 0.0, 0.0,
                       error=f"runner exit {e.returncode}: {e.stderr[:200]}")
    duration_ms = (time.perf_counter() - t0) * 1000.0
    try:
        r = json.loads(result.stdout)
        return Outcome(
            clip            = clip,
            liveness_score  = float(r.get("liveness", {}).get("score", 0.0)),
            presence_sum    = float(r.get("presence", {}).get("summary", 0.0)),
            engagement_sc   = float(r.get("engagement", {}).get("score", 0.0)),
            duration_ms     = duration_ms,
        )
    except (json.JSONDecodeError, ValueError, TypeError) as e:
        return Outcome(clip, 0.0, 0.0, 0.0, duration_ms,
                       error=f"parse error: {e}")


def _synthetic_outcome(clip: Clip) -> Outcome:
    """Deterministic stub used by selftest. Genuine clips score high
    on liveness; spoofed clips score low. Latency stub = 50ms."""
    base = 0.92 if clip.ground_truth_live else 0.07
    # Family-specific micro-offset so histogram math has variety.
    offset = {
        "genuine":               0.05,
        "replay":               -0.02,
        "tts":                   0.00,
        "voice_clone":           0.01,
        "voice_clone_telephony": 0.00,
        "other":                 0.00,
    }.get(clip.family, 0.0)
    s = max(0.0, min(1.0, base + offset))
    return Outcome(clip, s, s * 0.9, s * 0.85, 50.0)


# --- aggregation --------------------------------------------------

def bucket(v: float) -> int:
    v = max(0.0, min(1.0, v))
    # Index 0..9 — 1.0 collapses into the top bucket.
    return min(9, int(v * 10))


def aggregate(outcomes: Iterable[Outcome]) -> dict[str, FamilyStats]:
    by_family: dict[str, FamilyStats] = {}
    for o in outcomes:
        stats = by_family.setdefault(
            o.clip.family, FamilyStats(family=o.clip.family))
        stats.n += 1
        if o.error is not None:
            stats.n_errors += 1
            continue
        stats.liveness_hist[bucket(o.liveness_score)] += 1
        stats.presence_hist[bucket(o.presence_sum)]   += 1
        stats.durations_ms.append(o.duration_ms)
    return by_family


def far_frr(outcomes: list[Outcome], threshold: float) -> tuple[float, float]:
    """False-accept rate (FAR) = spoof clips that scored >= threshold.
    False-reject rate (FRR) = genuine clips that scored < threshold."""
    spoof_accepted = 0
    spoof_total    = 0
    genuine_rejected = 0
    genuine_total    = 0
    for o in outcomes:
        if o.error is not None:
            continue
        if o.clip.ground_truth_live:
            genuine_total += 1
            if o.liveness_score < threshold:
                genuine_rejected += 1
        else:
            spoof_total += 1
            if o.liveness_score >= threshold:
                spoof_accepted += 1
    far = (spoof_accepted   / spoof_total)   if spoof_total   else 0.0
    frr = (genuine_rejected / genuine_total) if genuine_total else 0.0
    return far, frr


# --- report -------------------------------------------------------

def render_report(per_family: dict[str, FamilyStats],
                  far: float, frr: float, threshold: float) -> str:
    out: list[str] = []
    out.append(f"# Sentlog benchmark report\n")
    out.append(f"FAR @ liveness ≥ {threshold:.2f}: **{far*100:.2f}%**  ")
    out.append(f"FRR @ liveness < {threshold:.2f}: **{frr*100:.2f}%**\n")
    out.append("## Per-family\n")
    for family, s in sorted(per_family.items()):
        out.append(f"### {family}  (n={s.n}, errors={s.n_errors})\n")
        if s.durations_ms:
            p50 = statistics.median(s.durations_ms)
            p95 = statistics.quantiles(s.durations_ms, n=20)[-1] \
                  if len(s.durations_ms) >= 20 else max(s.durations_ms)
            out.append(f"latency: p50 {p50:.1f} ms, p95 {p95:.1f} ms\n")
        out.append("| bucket | liveness | presence |")
        out.append("|--------|----------|----------|")
        for i in range(10):
            lo = i / 10.0
            out.append(f"| {lo:.1f} | {s.liveness_hist[i]} | {s.presence_hist[i]} |")
        out.append("")
    return "\n".join(out)


def report_as_json(per_family: dict[str, FamilyStats],
                   far: float, frr: float, threshold: float) -> dict:
    return {
        "threshold": threshold,
        "far": far,
        "frr": frr,
        "per_family": {
            f: {k: v for k, v in asdict(s).items() if k != "durations_ms"} |
               {"latency_p50_ms": statistics.median(s.durations_ms) if s.durations_ms else None,
                "latency_p95_ms": (statistics.quantiles(s.durations_ms, n=20)[-1]
                                    if len(s.durations_ms) >= 20
                                    else (max(s.durations_ms) if s.durations_ms else None))}
            for f, s in per_family.items()
        },
    }


# --- entry point --------------------------------------------------

def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("corpus_root", type=Path,
                   help="directory containing manifest.csv + clips/")
    p.add_argument("--runner", help="executable invoked per clip; "
                   "receives WAV path on argv, returns SentlogResult JSON "
                   "on stdout. Without this, a synthetic stub runs (selftest mode).")
    p.add_argument("--runner-arg", action="append", default=[],
                   help="extra arg prefixed to each --runner call. Repeatable.")
    p.add_argument("--threshold", type=float, default=0.7,
                   help="liveness threshold used for FAR/FRR calculation")
    p.add_argument("--timeout-sec", type=float, default=30.0)
    p.add_argument("--max-errors", type=int, default=100,
                   help="exit code 2 if more than this many clips error")
    p.add_argument("--json-out", type=Path,
                   help="write structured aggregates to this path")
    args = p.parse_args(argv)

    if not args.corpus_root.exists():
        print(f"corpus not found: {args.corpus_root}", file=sys.stderr)
        return 1

    try:
        clips = load_manifest(args.corpus_root)
    except FileNotFoundError as e:
        print(str(e), file=sys.stderr)
        return 1

    runner_cmd = ([args.runner] + args.runner_arg) if args.runner else None

    outcomes: list[Outcome] = []
    n_errors = 0
    for clip in clips:
        o = run_one(clip, runner_cmd, args.timeout_sec)
        outcomes.append(o)
        if o.error is not None:
            n_errors += 1

    per_family = aggregate(outcomes)
    far, frr = far_frr(outcomes, args.threshold)

    print(render_report(per_family, far, frr, args.threshold))

    if args.json_out:
        args.json_out.write_text(
            json.dumps(report_as_json(per_family, far, frr, args.threshold),
                       indent=2))

    if n_errors > args.max_errors:
        print(f"\n{n_errors} runner errors exceeded --max-errors={args.max_errors}",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
