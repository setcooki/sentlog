#!/usr/bin/env python3
"""Self-test for the reference-consumer.

Starts the server on a random local port, fires synthetic envelopes
at /http and /webhook, asserts the response codes + the in-memory
/received state. Run as:

    python3 tools/reference-consumer/selftest.py

Exits 0 on success, non-zero on the first failed assertion.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

# Import the server module from the same directory.
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server as ref_consumer  # noqa: E402


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def post_json(url: str, body: dict, headers: dict | None = None) -> tuple[int, dict]:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def get_json(url: str) -> tuple[int, dict]:
    with urllib.request.urlopen(url, timeout=5) as r:
        return r.status, json.loads(r.read())


# A minimally-valid envelope per the shape check in server.py.
VALID_ENVELOPE = {
    "@context":          ["https://www.w3.org/ns/credentials/v2"],
    "type":              ["VerifiableCredential", "SentlogEvidence"],
    "issuer":            "did:example:test",
    "validFrom":         "2026-05-27T00:00:00Z",
    "credentialSubject": {
        "envelope_format": "sentlog-v1",
        "projection":      "ScoresOnly",
    },
}


def main() -> int:
    port = free_port()
    hmac_secret = "test-secret"

    # Configure state, then start the server in a thread.
    ref_consumer.State.http_path        = "/http"
    ref_consumer.State.webhook_path     = "/webhook"
    ref_consumer.State.hmac_secret      = hmac_secret
    ref_consumer.State.signature_header = "X-Sentlog-Signature"
    ref_consumer.State.signature_format = "sha256=<hex>"
    ref_consumer.State.verify_pubkey_hex = None
    ref_consumer.State.strict           = False
    ref_consumer.State.received.clear()

    httpd = ThreadingHTTPServer(("127.0.0.1", port), ref_consumer.Handler)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()

    base = f"http://127.0.0.1:{port}"
    failures: list[str] = []

    def check(cond: bool, msg: str) -> None:
        if not cond:
            failures.append(msg)
            print(f"FAIL: {msg}", file=sys.stderr)
        else:
            print(f"ok:   {msg}")

    try:
        # 1. healthz works
        time.sleep(0.1)
        code, body = get_json(base + "/healthz")
        check(code == 200 and body.get("status") == "ok", "GET /healthz returns ok")

        # 2. /http accepts a valid envelope
        code, body = post_json(base + "/http", VALID_ENVELOPE)
        check(code == 200 and body.get("received"), "POST /http accepts valid envelope")

        # 3. /http rejects a malformed envelope shape with 200 (logged)
        #    when not strict
        code, body = post_json(base + "/http", {"not": "an envelope"})
        check(code == 200, "POST /http logs (not rejects) malformed envelope in non-strict mode")

        # 4. /webhook without HMAC header rejects with 401
        code, body = post_json(base + "/webhook", VALID_ENVELOPE)
        check(code == 401, "POST /webhook with missing HMAC -> 401")

        # 5. /webhook with correct HMAC accepts
        envelope_bytes = json.dumps(VALID_ENVELOPE).encode("utf-8")
        sig = "sha256=" + hmac.new(
            hmac_secret.encode("utf-8"),
            envelope_bytes,
            hashlib.sha256).hexdigest()
        code, body = post_json(
            base + "/webhook", VALID_ENVELOPE,
            headers={"X-Sentlog-Signature": sig})
        check(code == 200 and body.get("received"),
              "POST /webhook with valid HMAC accepts")

        # 6. /webhook with wrong HMAC rejects with 401
        code, body = post_json(
            base + "/webhook", VALID_ENVELOPE,
            headers={"X-Sentlog-Signature": "sha256=" + "00" * 32})
        check(code == 401, "POST /webhook with wrong HMAC -> 401")

        # 7. /received lists the accepted envelopes
        code, body = get_json(base + "/received")
        # 3 accepted: #2 (http), #3 (http malformed but logged in non-strict), #5 (webhook ok).
        check(code == 200 and body.get("count") == 3,
              f"GET /received reports 3 accepted envelopes (got {body.get('count')})")

        # 8. strict mode: malformed envelope -> 400
        ref_consumer.State.strict = True
        code, body = post_json(base + "/http", {"not": "an envelope"})
        check(code == 400, "POST /http in strict mode -> 400 on shape failure")

    finally:
        httpd.shutdown()

    if failures:
        print(f"\n{len(failures)} failure(s):", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
