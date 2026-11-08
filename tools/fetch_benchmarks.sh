#!/usr/bin/env bash
# Downloads the ISCAS-85 .bench netlists into benchmarks/downloaded/ and checks
# them against pinned SHA-256 hashes (tools/benchmarks.sha256).
#
# Source: release v1.0.2 of github.com/Coloquinte/LogicBenchmarks. That
# repository keeps no netlists in git; they are published only inside the
# release zip, so we download the zip, check its hash, and extract the files.
# No licence is stated for these files, so they are not committed (see NOTICE.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/benchmarks/downloaded"
HASHES="${ROOT}/tools/benchmarks.sha256"
ZIP_URL="https://github.com/Coloquinte/LogicBenchmarks/releases/download/v1.0.2/logic_benchmarks.zip"
ZIP_SHA256="91012556d72d5f0cc66087b021530bf069f31b4d574a9fa0108283a5b5e7342a"
CIRCUITS="c432 c499 c880 c1355 c1908 c2670 c3540 c5315 c6288 c7552"

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "downloading ${ZIP_URL}" >&2
curl --fail --silent --show-error --location --output "${TMP}/benchmarks.zip" "${ZIP_URL}"
echo "${ZIP_SHA256}  ${TMP}/benchmarks.zip" | sha256sum --check --quiet

mkdir -p "${DEST}"
for circuit in ${CIRCUITS}; do
    # -p writes the member to stdout, so we choose the output name ourselves.
    unzip -p "${TMP}/benchmarks.zip" "bench/iscas85-${circuit}.bench" > "${DEST}/${circuit}.bench"
done

if [[ "${1:-}" == "--print-hashes" ]]; then
    # Used once to create tools/benchmarks.sha256 after reviewing the files.
    (cd "${DEST}" && sha256sum *.bench)
    exit 0
fi

# sha256sum does not accept comment lines, so they are filtered out first.
grep -v "^#" "${HASHES}" | (cd "${DEST}" && sha256sum --check --strict)
echo "OK: ISCAS-85 netlists in ${DEST}"
