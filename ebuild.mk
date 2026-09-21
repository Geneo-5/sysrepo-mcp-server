################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

################################################################################
# Configuration files
################################################################################

config-in  := config.in
HEADERDIR  := $(TOPDIR)/include

doxyconf   := $(TOPDIR)/sphinx/Doxyfile
doxyenv    := SRCDIR="$(HEADERDIR) $(SRCDIR)"
sphinxsrc  := $(TOPDIR)/sphinx

subdirs := src

##############################################################################
# Testing
##############################################################################
#
# `make test' :
#   1. builds the sysrepo-mcp binary,
#   2. runs the pytest suite in tests/ (if present).
#
# The suite talks to the server the way a client does, over the FastCGI socket
# or through lighttpd. The binary is never invoked directly to probe its
# behaviour: it is a FastCGI responder, and running it from a terminal only
# proves that it refuses to run from a terminal.
#
# Library paths point at the libraries the container installs under
# /usr/local (see docker/Dockerfile).
#
PYTEST ?= python3 -m pytest
TESTS  ?= tests

.PHONY: test
test: build
	@if [ -d "$(TESTS)" ]; then \
		echo "==> Running test suite ($(PYTEST)) ..."; \
		PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:$$PKG_CONFIG_PATH" \
		LD_LIBRARY_PATH="/usr/local/lib:$$LD_LIBRARY_PATH" \
		SYSREPO_MCP_BIN="$(BUILDDIR)/src/$(PACKAGE)" \
		$(PYTEST) -v "$(TESTS)" $(PYTEST_ARGS)|| exit 1; \
	else \
		echo "==> No $(TESTS)/ directory found, nothing to test."; \
	fi

################################################################################
# Source code tags generation
################################################################################

tagfiles := $(shell find $(CURDIR) $(HEADERDIR) -type f)