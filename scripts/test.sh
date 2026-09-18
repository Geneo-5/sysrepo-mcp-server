#!/bin/bash
################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################
#
# Run the test suite inside the build container.
#
#   scripts/test.sh                 # everything
#   scripts/test.sh -k oven         # a pytest filter
#   scripts/test.sh tests/test_oven.py::test_full_oven_session
#
# Every argument is passed straight to pytest.
#
# The suite starts and stops lighttpd and the oven plugin itself, in a private
# sysrepo repository under the pytest temporary directory, so this script has
# nothing to orchestrate: it only provides the container.
#
################################################################################

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

DOCKER_IMAGE="${DOCKER_IMAGE:-sysrepo-mcp}"
DOCKER_TAG="${DOCKER_TAG:-latest}"
IMAGE="${DOCKER_IMAGE}:${DOCKER_TAG}"

# Port 80 by default, matching docker/lighttpd.conf and the documentation.
TEST_PORT="${SYSREPO_MCP_TEST_PORT:-80}"

log() { echo "==> $*"; }

if ! docker info >/dev/null 2>&1; then
    echo "Docker is not running. Start it and try again." >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    log "Image $IMAGE not found, building it ..."
    make -C "${PROJECT_DIR}/docker" build
fi

if [ ! -x "${PROJECT_DIR}/build/sysrepo-mcp" ]; then
    log "Binary missing, building it ..."
    "${SCRIPT_DIR}/build-docker.sh"
fi

log "Running the test suite in $IMAGE (port ${TEST_PORT}) ..."

# The project is mounted at the same absolute path inside the container so
# that every path pytest prints means the same thing on both sides.
#
# --network none is deliberately NOT used: lighttpd binds 127.0.0.1 inside the
# container's own namespace, and the default bridge already isolates it from
# the host.
docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "${PROJECT_DIR}:${PROJECT_DIR}" \
    -w "${PROJECT_DIR}" \
    -e "SYSREPO_MCP_TEST_PORT=${TEST_PORT}" \
    -e "HOME=/tmp" \
    "$IMAGE" \
    python3 -m pytest tests -v "$@"

log "Tests complete."
