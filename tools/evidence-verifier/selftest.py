#!/usr/bin/env python3
"""Self-test for the Sentlog evidence verifier.

Runs in CI as part of the `tools (evidence-verifier)` job. Covers:

- W3C vc-di-eddsa and vc-di-ecdsa `jcs` test vectors.
- fixtures/cpp_signed_envelope.json, signed by the C++ core.
  shared/tests/signing_test.cpp fails if the core stops producing that
  exact file, so the two jobs together catch C++/Python drift.
- RFC 8785 number and key-ordering examples.
- Rejection of tampered, malleated and ambiguous envelopes.

No external deps beyond `cryptography` (same as verify.py).
"""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))

from verify import (  # noqa: E402
    jcs_canonicalize,
    jcs_number,
    parse_envelope,
    verify_envelope,
)

# RFC 8032 §7.1 test vector 1 — same key as shared/tests/signing_test.cpp.
FIXTURE_PUBKEY = bytes.fromhex(
    "d75a980182b10ab7d54bfed3c964073a"
    "0ee172f3daa62325af021a68f707511a"
)
FIXTURE = HERE / "fixtures" / "cpp_signed_envelope.json"

# w3c/vc-di-eddsa TestVectors/eddsa-jcs-2022/signedJCS.json
W3C_EDDSA_PUBKEY = bytes.fromhex("b00d8d938e7f773d51565aad36a623f5344f7f5d1960f9cf3e8e12620ea2810f")
W3C_EDDSA = r"""{"@context":["https://www.w3.org/ns/credentials/v2","https://www.w3.org/ns/credentials/examples/v2"],"id":"urn:uuid:58172aac-d8ba-11ed-83dd-0b3aef56cc33","type":["VerifiableCredential","AlumniCredential"],"name":"Alumni Credential","description":"A minimum viable example of an Alumni Credential.","issuer":"https://vc.example/issuers/5678","validFrom":"2023-01-01T00:00:00Z","credentialSubject":{"id":"did:example:abcdefgh","alumniOf":"The School of Examples"},"proof":{"type":"DataIntegrityProof","cryptosuite":"eddsa-jcs-2022","created":"2023-02-24T23:36:38Z","verificationMethod":"did:key:z6MkrJVnaZkeFzdQyMZu1cgjg7k1pZZ6pvBQ7XJPt4swbTQ2#z6MkrJVnaZkeFzdQyMZu1cgjg7k1pZZ6pvBQ7XJPt4swbTQ2","proofPurpose":"assertionMethod","@context":["https://www.w3.org/ns/credentials/v2","https://www.w3.org/ns/credentials/examples/v2"],"proofValue":"z2HnFSSPPBzR36zdDgK8PbEHeXbR56YF24jwMpt3R1eHXQzJDMWS93FCzpvJpwTWd3GAVFuUfjoJdcnTMuVor51aX"}}"""

# w3c/vc-di-ecdsa TestVectors/ecdsa-jcs-2019-p256/signedJCSECDSAP256.json
W3C_ECDSA_PUBKEY = bytes.fromhex("0360fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6")
W3C_ECDSA = r"""{"@context":["https://www.w3.org/ns/credentials/v2","https://www.w3.org/ns/credentials/examples/v2"],"id":"urn:uuid:58172aac-d8ba-11ed-83dd-0b3aef56cc33","type":["VerifiableCredential","AlumniCredential"],"name":"Alumni Credential","description":"A minimum viable example of an Alumni Credential.","issuer":"https://vc.example/issuers/5678","validFrom":"2023-01-01T00:00:00Z","credentialSubject":{"id":"did:example:abcdefgh","alumniOf":"The School of Examples"},"proof":{"type":"DataIntegrityProof","cryptosuite":"ecdsa-jcs-2019","created":"2023-02-24T23:36:38Z","verificationMethod":"did:key:zDnaepBuvsQ8cpsWrVKw8fbpGpvPeNSjVPTWoq6cRqaYzBKVP#zDnaepBuvsQ8cpsWrVKw8fbpGpvPeNSjVPTWoq6cRqaYzBKVP","proofPurpose":"assertionMethod","@context":["https://www.w3.org/ns/credentials/v2","https://www.w3.org/ns/credentials/examples/v2"],"proofValue":"z5ptCet75SaEgzG4v4zJhbJtfNi74Wv7Fq15hhKouJQQjEPQvPZKaYxcMXAMLPQS2FXrkCWokNJkFVkwxNzZfD5oT"}}"""

failures: list[str] = []


def check(condition: bool, label: str) -> None:
    print(f"{'ok  ' if condition else 'FAIL'} {label}")
    if not condition:
        failures.append(label)


def accepted(envelope: object, pubkey: bytes) -> bool:
    return verify_envelope(envelope, pubkey)[0]


def rejected_as_bad_signature(envelope: object, pubkey: bytes) -> bool:
    ok, msg = verify_envelope(envelope, pubkey)
    return not ok and "DOES NOT verify" in msg


def test_jcs() -> None:
    numbers = {
        0.0: "0", -0.0: "0", 5e-324: "5e-324",
        1.7976931348623157e308: "1.7976931348623157e+308",
        -1.7976931348623157e308: "-1.7976931348623157e+308",
        9007199254740992: "9007199254740992",
        295147905179352830000.0: "295147905179352830000",
        1e21: "1e+21", 1e20: "100000000000000000000", 1e-7: "1e-7",
        0.000001: "0.000001", 1e-05: "0.00001", 333333333.33333329: "333333333.3333333",
        4.5: "4.5", 2e-3: "0.002", 0.6000000238418579: "0.6000000238418579",
        1500: "1500",
    }
    for value, expected in numbers.items():
        check(jcs_number(value) == expected, f"jcs number {value!r} -> {expected}")

    try:
        jcs_number(2**53 + 1)
        check(False, "jcs rejects integers beyond double precision")
    except ValueError:
        check(True, "jcs rejects integers beyond double precision")

    keys = {"\u20ac": 1, "\r": 2, "\U0001f600": 3, "\ufb33": 4, "1": 5, "\u0080": 6, "\u00f6": 7}
    expected = (
        '{"\\r":2,"1":5,"\u0080":6,"\u00f6":7,"\u20ac":1,"\U0001f600":3,"\ufb33":4}'
    ).encode("utf-8")
    check(jcs_canonicalize(keys) == expected, "jcs keys sort by UTF-16 code unit")
    check(jcs_canonicalize(["gr\u00fc\u00df\n\x1f/"]) ==
          '["gr\u00fc\u00df\\n\\u001f/"]'.encode("utf-8"),
          "jcs keeps raw UTF-8 with minimal escapes")


def test_w3c_vectors() -> None:
    check(accepted(json.loads(W3C_EDDSA), W3C_EDDSA_PUBKEY), "W3C eddsa-jcs-2022 vector verifies")
    check(accepted(json.loads(W3C_ECDSA), W3C_ECDSA_PUBKEY), "W3C ecdsa-jcs-2019 vector verifies")
    for name, text, key in (("eddsa", W3C_EDDSA, W3C_EDDSA_PUBKEY),
                            ("ecdsa", W3C_ECDSA, W3C_ECDSA_PUBKEY)):
        tampered = json.loads(text)
        tampered["name"] = "Forged"
        check(rejected_as_bad_signature(tampered, key), f"tampered {name} vector rejected")


def test_cpp_fixture() -> None:
    envelope = parse_envelope(FIXTURE.read_text(encoding="utf-8"))
    pubkey = FIXTURE_PUBKEY
    check(accepted(envelope, pubkey), "C++ envelope with non-ASCII text verifies")

    stamped = copy.deepcopy(envelope)
    stamped["tsa_timestamp"] = {"provider": "https://tsa.example", "token_b64": "AAAA"}
    check(accepted(stamped, pubkey), "C++ envelope with tsa_timestamp sidecar verifies")

    tampered = copy.deepcopy(envelope)
    tampered["credentialSubject"]["payload"]["transcript"]["text"] = "TAMPERED"
    check(rejected_as_bad_signature(tampered, pubkey), "tampered payload rejected")

    regraded = copy.deepcopy(envelope)
    regraded["credentialSubject"]["evidence_grade"] = "EnclaveSigned"
    check(rejected_as_bad_signature(regraded, pubkey), "edited evidence_grade rejected")

    moved = copy.deepcopy(envelope)
    moved["proof"]["created"] = "2030-01-01T00:00:00.000Z"
    check(rejected_as_bad_signature(moved, pubkey), "edited proof field rejected")

    check(rejected_as_bad_signature(envelope, b"\x01" + bytes(31)), "wrong pubkey rejected")


def test_malformed() -> None:
    envelope = parse_envelope(FIXTURE.read_text(encoding="utf-8"))
    pubkey = FIXTURE_PUBKEY

    def variant(**proof_changes: object) -> dict:
        out = copy.deepcopy(envelope)
        out["proof"].update(proof_changes)
        return out

    value = envelope["proof"]["proofValue"]
    bad = {
        "base64url proofValue": variant(proofValue="AAAA"),
        "non-base58 character": variant(proofValue=value[:-1] + "0"),
        "leading-zero padded signature": variant(proofValue="z1" + value[1:]),
        "unknown proof field": variant(extra="x"),
        "wrong proof type": variant(type="Ed25519Signature2020"),
        "wrong proofPurpose": variant(proofPurpose="authentication"),
        "mismatched proof @context": variant(**{"@context": ["https://evil.example"]}),
        "unknown cryptosuite": variant(cryptosuite="none"),
    }
    for label, candidate in bad.items():
        ok, msg = verify_envelope(candidate, pubkey)
        check(not ok and "DOES NOT verify" not in msg, f"{label} rejected as malformed")

    check(not accepted([], pubkey), "non-object envelope rejected")

    for label, text in (("duplicate key", '{"a":1,"a":2}'), ("NaN", '{"a":NaN}')):
        try:
            parse_envelope(text)
            check(False, f"{label} rejected")
        except ValueError:
            check(True, f"{label} rejected")


def test_cli() -> None:
    run = lambda *args, **kw: subprocess.run(  # noqa: E731
        [sys.executable, str(HERE / "verify.py"), *args],
        capture_output=True, text=True, check=False, **kw)
    pubkey = FIXTURE_PUBKEY.hex()
    result = run(str(FIXTURE), pubkey)
    check(result.returncode == 0 and "PASS" in result.stdout, "CLI exits 0 on a valid envelope")

    with tempfile.TemporaryDirectory() as tmp:
        envelope = parse_envelope(FIXTURE.read_text(encoding="utf-8"))
        envelope["issuer"] = "did:example:other"
        path = Path(tmp) / "tampered.json"
        path.write_text(json.dumps(envelope), encoding="utf-8")
        check(run(str(path), pubkey).returncode == 1, "CLI exits 1 on a tampered envelope")
        path.write_text('{"proof":{},"proof":{}}', encoding="utf-8")
        check(run(str(path), pubkey).returncode == 2, "CLI exits 2 on a duplicate key")
    check(run(str(FIXTURE), "abcd").returncode == 3, "CLI exits 3 on a bad key length")


def main() -> int:
    test_jcs()
    test_w3c_vectors()
    test_cpp_fixture()
    test_malformed()
    test_cli()
    if failures:
        print(f"selftest FAILED: {len(failures)} check(s)", file=sys.stderr)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
