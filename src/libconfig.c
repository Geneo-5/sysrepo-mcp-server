/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Parses the libconfig file described in docker/sysrepo-mcp.conf into a
 * struct mcp_config. See include/sysrepo/mcp/libconfig.h for the field
 * list and its mapping back to settings that formerly lived in Kconfig or
 * yang/sysrepo-mcp.yang.
 *
 * mcp_config_set_defaults() provides built-in defaults for omitted settings.
 * Authentication defaults to "none" for compatibility: an instance with no
 * configured keys is unauthenticated and runs with the service account's
 * sysrepo permissions. Operators must enable authentication before exposure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <libconfig.h>

#include <sysrepo/mcp/libconfig.h>
#include <sysrepo/mcp/log.h>

/* ---------------------------------------------------------------------- ranges
 *
 * Runtime bounds for libconfig values. Kept as macros so the bound and
 * the setting it applies to stay next to each other at the call site.
 */

#define MCP_RANGE_MAX_SESSIONS_MIN       1
#define MCP_RANGE_MAX_SESSIONS_MAX       1024
#define MCP_RANGE_SESSION_TTL_MIN        60
#define MCP_RANGE_SESSION_TTL_MAX        86400
#define MCP_RANGE_NOTIF_QUEUE_SIZE_MIN   8
#define MCP_RANGE_NOTIF_QUEUE_SIZE_MAX   65536
#define MCP_RANGE_MAX_TREE_DEPTH_MIN     1
#define MCP_RANGE_MAX_TREE_DEPTH_MAX     256
#define MCP_RANGE_LOG_LEVEL_MIN          0
#define MCP_RANGE_LOG_LEVEL_MAX          7
#define MCP_RANGE_TCP_PORT_MIN           1
#define MCP_RANGE_TCP_PORT_MAX           65535

/* ------------------------------------------------------------------------ xstrdup
 *
 * strdup(), but never returns NULL for a non-NULL input without printing
 * why: an allocation failure here would otherwise surface much later as a
 * mysterious NULL deref deep in the auth or ACL code.
 */

static char *
xstrdup(const char *s)
{
	char *out;

	if (s == NULL)
		return NULL;

	out = strdup(s);
	if (out == NULL)
		mcp_log_err("libconfig: out of memory duplicating \"%s\"", s);
	return out;
}

/* ------------------------------------------------------------------- copy_string
 *
 * Copies a config_setting_t string value into a fixed-size struct field,
 * truncating with a warning rather than overflowing it. Used for the small
 * fields (paths and cookie names) stored in the runtime config struct.
 */

static void
copy_string(char *dst, size_t dst_size, const char *src)
{
	size_t len;

	len = strlen(src);
	if (len >= dst_size) {
		mcp_log_warn("libconfig: value \"%s\" is too long (max %zu characters), truncating",
		            src, dst_size - 1);
		len = dst_size - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* -------------------------------------------------------------- check_range_uint
 */

static int
check_range_uint(const char *name, long long value, long long min, long long max)
{
	if (value < min || value > max) {
		mcp_log_err("libconfig: %s = %lld is out of range [%lld, %lld]",
		           name, value, min, max);
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------- load_schema
 */

static int
load_schema(config_t *cc, struct mcp_config *cfg)
{
	int v;

	if (config_lookup_int(cc, "server.session.max_tree_depth", &v)) {
		if (check_range_uint("server.session.max_tree_depth", v,
		                     MCP_RANGE_MAX_TREE_DEPTH_MIN,
		                     MCP_RANGE_MAX_TREE_DEPTH_MAX))
			return -1;
		cfg->max_tree_depth = (unsigned)v;
	}
	return 0;
}

/* ---------------------------------------------------------------- mcp_config_set_defaults
 */

void
mcp_config_set_defaults(struct mcp_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	cfg->max_sessions      = 64;
	cfg->session_ttl       = 1800;
	cfg->notif_queue_size  = 256;
	cfg->default_timeout_ms = 5000;
	cfg->max_tree_depth    = 32;
	cfg->transport_mode    = MCP_TRANSPORT_PROXY;
	copy_string(cfg->unix_socket_path, sizeof(cfg->unix_socket_path),
	            "/run/sysrepo-mcp/mcp.sock");
	copy_string(cfg->tcp_host, sizeof(cfg->tcp_host), "127.0.0.1");
	cfg->tcp_port = 8080;

	/* auth_method: 0 = "none" (no authentication), 1 = "bearer",
	 * 2 = "cookie".  Default is "none" so that a file that sets
	 * nothing behaves like the old unauthenticated server. */
	cfg->auth_method  = 0;	/* NONE */
	copy_string(cfg->cookie_name, sizeof(cfg->cookie_name), "mcp_session");
	cfg->api_keys      = NULL;
	cfg->api_key_count = 0;

	cfg->syslog_enabled  = 1;
	cfg->log_level       = 6;
	cfg->log_verbose     = 0;
	cfg->log_console     = 1;
	copy_string(cfg->log_file, sizeof(cfg->log_file),
	            "/var/log/sysrepo-mcp.log");
}

/* ------------------------------------------------------------------- load_session
 */

static int
load_session(config_t *cc, struct mcp_config *cfg)
{
	int v;

	if (config_lookup_int(cc, "server.session.max_sessions", &v)) {
		if (check_range_uint("server.session.max_sessions", v,
		                     MCP_RANGE_MAX_SESSIONS_MIN,
		                     MCP_RANGE_MAX_SESSIONS_MAX))
			return -1;
		cfg->max_sessions = (unsigned)v;
	}
	if (config_lookup_int(cc, "server.session.ttl", &v)) {
		if (check_range_uint("server.session.ttl", v,
		                     MCP_RANGE_SESSION_TTL_MIN,
		                     MCP_RANGE_SESSION_TTL_MAX))
			return -1;
		cfg->session_ttl = (unsigned)v;
	}
	if (config_lookup_int(cc, "server.session.notif_queue_size", &v)) {
		if (check_range_uint("server.session.notif_queue_size", v,
		                     MCP_RANGE_NOTIF_QUEUE_SIZE_MIN,
		                     MCP_RANGE_NOTIF_QUEUE_SIZE_MAX))
			return -1;
		cfg->notif_queue_size = (unsigned)v;
	}
	if (config_lookup_int(cc, "server.session.default_timeout_ms", &v)) {
		if (check_range_uint("server.session.default_timeout_ms", v, 100,
		                     60000))
			return -1;
		cfg->default_timeout_ms = (unsigned)v;
	}
	return 0;
}

/* ---------------------------------------------------------------- load_transport
 */

static int
load_transport(config_t *cc, struct mcp_config *cfg)
{
	const char *mode;
	const char *str;
	struct in_addr address;
	int port;

	if (config_lookup_string(cc, "server.transport.mode", &mode)) {
		if (!strcmp(mode, "proxy"))
			cfg->transport_mode = MCP_TRANSPORT_PROXY;
		else if (!strcmp(mode, "unix"))
			cfg->transport_mode = MCP_TRANSPORT_UNIX;
		else if (!strcmp(mode, "tcp"))
			cfg->transport_mode = MCP_TRANSPORT_TCP;
		else {
			mcp_log_err("libconfig: server.transport.mode must be "
			            "\"proxy\", \"unix\" or \"tcp\", got \"%s\"",
			            mode);
			return -1;
		}
	}
	if (config_lookup_string(cc, "server.transport.unix_socket_path", &str)) {
		if (!*str || strlen(str) >= sizeof(cfg->unix_socket_path)) {
			mcp_log_err("libconfig: server.transport.unix_socket_path must "
			            "contain 1 to %zu bytes",
			            sizeof(cfg->unix_socket_path) - 1);
			return -1;
		}
		copy_string(cfg->unix_socket_path, sizeof(cfg->unix_socket_path), str);
	}
	if (config_lookup_string(cc, "server.transport.tcp_host", &str)) {
		if (inet_pton(AF_INET, str, &address) != 1) {
			mcp_log_err("libconfig: server.transport.tcp_host must be an "
			            "IPv4 address");
			return -1;
		}
		copy_string(cfg->tcp_host, sizeof(cfg->tcp_host), str);
	}
	if (config_lookup_int(cc, "server.transport.tcp_port", &port)) {
		if (check_range_uint("server.transport.tcp_port", port,
		                     MCP_RANGE_TCP_PORT_MIN,
		                     MCP_RANGE_TCP_PORT_MAX))
			return -1;
		cfg->tcp_port = port;
	}
	return 0;
}

/* ---------------------------------------------------------------------- load_auth
 */

static int
load_auth(config_t *cc, struct mcp_config *cfg)
{
	const char        *method;
	const char        *str;
	config_setting_t  *keys;
	config_setting_t  *elem;
	struct mcp_api_key *list;
	int                 i;
	int                 count;

	if (config_lookup_string(cc, "server.auth.method", &method)) {
		if (!strcmp(method, "none")) {
			cfg->auth_method = 0;
		} else if (!strcmp(method, "bearer")) {
			cfg->auth_method = 1;
		} else if (!strcmp(method, "cookie")) {
			cfg->auth_method = 2;
		} else {
			mcp_log_err("libconfig: server.auth.method must be \"none\", \"bearer\" or \"cookie\", got \"%s\"", method);
			return -1;
		}
	}
	if (config_lookup_string(cc, "server.auth.cookie_name", &str))
		copy_string(cfg->cookie_name, sizeof(cfg->cookie_name), str);

	keys = config_lookup(cc, "server.auth.api_keys");
	if (keys == NULL)
		return 0; /* no keys configured: nobody can authenticate */

	count = config_setting_length(keys);
	if (count == 0)
		return 0;

	list = calloc((size_t)count, sizeof(*list));
	if (list == NULL) {
		mcp_log_err("libconfig: out of memory loading %d API key(s)", count);
		return -1;
	}

	for (i = 0; i < count; i++) {
		const char *key_str;
		const char *user_str;

		elem = config_setting_get_elem(keys, (unsigned)i);
		if (!config_setting_lookup_string(elem, "key", &key_str) ||
		    !config_setting_lookup_string(elem, "user", &user_str)) {
			mcp_log_err("libconfig: server.auth.api_keys[%d] needs both "
			            "\"key\" and \"user\"", i);
			for (i--; i >= 0; i--) {
				free(list[i].key);
				free(list[i].user);
			}
			free(list);
			return -1;
		}
		list[i].key  = xstrdup(key_str);
		list[i].user = xstrdup(user_str);
		if (list[i].key == NULL || list[i].user == NULL) {
			for (; i >= 0; i--) {
				free(list[i].key);
				free(list[i].user);
			}
			free(list);
			return -1;
		}
	}

	cfg->api_keys      = list;
	cfg->api_key_count = (size_t)count;
	return 0;
}

/* ---------------------------------------------------------------------- load_log
 */

static int
load_log(config_t *cc, struct mcp_config *cfg)
{
	const char *str;
	int         v;
	int         b;

	if (config_lookup_bool(cc, "server.log.syslog_enabled", &b))
		cfg->syslog_enabled = b;
	if (config_lookup_int(cc, "server.log.level", &v)) {
		if (check_range_uint("server.log.level", v,
		                     MCP_RANGE_LOG_LEVEL_MIN,
		                     MCP_RANGE_LOG_LEVEL_MAX))
			return -1;
		cfg->log_level = v;
	}
	if (config_lookup_bool(cc, "server.log.verbose", &b))
		cfg->log_verbose = b;
	if (config_lookup_bool(cc, "server.log.console", &b))
		cfg->log_console = b;
	if (config_lookup_string(cc, "server.log.file", &str))
		copy_string(cfg->log_file, sizeof(cfg->log_file), str);
	return 0;
}

/* ---------------------------------------------------------------- mcp_config_load
 */

int
mcp_config_load(const char *path, struct mcp_config *cfg)
{
	config_t cc;
	int      rc;

	config_init(&cc);

	if (!config_read_file(&cc, path)) {
		mcp_log_err("libconfig: %s:%d: %s", path, config_error_line(&cc),
		           config_error_text(&cc));
		config_destroy(&cc);
		return -1;
	}

	rc = load_session(&cc, cfg);
	if (rc == 0)
		rc = load_schema(&cc, cfg);
	if (rc == 0)
		rc = load_transport(&cc, cfg);
	if (rc == 0)
		rc = load_auth(&cc, cfg);
	if (rc == 0)
		rc = load_log(&cc, cfg);

	config_destroy(&cc);

	return rc;
}

/* --------------------------------------------------------------- mcp_config_free
 */

void
mcp_config_free(struct mcp_config *cfg)
{
	size_t i;

	if (cfg->api_keys != NULL) {
		for (i = 0; i < cfg->api_key_count; i++) {
			free(cfg->api_keys[i].key);
			free(cfg->api_keys[i].user);
		}
		free(cfg->api_keys);
		cfg->api_keys      = NULL;
		cfg->api_key_count = 0;
	}
}

/* ---------------------------------------------------------- mcp_config_find_key
 */

const char *
mcp_config_find_key(const struct mcp_config *cfg, const char *key)
{
	size_t i, j;

	for (i = 0; i < cfg->api_key_count; i++) {
		const char *stored = cfg->api_keys[i].key;
		const char *given = key;
		size_t stored_len = strlen(stored);
		size_t given_len = strlen(given);
		int different = (int)(stored_len ^ given_len);

		/* Compare every byte regardless of length difference */
		for (j = 0; j < (stored_len > given_len ? stored_len : given_len);
		     j++) {
			if (j < stored_len && j < given_len)
				different |= (unsigned char)stored[j]
				           ^ (unsigned char)given[j];
		}
		if (different == 0)
			return cfg->api_keys[i].user;
	}
	return NULL;
}
