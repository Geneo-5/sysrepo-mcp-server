#!/bin/bash -x
################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

export PROJECT_DIR="${PWD}"
rm -rf /dev/shm/*
sysrepo-plugind -V5

lighttpd -D -f ${PROJECT_DIR}/docker/lighttpd.conf