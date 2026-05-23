#!/usr/bin/env python3
"""Phase 1 exit-gate threshold checker.

Reads the JSON output produced by `gradle :benchmarks:connectedCheck`
(androidx.benchmark writes `benchmark-results.json` into the
connected device's app data) and asserts each measurement against
the thresholds in `docs/rollout-plan.md §2.3`.

Usage:
    python3 tools/benchmark/check-thresholds.py path/to/benchmark-results.json
    python3 tools/benchmark/check-thresholds.py --device dimensity-700 results.json

Exit code:
    0 — all measurements within "pass" thresholds.
    1 — at least one "review trigger" exceeded; Phase 1 architecture
        review required before proceeding.
    2 — JSON couldn't be parsed or required tests are missing.

Stdout: a markdown table suitable for pasting into the per-device
section of `docs/benchmarks/wk2.md`.
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass


# Thresholds from docs/rollout-plan.md §2.3. Updating this table
# requires updating the rollout plan and vice versa.
@dataclass
class Threshold:
    metric: str
    pass_max: float       # value at or below this = PASS
    review_max: float     # value above this triggers architecture review
    unit: str             # display-only

THRESHOLDS_NO_VIDEO = {
    "warm_p50_ms":   Threshold("Warm pipeline p50 (no video)", 6_000,  8_000, "ms"),
    "warm_p95_ms":   Threshold("Warm pipeline p95 (no video)", 9_000, 12_000, "ms"),
    "cold_load_ms":  Threshold("Cold model load",              4_000,  8_000, "ms"),
    "peak_mem_mb":   Threshold("Peak memory (no video)",         350,    450, "MB"),
}

# JSON-key → metric mapping. androidx.benchmark writes results under
# "benchmarks": [{"name": "...", "metrics": {"timeNs": {...}}, ...}].
# Specific to the SentlogPipelineBenchmark test class.
EXPECTED_TESTS = {
    "warmEvaluate":                    "warm_*_ms",
    "coldInitialize":                  "cold_load_ms",
    "peakNativeHeapAfterEvaluate":     "peak_mem_mb",
}


def percentile_ms(metric_block: dict, key: str) -> float | None:
    """Read a percentile from androidx.benchmark's timeNs block."""
    time_ns = metric_block.get("timeNs", {})
    return time_ns.get(key, None) / 1_000_000.0 if key in time_ns else None


def find_test(benchmarks: list, simple_name: str) -> dict | None:
    for b in benchmarks:
        # androidx.benchmark records "name" as the full method label,
        # e.g. "warmEvaluate[stage=...]". Match by prefix.
        name = b.get("name", "")
        if name == simple_name or name.startswith(simple_name + "["):
            return b
    return None


def extract_peak_mem_mb(stdout: str) -> float | None:
    """The peakNativeHeapAfterEvaluate test logs a tagged line we
    parse out of androidx.benchmark's captured stdout."""
    m = re.search(r"SENTLOG_BENCH_PEAK_NATIVE_HEAP_BYTES=(\d+)", stdout)
    if m is None:
        return None
    return int(m.group(1)) / (1024.0 * 1024.0)


def verdict(value: float, t: Threshold) -> str:
    if value <= t.pass_max:    return "PASS"
    if value <= t.review_max:  return "WARN"
    return "REVIEW"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results_json", help="Path to androidx.benchmark JSON output")
    ap.add_argument("--device", default="unknown",
                    help="Device label for the report (e.g. 'dimensity-700')")
    ap.add_argument("--with-video", action="store_true",
                    help="(future) Apply with-video thresholds")
    args = ap.parse_args()

    if args.with_video:
        print("with-video thresholds not yet wired (video pipeline lands in Phase 5)",
              file=sys.stderr)
        return 2

    try:
        with open(args.results_json) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"Could not read {args.results_json}: {e}", file=sys.stderr)
        return 2

    benchmarks = data.get("benchmarks", [])
    measurements: dict[str, float | None] = {}

    warm = find_test(benchmarks, "warmEvaluate")
    if warm is None:
        print("warmEvaluate result missing from JSON", file=sys.stderr)
        return 2
    metrics = warm.get("metrics", {})
    measurements["warm_p50_ms"] = percentile_ms(metrics, "median")
    measurements["warm_p95_ms"] = percentile_ms(metrics, "p95")

    cold = find_test(benchmarks, "coldInitialize")
    if cold is None:
        print("coldInitialize result missing from JSON", file=sys.stderr)
        return 2
    measurements["cold_load_ms"] = percentile_ms(cold.get("metrics", {}), "median")

    # Peak memory comes out of the captured stdout, not the metrics
    # block — androidx.benchmark doesn't model peak memory.
    stdout = data.get("instrumentation_stdout", "")
    measurements["peak_mem_mb"] = extract_peak_mem_mb(stdout)

    # Verdict.
    any_review = False
    rows = []
    for key, t in THRESHOLDS_NO_VIDEO.items():
        v = measurements.get(key)
        if v is None:
            rows.append(f"| {t.metric} | n/a | {t.pass_max} {t.unit} | {t.review_max} {t.unit} | MISSING |")
            any_review = True
        else:
            verd = verdict(v, t)
            if verd == "REVIEW":
                any_review = True
            rows.append(
                f"| {t.metric} | {v:.0f} {t.unit} | "
                f"{t.pass_max} {t.unit} | {t.review_max} {t.unit} | {verd} |"
            )

    print(f"## {args.device}\n")
    print(f"| Metric | Measured | Pass ≤ | Review > | Verdict |")
    print(f"|---|---|---|---|---|")
    for r in rows:
        print(r)
    print()

    return 1 if any_review else 0


if __name__ == "__main__":
    sys.exit(main())
