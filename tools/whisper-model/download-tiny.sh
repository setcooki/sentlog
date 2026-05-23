#!/usr/bin/env bash
# Download the whisper tiny model and stage it into both platform asset
# directories. See README.md for context. Re-run when the upstream
# whisper.cpp pin changes.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Upstream source — ggerganov publishes the canonical ggml-tiny.bin on
# Hugging Face under his whisper.cpp model repo. Pinning by URL +
# SHA-256 (rather than tag) so any silent re-publish is caught.
readonly MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin"
readonly MODEL_SHA256="bd577a113a864445d4c299885e0cb97d4ba92b5f"   # ggml-tiny.bin sha1 at v1.7.x
# Note: HF often surfaces both SHA-256 and SHA-1; the upstream
# model card lists SHA-1. If you bump the pin, update this and run
# `shasum file` to confirm. The script verifies SHA-1 for parity.

readonly STAGE_PATH="${SCRIPT_DIR}/whisper-tiny.bin"
readonly ANDROID_ASSETS="${REPO_ROOT}/android/sentlog/src/main/assets"
readonly IOS_RESOURCES="${REPO_ROOT}/ios/Sources/Sentlog/Resources"

echo "[whisper-model] Downloading from ${MODEL_URL}"
curl -L --fail --progress-bar -o "${STAGE_PATH}" "${MODEL_URL}"

actual="$(shasum "${STAGE_PATH}" | awk '{print $1}')"
if [ "${actual}" != "${MODEL_SHA256}" ]; then
    echo "[whisper-model] SHA-1 mismatch:"
    echo "  expected ${MODEL_SHA256}"
    echo "  actual   ${actual}"
    echo "  (if upstream legitimately changed, update MODEL_SHA256 in this script)"
    rm -f "${STAGE_PATH}"
    exit 1
fi
echo "[whisper-model] SHA-1 OK"

mkdir -p "${ANDROID_ASSETS}" "${IOS_RESOURCES}"
cp "${STAGE_PATH}" "${ANDROID_ASSETS}/whisper-tiny.bin"
cp "${STAGE_PATH}" "${IOS_RESOURCES}/whisper-tiny.bin"
rm -f "${STAGE_PATH}"

echo "[whisper-model] Staged into:"
echo "  ${ANDROID_ASSETS}/whisper-tiny.bin"
echo "  ${IOS_RESOURCES}/whisper-tiny.bin"
echo ""
echo "[whisper-model] Note: ios/Package.swift must also have the"
echo "  .copy(\"Resources/whisper-tiny.bin\") line uncommented for"
echo "  SwiftPM to actually bundle it. See ios/Package.swift's"
echo "  comments and the iOS chunk of the rollout plan."
