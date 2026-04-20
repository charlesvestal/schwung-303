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

# Sources: plugin entry + Open303 + GuitarML.
# Open303 also includes a C file (fft4g.c) — built via the C compiler.
CC="${CC:-${CROSS_PREFIX}gcc}"

# Compile C sources first.
C_OBJS=()
for c in src/dsp/open303/*.c; do
    obj="build/$(basename "${c%.c}").o"
    echo "  CC  $c"
    "$CC" -O3 -fPIC -std=c99 -c "$c" -o "$obj"
    C_OBJS+=("$obj")
done

# C++ sources.
CXX_SRCS=(src/dsp/plugin.cpp src/dsp/guitarml/guitarml_amp.cpp)
for f in src/dsp/open303/*.cpp; do CXX_SRCS+=("$f"); done

# Eigen is vendored at deps/Eigen/Eigen/... so deps/Eigen is the include root
# for "#include <Eigen/Dense>". RTNeural headers use a configurable backend;
# we force Eigen (matches jc303 via chowdsp).
INCLUDES=(-Isrc/dsp -Isrc/dsp/open303 -Isrc/dsp/guitarml \
          -Isrc/dsp/deps -Isrc/dsp/deps/Eigen -Isrc/dsp/deps/RTNeural)

echo "Compiling DSP..."
"$CXX" -O3 -fPIC -shared -std=c++17 \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare \
    -DRTNEURAL_USE_EIGEN=1 -DRTNEURAL_NO_DEBUG=1 \
    "${INCLUDES[@]}" \
    "${CXX_SRCS[@]}" "${C_OBJS[@]}" \
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
