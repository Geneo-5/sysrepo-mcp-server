################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp-server.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

srcdir := src
srctop := $(TOPDIR)/$(srcdir)

#
# Binary declaration : the sysrepo-mcp-server executable.
#
# `bins` tells eBuild which executables to build and install, `*-objs`
# lists the objects that are linked into the binary and `*-cflags` /
# `*-ldflags` provide the compile and link flags.
#
# Note: eBuild's per-object include rules are limited to quoted include
# directories (HEADERDIR / the source directory), so the project's public
# include path is added explicitly via -I (see sysrepo-mcp-server-cflags).
#
bins                   += $(PACKAGE)
$(PACKAGE)-objs        := $(srcdir)/main.o
$(PACKAGE)-src         := $(srctop)
$(PACKAGE)-cflags      := -I$(TOPDIR)/include \
                          -DPACKAGE_NAME='"$(PACKAGE)"' \
                          -DPACKAGE_VERSION='"$(VERSION)"' \
                          -DCONFIG_SYSREPO_MCP_SERVER_SYSLOG
$(PACKAGE)-ldflags     := -lconfig

include $(EBUILDDIR)/rules.mk
