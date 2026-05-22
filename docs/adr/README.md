# Architecture Decision Records

Each ADR captures one decision: the context that forced it, what we
chose, and what that commits us to. They follow a MADR-lite template:

- **Status** — Accepted, Superseded, Deprecated.
- **Context** — what problem and what alternatives.
- **Decision** — what we chose.
- **Consequences** — what this enables and constrains downstream.

ADRs are append-only. To change a decision, write a new ADR that
supersedes the old one and update the old one's status.

The first batch captures the load-bearing decisions referenced in the
[technical specification §1](../technical-specification.md#1-architectural-decisions).
Subsequent batches land as new decisions are made.

| # | Title | Spec ref |
|---|---|---|
| [0001](0001-cpp-shared-core-with-c-abi.md) | C++17 shared core with C ABI | 1.1 |
| [0002](0002-whispercpp-transcription-stack.md) | whisper.cpp tiny Q5_1 transcription stack | 1.2 |
| [0003](0003-signed-manifest-ota.md) | Baseline-bundled, Ed25519-signed-manifest OTA | 1.3 |
| [0004](0004-aasist-l-liveness-from-week-2.md) | AASIST-L liveness from Phase 1, not rule-based | 1.7 |
| [0005](0005-composable-forwarding.md) | Composable forwarding for all output destinations | 1.9 |
| [0006](0006-evidence-not-verdicts.md) | The SDK output is evidence, not verdicts | 1.22 |
| [0007](0007-vc20-envelope-format.md) | W3C Verifiable Credentials 2.0 envelope format | 1.25 |
| [0008](0008-rn-capacitor-bridges-in-v1.md) | React Native and Capacitor JS bridges in v1.0 | 1.16 |
