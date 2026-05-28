# Device attestation

Optional capability: signal that the SDK was running on a
non-tampered platform by attaching a Play Integrity (Android) or
App Attest (iOS) token to every dispatched envelope. When present,
the envelope's `evidence_grade` is promoted from `DeviceUnattested`
to `DeviceAttested`.

Inert by default: the SDK never calls Play Integrity / App Attest
on its own — the integrator drives the attestation flow on their
schedule and pushes the resulting token via
`Sentlog.setDeviceAttestation(...)`.

## Why integrator-driven

Both platforms' attestation APIs:

- have rate limits + quota (Play Integrity standard tier;
  Apple's DeviceCheck quota)
- charge against an integrator's Google Cloud / Apple Developer
  account, not Sentlog's
- need a project-specific API key / nonce strategy
- have lifecycles that often align with the integrator's existing
  app-startup / re-auth flows rather than per-evaluate timing

The SDK accepts the token; the integrator owns when and how to
fetch it.

## evidence_grade interaction

| Signing path | Attestation pushed | Resulting grade |
|---|---|---|
| Software Ed25519 (default) | no  | `DeviceUnattested` |
| Software Ed25519           | yes | `DeviceAttested`   |
| Secure Enclave / KeyStore  | no  | `EnclaveSigned`    |
| Secure Enclave / KeyStore  | yes | `EnclaveSigned` (never demoted) |

`EnclaveSigned` is the highest grade: hardware-backed signing
implies a non-tampered key surface, so the attestation token adds
no incremental signal. The SDK never demotes from `EnclaveSigned`
even if attestation is also present; the envelope still carries
the `device_attestation` block for verifiers that want to cross-
reference.

## Android

```kotlin
import com.google.android.play.core.integrity.IntegrityManagerFactory
import com.google.android.play.core.integrity.IntegrityTokenRequest
import me.cooki.sentlog.Sentlog

// Run at app start, after auth, or before a sensitive evaluate.
val manager = IntegrityManagerFactory.create(applicationContext)
val request = IntegrityTokenRequest.builder()
    .setNonce(myNonce)        // your nonce strategy
    .build()
manager.requestIntegrityToken(request)
    .addOnSuccessListener { resp ->
        Sentlog.setDeviceAttestation(
            tokenBytes      = resp.token().toByteArray(Charsets.UTF_8),
            attestationType = "play_integrity")
    }
    .addOnFailureListener {
        // attestation failed; SDK falls back to DeviceUnattested
        Sentlog.setDeviceAttestation(tokenBytes = null)
    }
```

Play Integrity tokens are JWS strings; we record them as bytes
(UTF-8). The verifier can re-parse them as JWS out of band.

A bundled `PlayIntegrityAttester` helper that wraps the boilerplate
above lands in Phase 5e-ii (introduces Google Play Services as an
optional dep).

## iOS

```swift
import DeviceCheck
import Sentlog

// AppAttest requires a hardware-backed key created once per
// install. Persist the keyId; reuse on subsequent attestations.
let service = DCAppAttestService.shared
guard service.isSupported else { return }

service.generateKey { keyId, error in
    guard let keyId else { return }
    let challenge = myChallengeBytes  // server-issued
    let hash = SHA256.hash(data: challenge).withUnsafeBytes { Data($0) }
    service.attestKey(keyId, clientDataHash: hash) { attestation, _ in
        guard let attestation else { return }
        try? Sentlog.setDeviceAttestation(
            attestation, attestationType: "app_attest")
    }
}
```

Attestation tokens are CBOR-encoded; we record them as bytes
unchanged. The verifier validates against Apple's AppAttest root
out of band.

A bundled `AppAttestAttester` helper that wraps the boilerplate
above lands in Phase 5e-ii.

## Token lifetime

The SDK never times tokens out — once pushed, the token stays
attached to every envelope until you push a fresh one or clear it
(`setDeviceAttestation(null)`). Both Play Integrity + App Attest
tokens have their own freshness windows; if yours expire, push a
fresh token before the next evaluate.

## What appears in the envelope

```jsonc
{
  "credentialSubject": {
    "envelope_format":   "sentlog-v1",
    "evidence_grade":    "DeviceAttested",
    "device_attestation": {
      "type":      "play_integrity",
      "token_b64": "eyJhbGciOiJSUzI1NiIs..."   // base64 of the token bytes
    },
    "payload": { ... }
  }
}
```

The block is absent when no token has been pushed.
