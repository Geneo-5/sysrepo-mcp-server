#!/bin/bash -x
################################################################################
# SPDX-License-Identifier: LGPL-3.0-only
#
# This file is part of sysrepo-mcp.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

export PROJECT_DIR="${PWD}"
rm -rf /dev/shm/*

# Generate a self-signed certificate in /tmp for test/demo purposes.
openssl req -x509 -newkey rsa:2048 \
  -keyout /tmp/sysrepo-mcp.key \
  -out  /tmp/sysrepo-mcp.crt \
  -days 365 -nodes \
  -subj "/CN=localhost" 2>/dev/null
cat /tmp/sysrepo-mcp.key /tmp/sysrepo-mcp.crt > /tmp/sysrepo-mcp.pem

sysrepo-plugind -V5

lighttpd -D -f ${PROJECT_DIR}/docker/lighttpd.conf