################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp-server.
# Copyright (C) 2025 Grégor Boirie <gregor.boirie@free.fr>
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

include $(EBUILDDIR)/main.mk
