/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

/**
 * @file
 * @brief Runtime configuration, read from a libconfig file at startup.
 *
 * yang/sysrepo-mcp.yang is gone: nothing here is installed into sysrepo, and
 * nothing here changes what the datastore offers. Everything that used to
 * live in that YANG module, or that was a Kconfig option but is really an
 * operator-tunable setting, is read once at startup from the file described
 * in docker/sysrepo-mcp.conf. What stays build-time Kconfig (paths, feature
 * toggles compiled in) is unaffected and still comes from config.in /
 * autoconf.h.
 *
 * See sphinx/todo.rst, "P1 -- Configuration: drop the YANG module, adopt
 * libconfig" for the option-by-option split.
 */

#ifndef _SYSREPO_MCP_LIBCONFIG_H
#define _SYSREPO_MCP_LIBCONFIG_H

#include "config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ------------------------------------------------------------------ mcp_api_key
 */

/** One entry of the API key list: a credential mapped to the NACM user it
 * authenticates as. */
struct mcp_api_key {
	char *key;	/* bearer token or cookie value, as sent by the client */
	char *user;	/* NACM user name to run the session as */
};

/* -------------------------------------------------------------------- mcp_config
 */

/**
 * Runtime configuration, parsed once at startup by mcp_config_load().
 *
 * A struct passed to mcp_config_load() must first have been initialised by
 * mcp_config_set_defaults(): the file only needs to set the values it wants
 * to override.
 */
struct mcp_config {
	/* server.session */
	unsigned max_sessions;		/* server.session.max_sessions */
	unsigned session_ttl;		/* server.session.ttl */
	unsigned notif_queue_size;	/* server.session.notif_queue_size */
	unsigned default_timeout_ms;	/* runtime default timeout for sysrepo ops */

	/* schema introspection */
	unsigned max_tree_depth;	/* server.session.max_tree_depth */

	/* server.auth: "none", "bearer" or "cookie".  When "none" (the
	 * default) no authentication is performed.  NACM is implicit
	 * whenever authentication is on (the old server.acl section is
	 * gone). */
	int  auth_method;		/* server.auth.method */
	char cookie_name[128];		/* server.auth.cookie_name */

	struct mcp_api_key *api_keys;	/* owned; see mcp_config_free() */
	size_t               api_key_count;


	/* server.log */
	int  syslog_enabled;		/* server.log.syslog_enabled */
	int  log_level;			/* server.log.level */
	int  log_verbose;		/* server.log.verbose */
	int  log_console;		/* server.log.console */
	char log_file[256];		/* server.log.file */
};

/** Fill @p cfg with built-in defaults. Call this before mcp_config_load(), so
 * a file that omits a setting leaves the documented default in place. */
void mcp_config_set_defaults(struct mcp_config *cfg);

/**
 * Parse @p path (libconfig syntax, see docker/sysrepo-mcp.conf) into @p cfg.
 *
 * @p cfg must already have been initialised by mcp_config_set_defaults():
 * only the settings present in the file are overridden. Runtime ranges are
 * checked before values are stored.
 *
 * Returns 0 on success. Returns -1 on a parse or validation error, after
 * logging a diagnostic; @p cfg is left in a partially-updated but
 * still safe-to-use state (defaults for anything not yet applied).
 */
int mcp_config_load(const char *path, struct mcp_config *cfg);

/** Release the memory owned by @p cfg (the API key array and its strings).
 * Safe to call on a zeroed or already-freed struct; does not free @p cfg. */
void mcp_config_free(struct mcp_config *cfg);

/** Look up @p key in @p cfg's API key list. Returns the NACM user it maps to,
 * or NULL if @p key is not configured. */
const char *mcp_config_find_key(const struct mcp_config *cfg,
				const char *key);

/** Install @p cfg as the runtime configuration.  Called once at startup
 * after mcp_config_load().  Subsequent calls overwrite the previous value.
 * The caller retains ownership of @p cfg. */
void mcp_config_set(const struct mcp_config *cfg);

/** Return the current runtime configuration.  The returned pointer is valid
 * until the next mcp_config_set() or program exit. */
const struct mcp_config *mcp_config_get(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_LIBCONFIG_H */
