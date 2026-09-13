#!/bin/bash
################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################
set -euo pipefail

THIS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$THIS_DIR")"

# Configure library paths for extern/ compiled libraries
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="/usr/local/lib:/usr/local/lib64:$LD_LIBRARY_PATH"
export LIBRARY_PATH="/usr/local/lib:$LIBRARY_PATH"
export C_INCLUDE_PATH="/usr/local/include:$C_INCLUDE_PATH"
export CPLUS_INCLUDE_PATH="/usr/local/include:$CPLUS_INCLUDE_PATH"

echo "==> Building sysrepo-mcp in $PROJECT_DIR"

cd "$PROJECT_DIR"

# Step 1: configure (optional, detects dependencies)
if [ -f "config.in" ]; then
        echo "==> Running make configure..."
        make configure || true
fi

# Step 2: build
echo "==> Running make..."
make || exit 1

# Step 3: install (local, no sudo needed in container)
echo "==> Running make install..."
make install PREFIX=/usr/local || true

echo "==> Build complete!"
echo "  Binary: $PROJECT_DIR/build/sysrepo-mcp"
