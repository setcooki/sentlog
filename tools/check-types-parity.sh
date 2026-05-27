#!/usr/bin/env bash
# Verify rn/src/types.ts and capacitor/src/types.ts are byte-equal
# to shared/types/sentlog.ts (modulo the generated-header
# prefix). Run by CI's `tools (types parity)` job; failure means
# someone hand-edited a bridge's types.ts instead of editing the
# canonical file and re-running tools/sync-types.sh.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

readonly SOURCE="${REPO_ROOT}/shared/types/sentlog.ts"
readonly TARGETS=(
    "${REPO_ROOT}/rn/src/types.ts"
    "${REPO_ROOT}/capacitor/src/types.ts"
)

if [ ! -f "${SOURCE}" ]; then
    echo "missing canonical types at ${SOURCE}" >&2
    exit 1
fi

# Compute the expected content (header + source) once.
readonly EXPECTED="$(mktemp)"
trap 'rm -f "${EXPECTED}"' EXIT
{
    printf "// GENERATED — DO NOT EDIT. Synced from shared/types/sentlog.ts\n"
    printf "// by tools/sync-types.sh. CI verifies these copies stay byte-equal\n"
    printf "// with the canonical file; edit shared/types/sentlog.ts and re-run\n"
    printf "// the script if you need to change a type.\n\n"
    cat "${SOURCE}"
} > "${EXPECTED}"

fail=0
for target in "${TARGETS[@]}"; do
    if [ ! -f "${target}" ]; then
        echo "FAIL: ${target#${REPO_ROOT}/} missing" >&2
        fail=1
        continue
    fi
    if ! diff -u "${EXPECTED}" "${target}" > /dev/null; then
        echo "FAIL: ${target#${REPO_ROOT}/} drifted from canonical:" >&2
        diff -u "${EXPECTED}" "${target}" | head -40 >&2
        fail=1
    fi
done

if [ "${fail}" -ne 0 ]; then
    echo "Hint: run tools/sync-types.sh and commit the result." >&2
    exit 1
fi
echo "[check-types] both bridge packages match canonical."
