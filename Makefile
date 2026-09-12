################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp-server.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

override PACKAGE  := sysrepo-mcp-server
override VERSION  := 0.1
EXTRA_CFLAGS     := -O2 -DNDEBUG -Wall -Wextra -Wformat=2
EXTRA_LDFLAGS    := -O2

export VERSION EXTRA_CFLAGS EXTRA_LDFLAGS

ifeq ($(strip $(EBUILDDIR)),)
ifneq ($(realpath extern/ebuild/main.mk),)
EBUILDDIR := $(realpath extern/ebuild)
else  # ($(realpath extern/ebuild/main.mk),)
EBUILDDIR := $(realpath /usr/share/ebuild)
endif # !($(realpath extern/ebuild/main.mk),)
endif # ($(strip $(EBUILDDIR)),)

ifeq ($(realpath $(EBUILDDIR)/main.mk),)
$(error '$(EBUILDDIR)': no valid eBuild install found !)
endif # ($(realpath $(EBUILDDIR)/main.mk),)


################################################################################
# Configuration files (optional)
################################################################################

# config-in := config.in  # Uncomment to enable Kconfig-based config.h generation



include $(EBUILDDIR)/main.mk

##############################################################################
# Testing
##############################################################################
#
# `make test` :
#   1. builds the sysrepo-mcp-server binary,
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

