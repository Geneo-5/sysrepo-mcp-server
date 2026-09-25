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

static struct mcp_config g_config = {0, };

/* ---------------------------------------------------------------- mcp_config_set
 */

void
mcp_config_set(const struct mcp_config *cfg)
{
	size_t i;

	if (g_config.api_keys) {
		mcp_config_free(&g_config);
	}

	memcpy(&g_config, cfg, sizeof(g_config));

	/* Deep copy api_keys so that mcp_config_free(&cfg) in main() does not
	 * free the copy stored in g_config (use-after-free / double-free). */
	if (cfg->api_keys) {
		g_config.api_keys = calloc(cfg->api_key_count, sizeof(*g_config.api_keys));
		if (g_config.api_keys) {
			for (i = 0; i < cfg->api_key_count; i++) {
				g_config.api_keys[i].key   = strdup(cfg->api_keys[i].key);
				g_config.api_keys[i].user  = strdup(cfg->api_keys[i].user);
			}
			g_config.api_key_count = cfg->api_key_count;
		}
	}
}

/* ---------------------------------------------------------------- mcp_config_get
 */

const struct mcp_config *
mcp_config_get(void)
{
	return &g_config;
}
