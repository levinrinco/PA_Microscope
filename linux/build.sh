#!/usr/bin/env bash
# Linux build for PA_Microscope. The shared source tree is one level up.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-build}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
BUILD_DIR="${BUILD_DIR:-${ROOT}/linux/build}"
BUILD_APP="${PA_BUILD_APP:-ON}"

case "${MODE}" in
    clean)
        rm -rf "${BUILD_DIR}"
        echo "Removed ${BUILD_DIR}/"
        exit 0
        ;;
    build|test) ;;
    *)
        echo "usage: $(basename "$0") [build|test|clean]" >&2
        exit 2
        ;;
esac

# Prefer Ninja if available, else fall back to Unix Makefiles.
if command -v ninja >/dev/null 2>&1; then
    GEN=(-G Ninja)
else
    GEN=(-G "Unix Makefiles")
fi

echo "=== [1/3] Configure (${GEN[*]}, ${BUILD_TYPE}, PA_BUILD_APP=${BUILD_APP}) ==="
cmake -S "${ROOT}" -B "${BUILD_DIR}" "${GEN[@]}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DPA_BUILD_APP="${BUILD_APP}" \
      -DBUILD_TESTING=ON

echo "=== [2/3] Build ==="
cmake --build "${BUILD_DIR}" --parallel

if [ "${MODE}" = "test" ]; then
    echo "=== [3/3] Test ==="
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo
echo "Build OK -> ${BUILD_DIR}/PA_Microscope"
echo "Run it with:  ${BUILD_DIR}/PA_Microscope"
