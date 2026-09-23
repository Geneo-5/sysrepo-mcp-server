/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Runtime configuration: global accessor for main.c.  The actual parsing
 * happens in libconfig.c, which loads a libconfig file into a struct mcp_config
 * populated with the same defaults as the old Kconfig symbols.
 *
 * main.c calls mcp_config_load() at startup, then stores the result through
 * mcp_config_set().  Every other module reads the global through mcp_config_get().
 */

#include <stdlib.h>
#include <string.h>

#include <sysrepo/mcp/libconfig.h>

/* Global configuration: pointer, not inline struct, so that main.c
 * can store the address (and later, a shared-memory pointer) without
 * exposing the full definition. */

static struct mcp_config g_config;

/* ---------------------------------------------------------------- mcp_config_set
 */

void
mcp_config_set(const struct mcp_config *cfg)
{
	memcpy(&g_config, cfg, sizeof(g_config));
}

/* ---------------------------------------------------------------- mcp_config_get
 */

const struct mcp_config *
mcp_config_get(void)
{
	return &g_config;
}
