# Hardware-backed signing

The SDK can sign envelopes with a key that lives in the device's
secure element — iOS Secure Enclave or Android KeyStore (StrongBox
where available). On success, the envelope's `evidence_grade` is
promoted to `EnclaveSigned` and the proof block uses
`ecdsa-jcs-2019` (Secure Enclave / KeyStore ECDSA-P256) or
`eddsa-jcs-2022` (KeyStore Ed25519 on API 33+).

Hardware signing is **opt-in**. The default signing path is the
software Ed25519 key registered via `setSigningKey`, which is fine
for most integrations. Reach for hardware signing when:

- the envelope's verifier requires enclave-backed proofs;
- your compliance posture mandates non-exportable private keys;
- you want a strong device-binding signal at envelope-issue time.

## Architecture

The shared C++ core does proof-block assembly + canonicalisation +
fallback handling. **It does not own the key material** — your app
generates the key in the secure element and registers a *signer
callback* that receives the JCS-canonical envelope bytes and returns
the raw signature.

This split is deliberate: every device's keystore API surface is
different, and the SDK shouldn't pretend to abstract them. Your
binding code keeps full control of key lifecycle, biometric
prompts, and any platform-specific quirks (`kSecAttrAccessible…`,
`setUserAuthenticationRequired`, etc.).

A SDK-bundled `SecureEnclaveSigner` / `KeyStoreSigner` helper that
takes care of the boilerplate is planned for Phase 7.5 and will land
without changing the API below.

## iOS (Swift)

The SDK ships a `SecureEnclaveSigner` helper that does key creation,
keychain persistence, and `did:key:` URI derivation for you:

```swift
import Sentlog

let signer = try SecureEnclaveSigner.create(
    keyTag: "me.example.myapp.sentlog")
try Sentlog.setHardwareSigner(signer)
```

That's it. The first call generates the ECDSA-P256 key inside the
Secure Enclave under the given keychain tag; subsequent calls load
the existing key. The signer's `verificationMethod` is a `did:key:`
URI derived from the public key — the verifier resolves it without an
external key directory. The public key bytes are also available via
`signer.publicKeyRaw` (65 bytes, uncompressed P-256) if you need to
hand them to a separate enrollment flow.

### Manual wiring (without the helper)

If you need finer control (biometric gating, multiple keys, custom
access control), construct the signer closure yourself:

```swift
import Security
import Sentlog

let access = SecAccessControlCreateWithFlags(
    nil, kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
    [.privateKeyUsage, .biometryCurrentSet],   // biometric-gated
    nil)!
let attrs: [String: Any] = [
    kSecAttrKeyType as String:           kSecAttrKeyTypeECSECPrimeRandom,
    kSecAttrKeySizeInBits as String:     256,
    kSecAttrTokenID as String:           kSecAttrTokenIDSecureEnclave,
    kSecPrivateKeyAttrs as String: [
        kSecAttrIsPermanent as String:   true,
        kSecAttrApplicationTag as String: "me.example.myapp.sentlog".data(using: .utf8)!,
        kSecAttrAccessControl as String: access,
    ]
]
let privateKey = SecKeyCreateRandomKey(attrs as CFDictionary, nil)!
try Sentlog.setHardwareSigner(
    { canonicalBytes in
        SecKeyCreateSignature(
            privateKey,
            .ecdsaSignatureMessageX962SHA256,
            canonicalBytes as CFData, nil) as Data?
    },
    cryptosuite: "ecdsa-jcs-2019",
    verificationMethod: "did:key:zYourPubKeyMultibase")
```

The `verificationMethod` becomes `proof.verificationMethod` in the
envelope. For did:key derivation see the multicodec prefix `0x1200`
(P-256 compressed) + base58btc — or use the helper above which does
it for you.

## Android (Kotlin)

The SDK ships a `KeyStoreSigner` helper that does AndroidKeyStore
key creation, StrongBox fallback, and `did:key:` URI derivation for
you:

```kotlin
import me.cooki.sentlog.KeyStoreSigner
import me.cooki.sentlog.Sentlog

val signer = KeyStoreSigner.create(keyAlias = "me.example.myapp.sentlog")
Sentlog.setHardwareSigner(signer)
```

That's it. The first call generates an ECDSA-P256 key in the
AndroidKeyStore under the given alias — StrongBox-backed when the
device supports it, regular hardware-backed otherwise (the helper
transparently retries on `StrongBoxUnavailableException`). Subsequent
calls load the existing key. The signer's `verificationMethod` is a
`did:key:` URI derived from the public key; `signer.publicKeyRaw`
exposes the raw 65-byte uncompressed P-256 bytes; `signer.strongBoxBacked`
tells you whether the key actually landed in StrongBox.

### Manual wiring (without the helper)

If you need finer control (biometric gating via
`setUserAuthenticationRequired`, custom alias-rotation policy,
Ed25519 on API 33+), construct the signer closure yourself:

```kotlin
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyStore
import java.security.Signature

val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
if (!keyStore.containsAlias("sentlog.signing")) {
    val kpg = java.security.KeyPairGenerator.getInstance(
        KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore")
    val spec = KeyGenParameterSpec.Builder(
            "sentlog.signing",
            KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY)
        .setAlgorithmParameterSpec(java.security.spec.ECGenParameterSpec("secp256r1"))
        .setDigests(KeyProperties.DIGEST_SHA256)
        .setIsStrongBoxBacked(true)
        .setUserAuthenticationRequired(true)   // biometric-gated
        .build()
    kpg.initialize(spec)
    kpg.generateKeyPair()
}

Sentlog.setHardwareSigner(
    signer = { canonical ->
        Signature.getInstance("SHA256withECDSA").apply {
            initSign(keyStore.getKey("sentlog.signing", null) as java.security.PrivateKey)
            update(canonical)
        }.sign()
    },
    cryptosuite = "ecdsa-jcs-2019",
    verificationMethod = "did:key:zYourPubKeyMultibase",
)
```

For Ed25519 on API 33+, swap the algorithm parameter spec for an
`EdDSAParameterSpec` and set `cryptosuite = "eddsa-jcs-2022"`. The
SDK accepts both cryptosuites.

## Fallback behaviour

If your signer returns `nil` / `null` (Secure Enclave unavailable on
simulator, biometric prompt denied, etc.):

- `SentlogConfig.hardwareSigningRequired = false` (default) — the SDK
  falls back to the software Ed25519 key registered via
  `setSigningKey`, or emits the envelope unsigned if no software key
  is set. A `HardwareSigningUnavailable` telemetry event fires so you
  can detect the silent downgrade.
- `SentlogConfig.hardwareSigningRequired = true` — the envelope is
  **dropped** with a `ForwarderOutcome { outcome: "dropped" }`
  telemetry event per affected forwarder. The integrator's app must
  decide whether to retry, prompt the user, or surface an error.

## evidence_grade promotion

When hardware signing succeeds, the envelope's
`credentialSubject.evidence_grade` is rewritten to `EnclaveSigned`
**before** canonicalisation, so the signature covers the promoted
value. The SDK-returned `SentlogResult.evidence_grade` is unchanged
(it reflects the device-attestation path, not the signing path);
verifiers should rely on the envelope value.

## Testing on simulator / emulator

Secure Enclave is not available in the iOS simulator;
`SecKeyCreateRandomKey` returns an error and your signer should
propagate `nil`. Without `hardwareSigningRequired = true`, the SDK
will emit `HardwareSigningUnavailable` and fall back silently — your
integration tests can assert on that telemetry event to verify the
fallback path.

Android emulators support the KeyStore but not StrongBox; the
`setIsStrongBoxBacked(true)` call may throw on emulators that don't
emulate the hardware module. Wrap the key-creation call in a try/catch
and retry without the StrongBox flag.
