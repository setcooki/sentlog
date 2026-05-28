# Sentlog reference consumer

Single-file, stdlib-only HTTP server you can point a `HttpSink` or
`WebhookSink` at to verify the forwarder dispatch path end-to-end.
Used by the Phase 4 exit-gate check ("all four sink types round-trip
through the reference consumer") and convenient for one-shot
integration tests during app development.

## Quick start

```bash
# Plain HTTP receiver (no HMAC, no proof verification)
python3 tools/reference-consumer/server.py --port 8080
```

Then configure an `HttpSink` pointing at `http://127.0.0.1:8080/http`.
Every received envelope is printed to stdout as one NDJSON line.

## With HMAC (for WebhookSink testing)

```bash
python3 tools/reference-consumer/server.py \
    --port 8080 \
    --hmac-secret "shared-with-the-WebhookSink-config" \
    --signature-format "sha256=<hex>"
```

Webhook POSTs without a valid `X-Sentlog-Signature` header are
rejected with HTTP 401. The signature format matches the SDK's
default — `sha256=<hex-digest>`.

## With proof verification

```bash
pip install cryptography

python3 tools/reference-consumer/server.py \
    --port 8080 \
    --verify-pubkey-hex 1a2b3c... \
    --strict   # optional — without --strict, bad proofs are logged but accepted
```

`--verify-pubkey-hex` is the 32-byte Ed25519 public key (64 hex
chars) corresponding to the signing key the SDK was handed via
`Sentlog.setSigningKey`. `cryptography` is the only non-stdlib
dependency, loaded lazily — the server runs without it when you don't
ask for proof verification.

## Endpoints

| Method | Path        | Purpose |
|--------|-------------|---------|
| POST   | `/http`     | HttpSink — no HMAC |
| POST   | `/webhook`  | WebhookSink — HMAC validated when `--hmac-secret` set |
| GET    | `/healthz`  | Liveness probe |
| GET    | `/received` | Last 100 received envelope summaries (in-memory) |

The HTTP / webhook paths are overridable via `--http-path` /
`--webhook-path` if you want to mount the receiver under different
URLs.

## Response codes

| Code | Meaning |
|------|---------|
| 200  | Envelope accepted (or logged in non-strict mode despite shape failure) |
| 400  | Strict mode: shape or proof failure |
| 401  | HMAC missing or mismatched on `/webhook` |
| 404  | Unknown path |

## NDJSON log format

Every accepted POST produces one line on stdout (and on `--log-file`
when set):

```json
{
  "received_at": "2026-05-27T18:23:04.123456+00:00",
  "endpoint":    "/http",
  "shape_ok":    true,
  "shape_msg":   "ok",
  "proof_ok":    true,
  "proof_msg":   "ok",
  "envelope":    { "...": "full envelope as POSTed" }
}
```

`proof_ok` is `null` when you didn't pass `--verify-pubkey-hex`.

## Self-test

```bash
python3 tools/reference-consumer/selftest.py
```

Starts the server on a random local port and runs ~8 assertions
covering `/healthz`, accept-valid-envelope, missing-HMAC rejection,
correct-HMAC acceptance, wrong-HMAC rejection, the `/received`
in-memory list, and strict-mode shape rejection. Exits 0 on success.
