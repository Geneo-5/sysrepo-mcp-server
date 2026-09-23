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
 * mcp_config_set_defaults(): the file only needs to set what it wants to
 * override, and every setting has the same default as the Kconfig symbol it
 * replaces.
 */
struct mcp_config {
	/* server.session */
	unsigned max_sessions;		/* SYSREPO_MCP_SERVER_MAX_SESSIONS */
	unsigned session_ttl;		/* SYSREPO_MCP_SERVER_SESSION_TTL */
	unsigned notif_queue_size;	/* SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE */
	unsigned default_timeout_ms;	/* runtime default timeout for sysrepo ops */

	/* schema introspection */
	unsigned max_tree_depth;	/* SYSREPO_MCP_SERVER_MAX_TREE_DEPTH */

	/* server.transport -- mutually exclusive, like the Kconfig choice
	 * it replaces. "mode" in the file is "unix" or "tcp". */
	int  transport_unix;		/* SYSREPO_MCP_SERVER_TRANSPORT_UNIX */
	int  transport_tcp;		/* SYSREPO_MCP_SERVER_TRANSPORT_TCP */
	char unix_socket_path[256];	/* SYSREPO_MCP_SERVER_UNIX_SOCKET_PATH */
	char tcp_host[256];		/* SYSREPO_MCP_SERVER_TCP_HOST */
	int  tcp_port;			/* SYSREPO_MCP_SERVER_TCP_PORT */

	/* server.auth -- "method" in the file is "bearer" or "cookie". */
	int  auth_bearer;		/* SYSREPO_MCP_SERVER_AUTH_BEARER */
	int  auth_cookie;		/* SYSREPO_MCP_SERVER_AUTH_COOKIE */
	char cookie_name[128];		/* SYSREPO_MCP_SERVER_COOKIE_NAME */

	struct mcp_api_key *api_keys;	/* owned; see mcp_config_free() */
	size_t               api_key_count;

	/* server.acl */
	int   acl_enabled;			/* SYSREPO_MCP_SERVER_ACL_ENABLED */
	int   acl_enable_nacm;			/* SYSREPO_MCP_SERVER_ACL_ENABLE_NACM */
	int   acl_enable_module_filter;	/* SYSREPO_MCP_SERVER_ACL_ENABLE_MODULE_FILTER */
	int   acl_enable_operation_filter;	/* SYSREPO_MCP_SERVER_ACL_ENABLE_OPERATION_FILTER */
	int   acl_enable_write_protection;	/* SYSREPO_MCP_SERVER_ACL_ENABLE_WRITE_PROTECTION */
	char *acl_allowed_modules;		/* SYSREPO_MCP_SERVER_ACL_ALLOWED_MODULES:
						 * owned, comma-separated, NULL/empty = all */

	/* server.log */
	int  syslog_enabled;		/* SYSREPO_MCP_SERVER_SYSLOG_ENABLED */
	int  log_level;			/* SYSREPO_MCP_SERVER_LOG_LEVEL */
	int  log_verbose;		/* SYSREPO_MCP_SERVER_LOG_VERBOSE */
	int  log_console;		/* SYSREPO_MCP_SERVER_LOG_CONSOLE */
	char log_file[256];		/* SYSREPO_MCP_SERVER_LOG_FILE */
};

/** Fill @cfg with the same defaults as the config.in Kconfig symbols it
 * replaces. Call this before mcp_config_load(), so a file that omits a
 * setting leaves the documented default in place. */
void mcp_config_set_defaults(struct mcp_config *cfg);

/**
 * Parse @path (libconfig syntax, see docker/sysrepo-mcp.conf) into @cfg.
 *
 * @cfg must already have been initialised by mcp_config_set_defaults():
 * only the settings present in the file are overridden. Ranges are checked
 * against the same bounds as the Kconfig symbols they replace.
 *
 * Returns 0 on success. Returns -1 on a parse or validation error, after
 * printing a diagnostic to stderr; @cfg is left in a partially-updated but
 * still safe-to-use state (defaults for anything not yet applied).
 */
int mcp_config_load(const char *path, struct mcp_config *cfg);

/** Release the memory owned by @cfg (the api_keys array and its strings,
 * acl_allowed_modules). Safe to call on a zeroed or already-freed struct;
 * does not free @cfg itself. */
void mcp_config_free(struct mcp_config *cfg);

/** Look up @key in @cfg's API key list. Returns the NACM user it maps to,
 * or NULL if @key is not configured. */
const char *mcp_config_find_key(const struct mcp_config *cfg,
				const char *key);

/** Install @cfg as the runtime configuration.  Called once at startup
 * after mcp_config_load().  Subsequent calls overwrite the previous value.
 * The caller retains ownership of @cfg. */
void mcp_config_set(const struct mcp_config *cfg);

/** Return the current runtime configuration.  The returned pointer is valid
 * until the next mcp_config_set() or program exit. */
const struct mcp_config *mcp_config_get(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_LIBCONFIG_H */
