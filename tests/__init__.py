# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

"""
sysrepo-mcp test suite.

tests/ is a package so that the test modules can import shared helpers from
conftest with a relative import (`from .conftest import ...`). Removing this
file breaks those imports.

Run it with `make test` inside the container, or directly:

    python3 -m pytest tests -v

Everything runs against lighttpd on port 80. Override with
SYSREPO_MCP_TEST_PORT when that port is unavailable, and with
SYSREPO_MCP_BIN when the binary is not at build/sysrepo-mcp.
"""
