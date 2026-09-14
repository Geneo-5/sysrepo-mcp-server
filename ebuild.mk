################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

srcdir := src
srctop := $(TOPDIR)/$(srcdir)

bins                   += $(PACKAGE)
$(PACKAGE)-objs        := $(srcdir)/main.o
main.o-src             := $(srctop)/main.c
$(PACKAGE)-cflags      := -I$(TOPDIR)/include \
                          -DPACKAGE_NAME='"$(PACKAGE)"' \
                          -DPACKAGE_VERSION='"$(VERSION)"'
$(PACKAGE)-ldflags     := $(shell pkg-config --libs json-c sysrepo 2>/dev/null) -lfcgi


################################################################################
# Configuration files
################################################################################

config-in  := config.in
doxyconf   := $(TOPDIR)/sphinx/Doxyfile
sphinxsrc  := $(TOPDIR)/sphinx


##############################################################################
# Testing
##############################################################################
#
# `make test` :
#   1. builds the sysrepo-mcp binary,
#   2. smoke-checks the binary (--help / --version),
#   3. runs the pytest suite in tests/ (if present).
#
# Library paths point at the libraries installed by the container (see
# docker/Dockerfile : installed under /usr/local).
#
PYTEST ?= python3 -m pytest
TESTS  ?= tests

.PHONY: test
test: build
	@echo "==> Smoke testing $(BUILDDIR)/$(PACKAGE) ..."
	$(BUILDDIR)/$(PACKAGE) --help
	$(BUILDDIR)/$(PACKAGE) --version
	@if [ -d "$(TESTS)" ]; then \
		echo "==> Running test suite ($(PYTEST)) ..."; \
		PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:$$PKG_CONFIG_PATH" \
		LD_LIBRARY_PATH="/usr/local/lib:$$LD_LIBRARY_PATH" \
		$(PYTEST) -v "$(TESTS)" || exit 1; \
	else \
		echo "==> No $(TESTS)/ directory found, nothing to test."; \
	fi

