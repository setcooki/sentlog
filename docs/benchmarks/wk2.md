# Phase 1 Exit-Gate Benchmark Results

**Status:** Pending — benchmark gate has not been executed.

This file records the Phase 1 exit-gate measurements per
[`docs/rollout-plan.md §2`](../rollout-plan.md). It will be populated
once `SentlogPipelineBenchmark` has been run on each of the three
target devices (per the operator runbook in [`README.md`](README.md))
and the threshold checker has produced verdicts.

The Phase 1 close is gated on this file showing PASS on all three
devices. WARN is acceptable but tracked; REVIEW triggers the
architecture review enumerated in
[`docs/rollout-plan.md §2.4`](../rollout-plan.md).

---

## Run metadata

| Field | Value |
|---|---|
| Sentlog commit | _(filled at run time, e.g. `9775f10`)_ |
| ONNX Runtime version | 1.17.1 (Android) / 1.17.0 (iOS SwiftPM) |
| AASIST-L source commit | _(filled at run time from `models/liveness/SOURCE.md`)_ |
| Silero VAD tag | v5.1 |
| Date | _(filled at run time)_ |
| Operator | _(filled at run time)_ |

---

## dimensity-700

**Device:** Samsung Galaxy A14 5G (Mediatek Dimensity 700)
**Android:** _(version)_
**Status:** _(PENDING / PASS / WARN / REVIEW)_

| Metric | Measured | Pass ≤ | Review > | Verdict |
|---|---|---|---|---|
| Warm pipeline p50 (no video) | _ms_ | 6 000 ms | 8 000 ms | _PENDING_ |
| Warm pipeline p95 (no video) | _ms_ | 9 000 ms | 12 000 ms | _PENDING_ |
| Cold model load | _ms_ | 4 000 ms | 8 000 ms | _PENDING_ |
| Peak memory (no video) | _MB_ | 350 MB | 450 MB | _PENDING_ |

Notes: _(any caveats from the run — first-cold-iteration outlier,
thermal throttle observed, ORT EP fallback, etc.)_

---

## tensor-g1

**Device:** Google Pixel 6a (Tensor G1)
**Android:** _(version)_
**Status:** _(PENDING / PASS / WARN / REVIEW)_

| Metric | Measured | Pass ≤ | Review > | Verdict |
|---|---|---|---|---|
| Warm pipeline p50 (no video) | _ms_ | 6 000 ms | 8 000 ms | _PENDING_ |
| Warm pipeline p95 (no video) | _ms_ | 9 000 ms | 12 000 ms | _PENDING_ |
| Cold model load | _ms_ | 4 000 ms | 8 000 ms | _PENDING_ |
| Peak memory (no video) | _MB_ | 350 MB | 450 MB | _PENDING_ |

Notes:

---

## snapdragon-6-class

**Device:** Moto G Power 2024 (Snapdragon 6-class)
**Android:** _(version)_
**Status:** _(PENDING / PASS / WARN / REVIEW)_

| Metric | Measured | Pass ≤ | Review > | Verdict |
|---|---|---|---|---|
| Warm pipeline p50 (no video) | _ms_ | 6 000 ms | 8 000 ms | _PENDING_ |
| Warm pipeline p95 (no video) | _ms_ | 9 000 ms | 12 000 ms | _PENDING_ |
| Cold model load | _ms_ | 4 000 ms | 8 000 ms | _PENDING_ |
| Peak memory (no video) | _MB_ | 350 MB | 450 MB | _PENDING_ |

Notes:

---

## Gate verdict

**Overall status:** _PENDING_

| Device | Verdict |
|---|---|
| dimensity-700 | PENDING |
| tensor-g1 | PENDING |
| snapdragon-6-class | PENDING |

**Decision:** _(Phase 2 starts / architecture review triggered /
re-run with mitigation)_

**Reviewer sign-off:** _(name + date)_
