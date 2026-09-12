#!/bin/bash
#
# ==============================================================================
# sysrepo-mcp-server -- Docker test runner
#
# Spins up the sysrepo-mcp-server *build environment* image and runs
# `make test` inside it (which builds the project and runs the test suite).
#
# The image (built by scripts/build.sh) is a build environment only: it holds
# every build & runtime dependency (the extern/ libraries, lighttpd, ...) but
# does NOT contain the compiled project. That happens here, at run time, on
# the bind-mounted working directory.
#
# Usage:
#   scripts/test.sh [test-filter]
#
#   test-filter : optional argument passed to pytest to select tests, e.g.
#                 scripts/test.sh test_config   # single file
#                 scripts/test.sh -k "auth"     # keyword filter
#
# ==============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths and configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="sysrepo-mcp-builder"
IMAGE_TAG="${IMAGE_NAME}:latest"
BUILD_CONTEXT="$PROJECT_ROOT"
DOCKERFILE="$PROJECT_ROOT/docker/Dockerfile"

# ---------------------------------------------------------------------------
# 1. Locate the image
# ---------------------------------------------------------------------------
if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    echo "Image $IMAGE_TAG not found. Building it now..."
    docker build -t "$IMAGE_TAG" -f "$DOCKERFILE" "$BUILD_CONTEXT"
fi

# ---------------------------------------------------------------------------
# 2. Run `make test` inside the image
# ---------------------------------------------------------------------------
cd "$PROJECT_ROOT"

if [ $# -gt 0 ]; then
    echo "Running: make test $* (in container)"
    docker run --rm \
        -u "$(id -u):$(id -g)" \
        -v "${PROJECT_ROOT}:/workspace" \
        -w /workspace \
        "$IMAGE_TAG" \
        make test "$@"
else
    echo "Running: make test (in container)"
    docker run --rm \
        -u "$(id -u):$(id -g)" \
        -v "${PROJECT_ROOT}:/workspace" \
        -w /workspace \
        "$IMAGE_TAG" \
        make test
fi
