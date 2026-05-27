#!/usr/bin/env bash
# Sync the canonical Sentlog TypeScript types into each bridge
# package's src/types.ts. Run after editing shared/types/sentlog.ts.
# CI's `tools (types parity)` job verifies the copies haven't
# drifted.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

readonly SOURCE="${REPO_ROOT}/shared/types/sentlog.ts"
readonly TARGETS=(
    "${REPO_ROOT}/rn/src/types.ts"
    "${REPO_ROOT}/capacitor/src/types.ts"
)

readonly HEADER="// GENERATED — DO NOT EDIT. Synced from shared/types/sentlog.ts
// by tools/sync-types.sh. CI verifies these copies stay byte-equal
// with the canonical file; edit shared/types/sentlog.ts and re-run
// the script if you need to change a type.
"

if [ ! -f "${SOURCE}" ]; then
    echo "missing canonical types at ${SOURCE}" >&2
    exit 1
fi

for target in "${TARGETS[@]}"; do
    mkdir -p "$(dirname "${target}")"
    {
        printf "%s\n" "${HEADER}"
        cat "${SOURCE}"
    } > "${target}"
    echo "[sync-types] wrote ${target#${REPO_ROOT}/}"
done
