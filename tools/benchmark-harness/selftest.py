#!/usr/bin/env python3
"""Self-test for the benchmark harness.

Builds a tiny synthetic corpus on disk (CSV manifest + zero-byte
clip stubs), runs harness.py against it in synthetic-runner mode
(no --runner flag), and asserts on the aggregated FAR/FRR + the
per-family histogram counts.

The harness is exercised end-to-end including manifest parsing,
synthetic-stub outcome generation, family aggregation, and
threshold-based FAR/FRR. Runs in <1s on stdlib alone.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).parent


def write_corpus(root: Path) -> None:
    (root / "clips").mkdir(parents=True, exist_ok=True)
    rows = [
        # 6 genuine speakers
        ("g01", "clips/g01.wav", "genuine", "1", "set-a"),
        ("g02", "clips/g02.wav", "genuine", "1", "set-a"),
        ("g03", "clips/g03.wav", "genuine", "1", "set-a"),
        ("g04", "clips/g04.wav", "genuine", "1", "set-b"),
        ("g05", "clips/g05.wav", "genuine", "1", "set-b"),
        ("g06", "clips/g06.wav", "genuine", "1", "set-b"),
        # 4 replay spoofs
        ("r01", "clips/r01.wav", "replay",  "0", "set-a"),
        ("r02", "clips/r02.wav", "replay",  "0", "set-a"),
        ("r03", "clips/r03.wav", "replay",  "0", "set-b"),
        ("r04", "clips/r04.wav", "replay",  "0", "set-b"),
        # 3 TTS spoofs
        ("t01", "clips/t01.wav", "tts",     "0", "set-c"),
        ("t02", "clips/t02.wav", "tts",     "0", "set-c"),
        ("t03", "clips/t03.wav", "tts",     "0", "set-c"),
    ]
    manifest = root / "manifest.csv"
    with open(manifest, "w", encoding="utf-8") as f:
        f.write("clip_id,path,family,ground_truth_live,source\n")
        for row in rows:
            f.write(",".join(row) + "\n")
    # Zero-byte clip stubs — the synthetic runner never reads them.
    for _id, path, *_ in rows:
        (root / path).touch()


def main() -> int:
    failures: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)
            print(f"FAIL: {msg}", file=sys.stderr)
        else:
            print(f"ok:   {msg}")

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        write_corpus(root)
        json_out = root / "report.json"

        result = subprocess.run(
            [sys.executable, str(HERE / "harness.py"),
             str(root), "--json-out", str(json_out)],
            capture_output=True, text=True, timeout=30)
        check(result.returncode == 0,
              f"harness exits 0 (got {result.returncode}, stderr={result.stderr[:200]!r})")

        check(json_out.exists(), "harness wrote --json-out")
        if not json_out.exists():
            return 1
        report = json.loads(json_out.read_text())

        # Threshold default = 0.7. Genuine clips get score 0.92+offset
        # which is well above; replay/tts get 0.05–0.07 well below. So
        # FAR (spoofs accepted) should be 0; FRR (genuine rejected)
        # should be 0.
        check(report["far"] == 0.0,
              f"FAR is 0.0 for clean synthetic corpus (got {report['far']})")
        check(report["frr"] == 0.0,
              f"FRR is 0.0 for clean synthetic corpus (got {report['frr']})")

        # Per-family n counts match the manifest.
        per = report["per_family"]
        check(per["genuine"]["n"] == 6, f"genuine n=6 (got {per['genuine']['n']})")
        check(per["replay"]["n"]  == 4, f"replay n=4 (got {per['replay']['n']})")
        check(per["tts"]["n"]     == 3, f"tts n=3 (got {per['tts']['n']})")

        # All synthetic outcomes are clean (no errors).
        for family in ("genuine", "replay", "tts"):
            check(per[family]["n_errors"] == 0,
                  f"{family} has 0 runner errors")

        # Histograms: genuine cluster in top bucket (index 9 → 0.9..1.0),
        # spoofs in bottom (index 0 → 0.0..0.1).
        check(per["genuine"]["liveness_hist"][9] == 6,
              "all 6 genuine clips bucket into the [0.9, 1.0] liveness band")
        check(per["replay"]["liveness_hist"][0] == 4,
              "all 4 replay clips bucket into the [0.0, 0.1] liveness band")

    if failures:
        print(f"\n{len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
