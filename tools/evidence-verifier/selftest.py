#!/usr/bin/env python3
"""Self-test for the Sentlog evidence verifier.

Produces a fixture envelope shaped like what the SDK emits in Phase
4b/4c, signs it with the same RFC 8032 §7.1 test-vector-1 key
gtest uses, then runs verify.py against it and asserts the verdict
flips correctly when the envelope is tampered with.

Runs in CI as part of the `tools (evidence-verifier)` job — so
drift between Python jcs_canonicalize() and C++
signing.cpp:jcs_canonicalize() surfaces there.

No external deps beyond `cryptography` (same as verify.py).
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent

# RFC 8032 §7.1 test vector 1 — same bytes the C++ signing_test
# (shared/tests/signing_test.cpp) pins.
TEST_SEED = bytes.fromhex(
    "9d61b19deffd5a60ba844af492ec2cc4"
    "4449c5697b326919703bac031cae7f60"
)
TEST_PUBKEY = bytes.fromhex(
    "d75a980182b10ab7d54bfed3c964073a"
    "0ee172f3daa62325af021a68f707511a"
)


def jcs_canonicalize(obj: object) -> bytes:
    """Same shape as verify.py's jcs_canonicalize."""
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("utf-8")


def base64url_no_pad_encode(data: bytes) -> str:
    import base64
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def make_unsigned_envelope() -> dict:
    """Shape matches shared/src/internal/envelope.cpp output."""
    return {
        "@context": [
            "https://www.w3.org/ns/credentials/v2",
            "https://sentlog.org/contexts/evidence/v1",
        ],
        "type": ["VerifiableCredential", "SentlogEvidence"],
        "issuer": "did:example:integrator",
        "validFrom": "2026-05-26T10:00:00.000Z",
        "credentialSubject": {
            "envelope_format": "sentlog-evidence/1",
            "projection": "Full",
            "payload": {
                "schema_version": "1",
                "transcript": {"text": "hello", "language": "en", "confidence": 0.9},
                "liveness": {"score": 0.88, "replay_risk": 0.05, "synthetic_risk": 0.07},
                "fingerprint": {"algo": "sentlog-pcp-v1", "bytes": ""},
            },
        },
    }


def sign_envelope_eddsa_jcs(envelope: dict) -> dict:
    """Mirror shared/src/internal/signing.cpp:sign_envelope_eddsa_jcs."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    proof_options = {
        "type":               "DataIntegrityProof",
        "cryptosuite":        "eddsa-jcs-2022",
        "created":            "2026-05-26T10:00:00.000Z",
        "verificationMethod": "did:key:zSelftest",
        "proofPurpose":       "assertionMethod",
    }
    env_for_signing = dict(envelope)
    env_for_signing["proof"] = proof_options
    canonical = jcs_canonicalize(env_for_signing)

    key = Ed25519PrivateKey.from_private_bytes(TEST_SEED)
    sig = key.sign(canonical)
    proof_options["proofValue"] = base64url_no_pad_encode(sig)
    env_signed = dict(envelope)
    env_signed["proof"] = proof_options
    return env_signed


def run_verifier(envelope_path: Path, pubkey_hex: str) -> tuple[int, str]:
    result = subprocess.run(
        [sys.executable, str(HERE / "verify.py"), str(envelope_path), pubkey_hex],
        capture_output=True, text=True, check=False,
    )
    return result.returncode, result.stdout + result.stderr


def main() -> int:
    pubkey_hex = TEST_PUBKEY.hex()

    # --- 1. Sign a clean envelope and verify it succeeds. -------------
    envelope = sign_envelope_eddsa_jcs(make_unsigned_envelope())
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(envelope, f)
        envelope_path = Path(f.name)

    code, output = run_verifier(envelope_path, pubkey_hex)
    print(output)
    if code != 0:
        print("FAIL: clean envelope did not verify (exit code "
              f"{code}, expected 0)", file=sys.stderr)
        return 1
    if "PASS" not in output:
        print("FAIL: expected 'PASS' in verifier output", file=sys.stderr)
        return 1

    # --- 2. Tamper the envelope and verify it FAILS. ------------------
    tampered = json.loads(envelope_path.read_text())
    tampered["credentialSubject"]["payload"]["transcript"]["text"] = "TAMPERED"
    tampered_path = envelope_path.with_suffix(".tampered.json")
    tampered_path.write_text(json.dumps(tampered))

    code, output = run_verifier(tampered_path, pubkey_hex)
    print(output)
    if code != 1:
        print(f"FAIL: tampered envelope should exit 1, got {code}",
              file=sys.stderr)
        return 1
    if "FAIL" not in output:
        print("FAIL: expected 'FAIL' in tampered verifier output",
              file=sys.stderr)
        return 1

    # --- 3. Wrong pubkey → fail. --------------------------------------
    wrong_pubkey = ("00" * 32)
    code, output = run_verifier(envelope_path, wrong_pubkey)
    if code != 1:
        print(f"FAIL: wrong-pubkey verify should exit 1, got {code}",
              file=sys.stderr)
        return 1

    envelope_path.unlink(missing_ok=True)
    tampered_path.unlink(missing_ok=True)
    print("selftest OK: clean envelope passes; tampering and wrong-pubkey fail.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
