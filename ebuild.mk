################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp-server.
# Copyright (C) 2025 Grégor Boirie <gregor.boirie@free.fr>
################################################################################

srcdir := src
srctop := $(TOPDIR)/$(srcdir)

obj-y := $(srcdir)/main.o

include $(EBUILDDIR)/rules.mk
