# Sentlog Benchmark Operator Runbook

This is the human-facing companion to the Phase 1 exit gate
(`docs/rollout-plan.md §2`). The benchmark gate is the deliverable
that says "Phase 1 is done." It requires running the
`SentlogPipelineBenchmark` on three real Android devices and
recording the results in [`wk2.md`](wk2.md).

## What you'll need

- A development machine with the Android SDK + NDK + ADB.
- USB-C cables for each device.
- The three target devices listed in
  [`docs/rollout-plan.md §2.1`](../rollout-plan.md):
  - Samsung A14 5G (Mediatek Dimensity 700)
  - Pixel 6a (Tensor G1)
  - Moto G Power 2024 (Snapdragon 6-class)
- Each device set up for developer-mode USB debugging.
- Python 3.10+ for the threshold checker.

You should also have committed `aasist_l.onnx` and `silero_vad.onnx`
to `android/sentlog/src/main/assets/` (run
`tools/aasist-conversion/convert-aasist-l.py` if you haven't). If
they're missing the benchmark still runs but only exercises the
energy-VAD / placeholder-liveness path — Phase 1 doesn't pass on
that.

## What the benchmark measures

Three tests in `SentlogPipelineBenchmark.kt`:

1. `warmEvaluate` — pushes a 15 s synthetic speech-like clip into a
   pre-initialised SDK and times the `evaluate()` call. Many
   iterations; androidx.benchmark reports median (p50) and p95.
2. `coldInitialize` — measures `Sentlog.initialize()` cost,
   including the bundled model reads. Each iteration destroys and
   re-creates the SDK so every tick is a cold start.
3. `peakNativeHeapAfterEvaluate` — runs a single evaluate and
   captures the native heap size via `Debug.getNativeHeapAllocatedSize`.

Workload is fully defined in
[`docs/rollout-plan.md §2.2`](../rollout-plan.md).

## Per-device procedure

For each of the three devices:

```bash
# 1. Connect device via USB. Confirm it's reachable.
adb devices            # should list a single attached device

# 2. (one-time) confirm RECORD_AUDIO is grantable, screen on,
#     storage allowed for benchmark.json output.
adb shell settings put global animator_duration_scale 0
adb shell settings put global window_animation_scale 0
adb shell settings put global transition_animation_scale 0
adb shell svc power stayon usb     # keep awake while plugged in

# 3. Run the benchmark.
cd android
gradle :benchmarks:connectedReleaseAndroidTest \
    -Pandroid.testInstrumentationRunnerArguments.androidx.benchmark.suppressErrors=DEBUGGABLE,EMULATOR

# Output JSON lands at:
#   android/benchmarks/build/outputs/connected_android_test_additional_output/release/
#       <device-id>/benchmarkData.json
# Pull it to your machine.
adb pull /sdcard/Android/media/me.cooki.sentlog.benchmarks/me.cooki.sentlog.benchmarks-benchmarkData.json \
    /tmp/benchmark-<device-label>.json

# 4. Threshold check.
python3 ../tools/benchmark/check-thresholds.py \
    --device <device-label> /tmp/benchmark-<device-label>.json
```

`<device-label>` should be one of `dimensity-700`, `tensor-g1`,
`snapdragon-6-class` — these are the section anchors in
[`wk2.md`](wk2.md).

The threshold checker exits 0 on PASS, 1 if any measurement crosses
into REVIEW territory, 2 on bad/missing JSON. The stdout is a
markdown table you paste into the per-device section of `wk2.md`.

## What "pass" means

All four measurements within the **PASS** thresholds in
`docs/rollout-plan.md §2.3` (warm p50 ≤ 6 s, warm p95 ≤ 9 s, cold
load ≤ 4 s, peak memory ≤ 350 MB), on all three devices.

Anything in **WARN** is acceptable but tracked — the device runs
the SDK but hovers close to the cliff. **REVIEW** triggers the
formal architecture review enumerated in
[`docs/rollout-plan.md §2.4`](../rollout-plan.md) (escape valves
ranked).

## What happens after the gate

- All-pass: record results in `wk2.md`, commit, Phase 2 starts.
- Any REVIEW: convene the architecture review per §2.4, decide
  on an escape valve, re-run.
- Missing devices: postpone Phase 1 close until devices acquired.

The gate isn't a checkbox — it's the empirical validation that the
sub-$200 device floor is real. Don't shortcut it.

## Re-running

The benchmark runs as often as needed. Re-run when:

- A new chunk lands that changes the pipeline (anything in
  `shared/src/internal/pipeline.cpp` or model files).
- The pinned ORT version bumps.
- Either model file is regenerated.
- An on-device regression report comes in from an integrator.

Each re-run produces a fresh `wk2.md`-style entry timestamped with
the commit SHA in the rollout plan.
