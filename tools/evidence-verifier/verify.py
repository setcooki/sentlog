#!/usr/bin/env python3
"""Sentlog evidence-envelope verifier (Phase 4i).

Single-file Python verifier the SDK ships as the reference tool
under tools/evidence-verifier/. Reads a Sentlog VC 2.0 envelope
JSON, re-canonicalises per RFC 8785 JCS, and checks the
`proof.proofValue` Ed25519 signature against an integrator-supplied
public key. Outputs a verdict report on stdout.

Usage:
    python3 verify.py ENVELOPE_JSON_PATH PUBKEY_HEX
    cat envelope.json | python3 verify.py - PUBKEY_HEX

PUBKEY_HEX is the 32-byte Ed25519 public key as 64 hex chars
(matching the bytes the SDK was handed for signing). did:key:
resolution is a TODO — for now the verifier consumer pulls the
pubkey out of band.

Exit codes:
    0 — signature verifies
    1 — signature DOES NOT verify (tampered or wrong key)
    2 — envelope is malformed / missing proof / unsupported cryptosuite
    3 — usage error (bad args, missing files)

Dependencies: stdlib + `cryptography`. Install with:
    pip install cryptography
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
from typing import Any


def base64url_no_pad_decode(s: str) -> bytes:
    """W3C VC Data Integrity proofValue: base64url, no padding."""
    pad = (-len(s)) % 4
    return base64.urlsafe_b64decode(s + ("=" * pad))


def jcs_canonicalize(obj: Any) -> bytes:
    """RFC 8785 JSON Canonicalisation Scheme.

    Implementation matches `shared/src/internal/signing.cpp`:
        - object keys sorted lexicographically
        - no insignificant whitespace
        - numbers via Python's default JSON number serialisation,
          which is compatible with the SDK's nlohmann::json output
          for the value types we ship (floats, ints, strings, bools).
    """
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("utf-8")


def load_envelope(path: str) -> dict[str, Any]:
    if path == "-":
        return json.load(sys.stdin)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def verify_envelope(envelope: dict[str, Any], pubkey_bytes: bytes) -> tuple[bool, str]:
    """Returns (ok, message)."""
    proof = envelope.get("proof")
    if not isinstance(proof, dict):
        return False, "envelope has no `proof` block"

    cryptosuite = proof.get("cryptosuite")
    if cryptosuite != "eddsa-jcs-2022":
        return False, (
            f"unsupported cryptosuite {cryptosuite!r}; "
            "this verifier only handles eddsa-jcs-2022"
        )

    proof_value = proof.get("proofValue")
    if not isinstance(proof_value, str) or not proof_value:
        return False, "proof.proofValue is missing or empty"

    # The signing input per W3C Data Integrity §4.4 with eddsa-jcs-2022:
    # canonicalise(envelope_with_proof_options_but_without_proofValue).
    proof_options = {k: v for k, v in proof.items() if k != "proofValue"}
    envelope_for_verify = dict(envelope)
    envelope_for_verify["proof"] = proof_options
    signing_input = jcs_canonicalize(envelope_for_verify)

    try:
        signature = base64url_no_pad_decode(proof_value)
    except Exception as e:  # noqa: BLE001
        return False, f"proofValue is not valid base64url: {e}"

    # Verify with cryptography. Imported lazily so the script can
    # still emit a useful error if the package isn't installed.
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PublicKey,
        )
    except ImportError:
        return False, (
            "`cryptography` is required: pip install cryptography"
        )

    try:
        key = Ed25519PublicKey.from_public_bytes(pubkey_bytes)
        key.verify(signature, signing_input)
    except Exception:  # noqa: BLE001
        return False, "signature DOES NOT verify against the provided public key"

    return True, "signature OK"


def render_report(envelope: dict[str, Any], ok: bool, msg: str) -> str:
    subject = envelope.get("credentialSubject", {})
    lines: list[str] = []
    lines.append(f"verdict:        {'PASS' if ok else 'FAIL'}")
    lines.append(f"detail:         {msg}")
    lines.append(f"issuer:         {envelope.get('issuer', '?')}")
    lines.append(f"validFrom:      {envelope.get('validFrom', '?')}")
    lines.append(f"projection:     {subject.get('projection', '?')}")
    lines.append(f"envelope_format:{subject.get('envelope_format', '?')}")
    lines.append(f"evidence_grade: {subject.get('evidence_grade', '?')}")
    proof = envelope.get("proof", {})
    if proof:
        lines.append(f"cryptosuite:    {proof.get('cryptosuite', '?')}")
        lines.append(f"verifMethod:    {proof.get('verificationMethod', '?')}")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Verify a Sentlog evidence envelope.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("envelope", help="path to envelope JSON, or '-' for stdin")
    ap.add_argument("pubkey_hex", help="32-byte Ed25519 public key as hex")
    args = ap.parse_args(argv)

    try:
        pubkey_bytes = bytes.fromhex(args.pubkey_hex)
    except ValueError as e:
        print(f"bad PUBKEY_HEX: {e}", file=sys.stderr)
        return 3
    if len(pubkey_bytes) != 32:
        print(
            f"PUBKEY_HEX must decode to 32 bytes, got {len(pubkey_bytes)}",
            file=sys.stderr,
        )
        return 3

    try:
        envelope = load_envelope(args.envelope)
    except (OSError, json.JSONDecodeError) as e:
        print(f"could not load envelope: {e}", file=sys.stderr)
        return 3

    ok, msg = verify_envelope(envelope, pubkey_bytes)
    print(render_report(envelope, ok, msg))
    if ok:
        return 0
    # 2 for malformed/unsupported; 1 for bad signature.
    if "DOES NOT verify" in msg:
        return 1
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
