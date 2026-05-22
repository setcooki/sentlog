# Security Policy

Sentlog produces signed evidence about human presence and intent. The
SDK ships claims that downstream systems trust. We take security
reports seriously and want to make it easy for researchers to reach us.

## Reporting a vulnerability

**Do not open a public GitHub issue for security reports.**

Email **set@cooki.me** with the subject line `[sentlog-security]` and
include:

- A clear description of the issue and where it lives in the code.
- Steps to reproduce, or a proof-of-concept if you have one.
- Affected versions / commit hashes you tested against.
- Whether the issue is already public anywhere.
- Your name or handle, if you'd like credit in the eventual advisory.

We will acknowledge receipt within **3 business days** and follow up
with a remediation plan and an expected timeline within **10 business
days** of the initial report.

## Supported versions

Until the v1.0 release, only the `main` branch is supported. Fixes
land directly on `main`.

After v1.0, the most recent minor version of every released major
version gets security fixes for **12 months** after that major is
superseded. Older versions are best-effort.

| Version | Supported |
|---|---|
| `main` (pre-v1.0) | ✅ |
| `1.x` latest minor | TBD at v1.0 |
| Older | ❌ |

## Scope

In scope:

- The Sentlog C++ shared core (`shared/`).
- Platform bindings (`android/`, `ios/`).
- JS bridges (`rn/`, `capacitor/`).
- The OTA manifest verification path.
- The W3C Verifiable Credentials envelope construction and signing.
- The reference evidence verifier (`tools/evidence-verifier/`).

Out of scope (these belong to upstream projects — please report to
them directly):

- Vulnerabilities in whisper.cpp, Silero VAD, openSMILE, MediaPipe
  Face Mesh, Chromaprint, AASIST-L, or any other dependency.
- Vulnerabilities in integrator-supplied backends, signing keys, or
  TSA endpoints.
- App-level issues in apps that embed Sentlog (those are the
  integrator's responsibility).

## Disclosure approach

We prefer **coordinated disclosure.** We will:

1. Confirm the report and assess severity.
2. Develop and test a fix in a private branch.
3. Coordinate a release date with the reporter.
4. Publish a fix, a security advisory describing the issue, and
   credit to the reporter (with permission).

If a vulnerability is being actively exploited or has already been
publicly disclosed, we will accelerate the timeline.

## What is NOT a vulnerability

Some things look like vulnerabilities but are documented design
constraints. Please review
[docs/whitepaper.md](docs/whitepaper.md) and
[docs/technical-specification.md](docs/technical-specification.md)
before reporting:

- **Voice cloning is not perfectly detected.** This is explicit; per
  the whitepaper, liveness is a *risk signal*, not an authenticator.
  Cross-generator performance against modern voice cloning sits at
  60–80% clean and 55–70% over telephony.
- **The integrator owns destinations and DPAs.** If a forwarder
  transmits data to a destination the integrator configured, that
  is by design.
- **Demographic bias inherited from upstream models.** Documented in
  the technical specification.
- **The SDK does not request permissions on the integrator's behalf.**
  Permission denials surface in diagnostics; this is the documented
  behaviour.

## Hall of fame

(Researchers who have responsibly disclosed will be listed here.)
