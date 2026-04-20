#!/usr/bin/env bash
# Build 303 module for Schwung (aarch64, Move hardware).
# Uses Docker for cross-compilation; set CROSS_PREFIX to skip Docker.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

IMAGE_NAME="schwung-303-builder"
MODULE_ID="303"
DIST_DIR="dist/${MODULE_ID}"
TARBALL="dist/${MODULE_ID}-module.tar.gz"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== 303 build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
CXX="${CXX:-${CROSS_PREFIX}g++}"

rm -rf dist build
mkdir -p "$DIST_DIR" build

# Sources: plugin entry + Open303 + GuitarML (populated during implementation).
SRCS=(src/dsp/plugin.cpp)
# shellcheck disable=SC2086
if compgen -G "src/dsp/open303/*.cpp" > /dev/null; then
    SRCS+=($(ls src/dsp/open303/*.cpp))
fi
if compgen -G "src/dsp/guitarml/*.cpp" > /dev/null; then
    SRCS+=($(ls src/dsp/guitarml/*.cpp))
fi
if compgen -G "src/dsp/guitarml/neural_utils/*.cpp" > /dev/null; then
    SRCS+=($(ls src/dsp/guitarml/neural_utils/*.cpp))
fi

INCLUDES=(-Isrc/dsp -Isrc/dsp/open303 -Isrc/dsp/guitarml -Isrc/dsp/deps)

echo "Compiling DSP..."
"$CXX" -O3 -fPIC -shared -std=c++17 -Wall -Wextra -Wno-unused-parameter \
    "${INCLUDES[@]}" \
    "${SRCS[@]}" \
    -o build/dsp.so \
    -lm

echo "Packaging..."
# ExtFS-mounted volumes on macOS reject close/dealloc after cp; use cat.
cat build/dsp.so > "$DIST_DIR/dsp.so"
chmod 0755 "$DIST_DIR/dsp.so"
cat src/module.json > "$DIST_DIR/module.json"
cat src/ui.js > "$DIST_DIR/ui.js"
[ -f src/help.json ] && cat src/help.json > "$DIST_DIR/help.json" || true

if [ -d src/models ]; then
    mkdir -p "$DIST_DIR/models"
    (cd src/models && find . -name '*.json' -print0 | while IFS= read -r -d '' f; do
        mkdir -p "$REPO_ROOT/$DIST_DIR/models/$(dirname "$f")"
        cat "$f" > "$REPO_ROOT/$DIST_DIR/models/$f"
    done)
fi

(cd dist && tar -czf "${MODULE_ID}-module.tar.gz" "${MODULE_ID}/")

echo "Built: $TARBALL"
ls -lh "$TARBALL"
file "$DIST_DIR/dsp.so"
