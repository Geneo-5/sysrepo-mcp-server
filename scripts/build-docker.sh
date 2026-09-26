#!/bin/bash
################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

set -euo pipefail

# Project root (where this script lives)
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_IMAGE="sysrepo-mcp"
DOCKER_TAG="latest"

# Current user and group IDs (matches docker/Makefile convention)
DOCKER_UID="$(id -u):$(id -g)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

usage() {
    echo "Usage: $(basename "$0") [options]"
    echo ""
    echo "Build and test sysrepo-mcp inside Docker."
    echo ""
    echo "Options:"
    echo "  --doc        Build documentation (html, pdf, man)"
    echo "  --doc-html   Build HTML documentation only"
    echo "  --doc-pdf    Build PDF documentation only"
    echo "  --doc-man    Build man pages only"
    echo "  --clean      Clean build (no cache)"
    echo "  --force      Force rebuild (image + sources)"
    echo "  --test       Run smoke tests after build"
    echo "  --test=ARGS  Run smoke tests after build with ARG"
    echo "  --run        Run after build"
    echo "  --help       Show this help"
    echo ""
    echo "Examples:"
    echo "  $(basename "$0")              # Build binary only"
    echo "  $(basename "$0") --doc        # Build binary + all docs"
    echo "  $(basename "$0") --doc-html   # Build binary + HTML docs"
    echo "  $(basename "$0") --test       # Build + smoke test"
    echo "  $(basename "$0") --run        # Build + run"
}

# Parse arguments
DOCS=0
DOC_MODE="all"  # all, html, pdf, man
CLEAN=0
FORCE=0
TEST=0
TEST_ARGS=""
RUN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --doc)
            DOCS=1
            DOC_MODE="doc"
            shift
            ;;
        --doc-html)
            DOCS=1
            DOC_MODE="html"
            shift
            ;;
        --doc-pdf)
            DOCS=1
            DOC_MODE="pdf"
            shift
            ;;
        --doc-man)
            DOCS=1
            DOC_MODE="man"
            shift
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --run)
            RUN=1
            shift
            ;;
        --test)
            TEST=1
            shift
            ;;
        --test=*)
            TEST_ARGS=${1#*=}
            TEST=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Check Docker is running
if ! docker info &>/dev/null; then
    log_error "Docker is not running. Start Docker and try again."
    exit 1
fi

# Step 1: Download extern sources (only if --force or not present)
if [ "$FORCE" -eq 1 ] || [ ! -d "${PROJECT_DIR}/extern/ebuild" ]; then
    log_info "Downloading extern sources..."
    docker run --rm -u "${DOCKER_UID}" \
        -v "${PROJECT_DIR}:${PROJECT_DIR}" \
        -w "${PROJECT_DIR}" \
        "${DOCKER_IMAGE}:${DOCKER_TAG}" \
        make -C "${PROJECT_DIR}/docker" extern 2>&1 | grep -v "avertissement"
    log_info "Extern sources ready."
else
    log_info "Extern sources already present, skipping download."
fi

# Step 2: Build Docker image (only if --force or not present)
if [ "$FORCE" -eq 1 ] || [ ! "$(docker image inspect --format '{{.Id}}' "${DOCKER_IMAGE}:${DOCKER_TAG}" 2>/dev/null)" ]; then
    log_info "Building Docker image ${DOCKER_IMAGE}:${DOCKER_TAG} ..."
    if [ "$CLEAN" -eq 1 ]; then
        log_warn "Clean build (no cache)."
        make -C "${PROJECT_DIR}/docker" build-nc 2>&1 | grep -v "avertissement"
    else
        make -C "${PROJECT_DIR}/docker" build 2>&1 | grep -v "avertissement"
    fi
    log_info "Docker image built: ${DOCKER_IMAGE}:${DOCKER_TAG}"
else
    log_info "Docker image ${DOCKER_IMAGE}:${DOCKER_TAG} already exists, skipping build."
fi

# Step 3: Build documentation (if --doc)
if [ "$DOCS" -eq 1 ]; then
    case "${DOC_MODE}" in
        doc)
            log_info "Building all documentation (html + pdf + info + man)..."
            ;;
        html)
            log_info "Building HTML documentation..."
            ;;
        pdf)
            log_info "Building PDF documentation..."
            ;;
        man)
            log_info "Building man pages..."
            ;;
    esac

    docker run --rm -u "${DOCKER_UID}" \
        -v "${PROJECT_DIR}:${PROJECT_DIR}" \
        -w "${PROJECT_DIR}" \
        "${DOCKER_IMAGE}:${DOCKER_TAG}" \
        make "${DOC_MODE}" 2>&1 | grep -v "avertissement" | grep -v "can't cd" | grep -v "ModuleNotFoundError" | grep -v "Traceback"

    case "${DOC_MODE}" in
        all)
            log_info "Documentation built:"
            log_info "  HTML:  ${PROJECT_DIR}/build/doc/html/"
            log_info "  PDF:   ${PROJECT_DIR}/build/doc/pdf/"
            log_info "  Info:  ${PROJECT_DIR}/build/doc/info/"
            log_info "  Man:   ${PROJECT_DIR}/build/doc/man/"
            ;;
        html)
            log_info "HTML documentation: ${PROJECT_DIR}/build/doc/html/"
            ;;
        pdf)
            log_info "PDF documentation: ${PROJECT_DIR}/build/doc/pdf/"
            ;;
        man)
            log_info "Man pages: ${PROJECT_DIR}/build/doc/man/"
            ;;
    esac
fi

export EXTRA_CFLAGS="-Werror -Wmissing-declarations -Wstrict-prototypes \
                     -Wextra -Wshadow -Wformat=2 -Wuninitialized \
                     -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                     -O0 -ggdb3"
export EXTRA_LDFLAGS="-Wl,-z,relro,-z,now"

# Step 4: Compile binary
log_info "Compiling in Docker container..."
docker run --rm -u "${DOCKER_UID}" \
    -v "${PROJECT_DIR}:${PROJECT_DIR}" \
    -w "${PROJECT_DIR}" \
    "${DOCKER_IMAGE}:${DOCKER_TAG}" \
    make  EXTRA_CFLAGS="${EXTRA_CFLAGS}" EXTRA_LDFLAGS="${EXTRA_LDFLAGS}" clean defconfig build

log_info "Build complete. Binary: ${PROJECT_DIR}/build/sysrepo-mcp"

# Step 5: Smoke test (if requested)
if [ "$TEST" -eq 1 ]; then
    log_info "Running smoke tests..."
    docker run --rm \
        -v "${PROJECT_DIR}:${PROJECT_DIR}" \
        -w "${PROJECT_DIR}" \
        "${DOCKER_IMAGE}:${DOCKER_TAG}" \
        make test PYTEST_ARGS="${TEST_ARGS}"
fi

# Step 6: Run (if requested)
if [ "$RUN" -eq 1 ]; then
    log_info "Running service..."
    docker run --rm -it \
        -p 80:80 \
        -p 443:443 \
        -v "${PROJECT_DIR}:${PROJECT_DIR}" \
        -w "${PROJECT_DIR}" \
        "${DOCKER_IMAGE}:${DOCKER_TAG}" \
        docker/run.sh
fi

log_info "Done."
