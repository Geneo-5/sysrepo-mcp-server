/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

/**
 * @file
 * @brief Build-time identity of the package.
 *
 * PACKAGE_NAME and PACKAGE_VERSION are normally injected by the build system
 * (see the -D flags in ebuild.mk, which derive them from VERSION in the
 * top-level Makefile). The fallbacks below only matter when a translation
 * unit is compiled outside that build, for instance by an editor's language
 * server; they must never hardcode a version number that the build could
 * contradict.
 */

#ifndef _SYSREPO_MCP_CONFIG_VERSION_H
#define _SYSREPO_MCP_CONFIG_VERSION_H

#include <config.h>

#ifndef PACKAGE_NAME
/** Package name, normally supplied by the build system. */
#define PACKAGE_NAME "sysrepo-mcp"
#endif /* !PACKAGE_NAME */

#ifndef PACKAGE_VERSION
/** Package version, normally supplied by the build system. */
#define PACKAGE_VERSION "0.0.0-unknown"
#endif /* !PACKAGE_VERSION */

#endif /* !_SYSREPO_MCP_CONFIG_VERSION_H */
