#!/usr/bin/env python3
"""Sentlog reference consumer — minimal HTTP server for HttpSink +
WebhookSink testing.

Single-file, stdlib-only HTTP server the integrator (or our own
integration tests) can point an HttpSink / WebhookSink at to verify
the dispatch path end-to-end. Logs every received envelope, validates
basic shape, optionally verifies the WebhookSink HMAC, optionally
verifies the envelope's Data Integrity proof.

Usage:
    python3 server.py [--port 8080]
                       [--http-path /http]
                       [--webhook-path /webhook]
                       [--hmac-secret SECRET]
                       [--signature-header X-Sentlog-Signature]
                       [--signature-format "sha256=<hex>"]
                       [--verify-pubkey-hex 64-hex-chars]
                       [--strict]
                       [--log-file PATH]

Endpoints:
    POST /http      — for HttpSink. No HMAC. 200 on accept, 400 on shape
                      error, 401 on (when --strict) proof failure.
    POST /webhook   — for WebhookSink. Validates HMAC if --hmac-secret
                      given; rejects 401 on mismatch.
    GET  /healthz   — liveness probe.
    GET  /received  — list of received envelope summaries (in-memory,
                      most recent first; capped at the last 100).

Each accepted envelope is appended to stdout (and to --log-file when
given) as one NDJSON line:

    {"received_at": "...", "endpoint": "/http",
     "shape_ok": true, "proof_ok": true|null,
     "envelope": {...full envelope...}}

`proof_ok` is null when --verify-pubkey-hex was not supplied.

Exit codes:
    0 — server exited cleanly (SIGINT)
    1 — server failed to bind / start

Dependencies: stdlib only when not verifying proofs.
              `cryptography` when --verify-pubkey-hex is set.
              Install with: pip install cryptography
"""

from __future__ import annotations

import argparse
import collections
import datetime
import hashlib
import hmac
import json
import signal
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Optional


# --- shape validation ---------------------------------------------

# Required top-level envelope fields per spec §3 + §4. The full schema
# is broader; we check just enough to flag obviously-broken POSTs.
REQUIRED_TOP = {"@context", "type", "issuer", "validFrom", "credentialSubject"}


def envelope_shape_ok(env: Any) -> tuple[bool, str]:
    if not isinstance(env, dict):
        return False, "envelope must be a JSON object"
    missing = REQUIRED_TOP - set(env.keys())
    if missing:
        return False, f"missing required fields: {sorted(missing)}"
    if "VerifiableCredential" not in env.get("type", []):
        return False, "type[] missing 'VerifiableCredential'"
    if "SentlogEvidence" not in env.get("type", []):
        return False, "type[] missing 'SentlogEvidence'"
    cs = env.get("credentialSubject")
    if not isinstance(cs, dict) or "envelope_format" not in cs:
        return False, "credentialSubject.envelope_format missing"
    return True, "ok"


# --- HMAC for WebhookSink -----------------------------------------

def compute_hmac(body: bytes, secret: str, fmt: str) -> str:
    digest = hmac.new(secret.encode("utf-8"), body, hashlib.sha256).digest()
    hex_d  = digest.hex()
    if fmt == "sha256=<hex>":
        return "sha256=" + hex_d
    if fmt == "<hex>":
        return hex_d
    if fmt == "<base64>":
        import base64
        return base64.b64encode(digest).decode("ascii")
    raise ValueError(f"unknown signature_format: {fmt!r}")


def hmac_matches(received: str, expected: str) -> bool:
    # Constant-time compare to avoid leaking the secret via timing.
    return hmac.compare_digest(received, expected)


# --- optional proof verification ----------------------------------

def verify_proof(envelope: dict, pubkey_hex: str) -> tuple[bool, str]:
    """Delegates to the standalone evidence verifier. Imported lazily
    so the server runs without cryptography installed when the
    integrator doesn't ask for proof verification."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "evidence-verifier"))
    from verify import verify_envelope

    try:
        return verify_envelope(envelope, bytes.fromhex(pubkey_hex))
    except ValueError as e:
        return False, f"verify error: {e}"


# --- request handler ----------------------------------------------

class State:
    """Per-server-instance state (handler classes can't carry state
    directly; we stash it on the class)."""
    http_path: str            = "/http"
    webhook_path: str         = "/webhook"
    hmac_secret: Optional[str] = None
    signature_header: str     = "X-Sentlog-Signature"
    signature_format: str     = "sha256=<hex>"
    verify_pubkey_hex: Optional[str] = None
    strict: bool              = False
    log_file: Optional[str]   = None
    received: "collections.deque[dict]" = collections.deque(maxlen=100)


class Handler(BaseHTTPRequestHandler):
    # Keep the default verbose access log to stderr; envelope content
    # goes to stdout / --log-file so the two streams don't tangle.

    def do_GET(self):  # noqa: N802 (BaseHTTPRequestHandler convention)
        if self.path == "/healthz":
            self._send_json(200, {"status": "ok"})
            return
        if self.path == "/received":
            self._send_json(200, {"count": len(State.received),
                                   "items": list(State.received)})
            return
        self._send_json(404, {"error": "not found"})

    def do_POST(self):  # noqa: N802
        if self.path == State.http_path:
            self._handle(envelope_post=True, expect_hmac=False)
            return
        if self.path == State.webhook_path:
            self._handle(envelope_post=True, expect_hmac=True)
            return
        self._send_json(404, {"error": "not found"})

    def _handle(self, envelope_post: bool, expect_hmac: bool) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length) if length > 0 else b""
        except Exception as e:
            self._send_json(400, {"error": f"body read failed: {e}"})
            return

        if expect_hmac and State.hmac_secret is not None:
            received_sig = self.headers.get(State.signature_header, "")
            expected_sig = compute_hmac(
                body, State.hmac_secret, State.signature_format)
            if not hmac_matches(received_sig, expected_sig):
                self._send_json(401, {"error": "HMAC mismatch",
                                       "header": State.signature_header})
                return

        try:
            envelope = json.loads(body.decode("utf-8")) if body else {}
        except Exception as e:
            self._send_json(400, {"error": f"JSON decode: {e}"})
            return

        shape_ok, shape_msg = envelope_shape_ok(envelope)
        proof_ok: Optional[bool] = None
        proof_msg: Optional[str] = None
        if State.verify_pubkey_hex:
            proof_ok, proof_msg = verify_proof(
                envelope, State.verify_pubkey_hex)

        log_entry = {
            "received_at": datetime.datetime.now(
                datetime.timezone.utc).isoformat(),
            "endpoint": self.path,
            "shape_ok": shape_ok,
            "shape_msg": shape_msg,
            "proof_ok": proof_ok,
            "proof_msg": proof_msg,
            "envelope": envelope,
        }
        State.received.appendleft({
            k: v for k, v in log_entry.items() if k != "envelope"
        } | {"issuer": envelope.get("issuer"),
              "validFrom": envelope.get("validFrom")})

        line = json.dumps(log_entry, separators=(",", ":"))
        print(line, flush=True)
        if State.log_file:
            with open(State.log_file, "a", encoding="utf-8") as f:
                f.write(line + "\n")

        if State.strict and (not shape_ok or proof_ok is False):
            self._send_json(400, {"error": "strict-mode rejection",
                                   "shape_ok": shape_ok,
                                   "proof_ok": proof_ok})
            return
        self._send_json(200, {"received": True})

    def _send_json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# --- entry point --------------------------------------------------

def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=8080)
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--http-path", default="/http")
    p.add_argument("--webhook-path", default="/webhook")
    p.add_argument("--hmac-secret")
    p.add_argument("--signature-header", default="X-Sentlog-Signature")
    p.add_argument("--signature-format", default="sha256=<hex>",
                   choices=["sha256=<hex>", "<hex>", "<base64>"])
    p.add_argument("--verify-pubkey-hex",
                   help="32-byte Ed25519 public key as 64 hex chars; when set, "
                        "each envelope's eddsa-jcs-2022 proof is verified")
    p.add_argument("--strict", action="store_true",
                   help="reject (HTTP 400) on shape or proof failure")
    p.add_argument("--log-file",
                   help="path to append received envelope NDJSON lines to")
    args = p.parse_args(argv)

    State.http_path        = args.http_path
    State.webhook_path     = args.webhook_path
    State.hmac_secret      = args.hmac_secret
    State.signature_header = args.signature_header
    State.signature_format = args.signature_format
    State.verify_pubkey_hex = args.verify_pubkey_hex
    State.strict           = args.strict
    State.log_file         = args.log_file

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print(f"reference-consumer listening on http://{args.bind}:{args.port}",
          file=sys.stderr)
    print(f"  HTTP    POST {args.http_path}", file=sys.stderr)
    print(f"  Webhook POST {args.webhook_path}"
          + ("  [HMAC required]" if args.hmac_secret else "  [HMAC optional]"),
          file=sys.stderr)
    if args.verify_pubkey_hex:
        print(f"  Proof verify ON ({len(args.verify_pubkey_hex)} hex chars)",
              file=sys.stderr)

    def shutdown(_sig, _frame):
        print("reference-consumer shutting down", file=sys.stderr)
        server.shutdown()
    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    try:
        server.serve_forever()
    except OSError as e:
        print(f"reference-consumer failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
