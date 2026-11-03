#!/usr/bin/env python3
"""Sentlog evidence-envelope verifier.

Single-file Python verifier the SDK ships as the reference tool
under tools/evidence-verifier/. Reads a Sentlog VC 2.0 envelope JSON
and checks its `eddsa-jcs-2022` or `ecdsa-jcs-2019` Data Integrity
proof against an integrator-supplied public key. Outputs a verdict
report on stdout.

Usage:
    python3 verify.py ENVELOPE_JSON_PATH PUBKEY_HEX
    cat envelope.json | python3 verify.py - PUBKEY_HEX

PUBKEY_HEX is the public key in hex: 32 bytes for Ed25519, or a
33-byte compressed / 65-byte uncompressed SEC1 point for P-256.
did:key: resolution is a TODO — for now the verifier consumer pulls
the pubkey out of band.

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
import hashlib
import json
import sys
from typing import Any

BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
PROOF_FIELDS = {
    "type", "cryptosuite", "created", "verificationMethod",
    "proofPurpose", "@context", "proofValue",
}
MAX_SAFE_DOUBLE_INT = 2**53


def multibase_base58btc_decode(text: str) -> bytes:
    """Strict: a `z` prefix and only base58btc characters."""
    if not text.startswith("z"):
        raise ValueError("missing multibase 'z' prefix")
    body = text[1:]
    value = 0
    for char in body:
        digit = BASE58_ALPHABET.find(char)
        if digit < 0:
            raise ValueError(f"invalid base58 character {char!r}")
        value = value * 58 + digit
    zeros = len(body) - len(body.lstrip("1"))
    return b"\x00" * zeros + value.to_bytes((value.bit_length() + 7) // 8, "big")


def jcs_number(value: float | int) -> str:
    """ECMAScript Number.prototype.toString (RFC 8785 §3.2.2.3)."""
    if isinstance(value, int) and abs(value) > MAX_SAFE_DOUBLE_INT:
        raise ValueError(f"integer {value} is not representable as an IEEE double")
    number = float(value)
    if number == 0:
        return "0"
    # repr() gives the shortest round-trip digits, as ECMAScript requires.
    text = repr(abs(number))
    mantissa, _, exp_part = text.partition("e")
    whole, _, frac = mantissa.partition(".")
    digits = (whole + frac).lstrip("0")
    point = len(whole) + (int(exp_part) if exp_part else 0)
    point -= len(whole + frac) - len((whole + frac).lstrip("0"))
    digits = digits.rstrip("0") or "0"
    k, n = len(digits), point
    sign = "-" if number < 0 else ""
    if k <= n <= 21:
        return sign + digits + "0" * (n - k)
    if 0 < n <= 21:
        return sign + digits[:n] + "." + digits[n:]
    if -6 < n <= 0:
        return sign + "0." + "0" * -n + digits
    tail = "." + digits[1:] if k > 1 else ""
    return f"{sign}{digits[0]}{tail}e{'+' if n > 0 else '-'}{abs(n - 1)}"


def jcs_canonicalize(obj: Any) -> bytes:
    """RFC 8785 JSON Canonicalization Scheme."""
    return _jcs(obj).encode("utf-8")


def _jcs(obj: Any) -> str:
    if obj is None or isinstance(obj, bool):
        return json.dumps(obj)
    if isinstance(obj, (int, float)):
        return jcs_number(obj)
    if isinstance(obj, str):
        return json.dumps(obj, ensure_ascii=False)
    if isinstance(obj, list):
        return "[" + ",".join(_jcs(el) for el in obj) + "]"
    if isinstance(obj, dict):
        keys = sorted(obj, key=lambda k: k.encode("utf-16-be"))
        return "{" + ",".join(
            json.dumps(k, ensure_ascii=False) + ":" + _jcs(obj[k]) for k in keys
        ) + "}"
    raise ValueError(f"not a JSON value: {type(obj).__name__}")


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key, value in pairs:
        if key in out:
            raise ValueError(f"duplicate JSON key {key!r}")
        out[key] = value
    return out


def _reject_constant(name: str) -> None:
    raise ValueError(f"{name} is not valid JSON")


def parse_envelope(text: str) -> Any:
    """Strict JSON: signing covers one reading of the document, so a
    duplicate key or a NaN could otherwise show readers something else."""
    return json.loads(
        text,
        object_pairs_hook=_reject_duplicate_keys,
        parse_constant=_reject_constant,
    )


def load_envelope(path: str) -> Any:
    if path == "-":
        return parse_envelope(sys.stdin.read())
    with open(path, "r", encoding="utf-8") as f:
        return parse_envelope(f.read())


def signing_input(document: dict[str, Any], proof_config: dict[str, Any]) -> bytes:
    """SHA-256(JCS(proof config)) || SHA-256(JCS(document))."""
    return (
        hashlib.sha256(jcs_canonicalize(proof_config)).digest()
        + hashlib.sha256(jcs_canonicalize(document)).digest()
    )


def _verify_signature(
    cryptosuite: str, pubkey_bytes: bytes, signature: bytes, message: bytes
) -> None:
    """Raises on any failure."""
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

    if cryptosuite == "eddsa-jcs-2022":
        Ed25519PublicKey.from_public_bytes(pubkey_bytes).verify(signature, message)
    else:
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), pubkey_bytes)
        der = encode_dss_signature(
            int.from_bytes(signature[:32], "big"),
            int.from_bytes(signature[32:], "big"),
        )
        key.verify(der, message, ec.ECDSA(hashes.SHA256()))


def verify_envelope(envelope: Any, pubkey_bytes: bytes) -> tuple[bool, str]:
    """Returns (ok, message)."""
    if not isinstance(envelope, dict):
        return False, "envelope is not a JSON object"
    proof = envelope.get("proof")
    if not isinstance(proof, dict):
        return False, "envelope has no `proof` block"

    cryptosuite = proof.get("cryptosuite")
    if cryptosuite not in ("eddsa-jcs-2022", "ecdsa-jcs-2019"):
        return False, (
            f"unsupported cryptosuite {cryptosuite!r}; "
            "this verifier handles eddsa-jcs-2022 and ecdsa-jcs-2019"
        )
    if proof.get("type") != "DataIntegrityProof":
        return False, f"unsupported proof type {proof.get('type')!r}"
    if proof.get("proofPurpose") != "assertionMethod":
        return False, f"unsupported proofPurpose {proof.get('proofPurpose')!r}"
    unknown = set(proof) - PROOF_FIELDS
    if unknown:
        return False, f"proof has unsupported fields: {sorted(unknown)}"

    proof_value = proof.get("proofValue")
    if not isinstance(proof_value, str) or not proof_value:
        return False, "proof.proofValue is missing or empty"
    try:
        signature = multibase_base58btc_decode(proof_value)
    except ValueError as e:
        return False, f"proofValue is not valid multibase base58btc: {e}"
    if len(signature) != 64:
        return False, f"signature must be 64 bytes, got {len(signature)}"

    if "@context" in proof:
        context = envelope.get("@context")
        wanted = proof["@context"]
        if (not isinstance(wanted, list) or not isinstance(context, list)
                or context[: len(wanted)] != wanted):
            return False, "proof.@context does not match the envelope's @context"

    # tsa_timestamp is a sidecar added after signing.
    document = {k: v for k, v in envelope.items()
                if k not in ("proof", "tsa_timestamp")}
    proof_config = {k: v for k, v in proof.items() if k != "proofValue"}
    try:
        message = signing_input(document, proof_config)
    except (ValueError, RecursionError) as e:
        return False, f"envelope can't be canonicalized: {e}"

    try:
        import cryptography  # noqa: F401
    except ImportError:
        return False, "`cryptography` is required: pip install cryptography"

    try:
        _verify_signature(cryptosuite, pubkey_bytes, signature, message)
    except Exception:  # noqa: BLE001
        return False, "signature DOES NOT verify against the provided public key"

    return True, "signature OK"


def render_report(envelope: Any, ok: bool, msg: str) -> str:
    if not isinstance(envelope, dict):
        return f"verdict:        FAIL\ndetail:         {msg}"
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
    # A valid signature only shows the key holder made these claims.
    lines.append("note:           evidence_grade is the signer's claim; "
                 "check hardware backing of the key separately")
    if "device_attestation" in subject:
        lines.append("note:           device_attestation token NOT validated")
    if "tsa_timestamp" in envelope:
        lines.append("note:           tsa_timestamp present, NOT validated "
                     "and not covered by the proof")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Verify a Sentlog evidence envelope.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("envelope", help="path to envelope JSON, or '-' for stdin")
    ap.add_argument("pubkey_hex",
                    help="public key as hex: Ed25519, or SEC1 P-256")
    args = ap.parse_args(argv)

    try:
        pubkey_bytes = bytes.fromhex(args.pubkey_hex)
    except ValueError as e:
        print(f"bad PUBKEY_HEX: {e}", file=sys.stderr)
        return 3
    if len(pubkey_bytes) not in (32, 33, 65):
        print(
            f"PUBKEY_HEX must decode to 32, 33 or 65 bytes, got {len(pubkey_bytes)}",
            file=sys.stderr,
        )
        return 3

    try:
        envelope = load_envelope(args.envelope)
    except OSError as e:
        print(f"could not load envelope: {e}", file=sys.stderr)
        return 3
    except (ValueError, RecursionError) as e:  # ValueError covers JSONDecodeError
        print(f"malformed envelope: {e}", file=sys.stderr)
        return 2

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
