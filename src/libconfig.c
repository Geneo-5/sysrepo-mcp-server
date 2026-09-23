/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Parses the libconfig file described in docker/sysrepo-mcp.conf into a
 * struct mcp_config. See include/sysrepo/mcp/libconfig.h for the field
 * list and its mapping back to the SYSREPO_MCP_SERVER_* names that used to
 * be either a Kconfig option or a leaf in yang/sysrepo-mcp.yang.
 *
 * mcp_config_set_defaults() carries the exact same defaults as the Kconfig
 * symbols in config.in, so a file that sets nothing still yields a working,
 * secure-by-default configuration (no API keys means nobody can
 * authenticate, which is the safe failure mode).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libconfig.h>

#include <sysrepo/mcp/libconfig.h>

/* ---------------------------------------------------------------------- ranges
 *
 * Mirrors the `range' lines in config.in. Kept as macros so the bound and
 * the option it applies to stay next to each other at the call site.
 */

#define MCP_RANGE_MAX_SESSIONS_MIN       1
#define MCP_RANGE_MAX_SESSIONS_MAX       1024
#define MCP_RANGE_SESSION_TTL_MIN        60
#define MCP_RANGE_SESSION_TTL_MAX        86400
#define MCP_RANGE_NOTIF_QUEUE_SIZE_MIN   8
#define MCP_RANGE_NOTIF_QUEUE_SIZE_MAX   65536
#define MCP_RANGE_MAX_TREE_DEPTH_MIN     1
#define MCP_RANGE_MAX_TREE_DEPTH_MAX     256
#define MCP_RANGE_TCP_PORT_MIN           1
#define MCP_RANGE_TCP_PORT_MAX           65535
#define MCP_RANGE_LOG_LEVEL_MIN          0
#define MCP_RANGE_LOG_LEVEL_MAX          7

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
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: out of memory "
		        "duplicating \"%s\"\n", s);
	return out;
}

/* ------------------------------------------------------------------- copy_string
 *
 * Copies a config_setting_t string value into a fixed-size struct field,
 * truncating with a warning rather than overflowing it. Used for the small
 * fields (paths, host, cookie name) that were plain fixed-size strings in
 * the Kconfig options they replace.
 */

static void
copy_string(char *dst, size_t dst_size, const char *src)
{
	size_t len;

	len = strlen(src);
	if (len >= dst_size) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: value \"%s\" is "
		        "too long (max %zu characters), truncating\n",
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
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: %s = %lld is out "
		        "of range [%lld, %lld]\n", name, value, min, max);
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------- load_schema
 */

static int
load_schema(config_t *cc, struct mcp_config *cfg)
{
	long long v;

	if (config_lookup_int(cc, "server.session.max_tree_depth", (int *)&v)) {
		if (check_range_uint("server.session.max_tree_depth", v,
		                     MCP_RANGE_MAX_SESSIONS_MIN,
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

	cfg->transport_unix = 1;
	cfg->transport_tcp  = 0;
	copy_string(cfg->unix_socket_path, sizeof(cfg->unix_socket_path),
	            "/var/run/sysrepo-mcp.sock");
	copy_string(cfg->tcp_host, sizeof(cfg->tcp_host), "127.0.0.1");
	cfg->tcp_port = 8080;

	cfg->auth_bearer = 1;
	cfg->auth_cookie = 0;
	copy_string(cfg->cookie_name, sizeof(cfg->cookie_name), "mcp_session");
	cfg->api_keys      = NULL;
	cfg->api_key_count = 0;

	cfg->acl_enabled                  = 1;
	cfg->acl_enable_nacm              = 1;
	cfg->acl_enable_module_filter     = 1;
	cfg->acl_enable_operation_filter  = 0;
	cfg->acl_enable_write_protection  = 0;
	cfg->acl_allowed_modules          = NULL; /* empty = all modules */

	cfg->syslog_enabled = 1;
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
	long long v;

	if (config_lookup_int(cc, "server.session.max_sessions", (int *)&v)) {
		if (check_range_uint("server.session.max_sessions", v,
		                     MCP_RANGE_MAX_SESSIONS_MIN,
		                     MCP_RANGE_MAX_SESSIONS_MAX))
			return -1;
		cfg->max_sessions = (unsigned)v;
	}
	if (config_lookup_int(cc, "server.session.ttl", (int *)&v)) {
		if (check_range_uint("server.session.ttl", v,
		                     MCP_RANGE_SESSION_TTL_MIN,
		                     MCP_RANGE_SESSION_TTL_MAX))
			return -1;
		cfg->session_ttl = (unsigned)v;
	}
	if (config_lookup_int(cc, "server.session.notif_queue_size", (int *)&v)) {
		if (check_range_uint("server.session.notif_queue_size", v,
		                     MCP_RANGE_NOTIF_QUEUE_SIZE_MIN,
		                     MCP_RANGE_NOTIF_QUEUE_SIZE_MAX))
			return -1;
		cfg->notif_queue_size = (unsigned)v;
	}
	if (config_lookup_int(cc, "server.session.default_timeout_ms", (int *)&v)) {
		if (check_range_uint("server.session.default_timeout_ms", v, 100,
		                     60000))
			return -1;
		cfg->default_timeout_ms = (unsigned)v;
	}
	return 0;
}

/* ----------------------------------------------------------------- load_transport
 */

static int
load_transport(config_t *cc, struct mcp_config *cfg)
{
	const char *mode;
	const char *str;
	int         port;

	if (config_lookup_string(cc, "server.transport.mode", &mode)) {
		if (!strcmp(mode, "unix")) {
			cfg->transport_unix = 1;
			cfg->transport_tcp  = 0;
		} else if (!strcmp(mode, "tcp")) {
			cfg->transport_unix = 0;
			cfg->transport_tcp  = 1;
		} else {
			fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: "
			        "server.transport.mode must be \"unix\" or "
			        "\"tcp\", got \"%s\"\n", mode);
			return -1;
		}
	}
	if (config_lookup_string(cc, "server.transport.unix_socket_path", &str))
		copy_string(cfg->unix_socket_path,
		            sizeof(cfg->unix_socket_path), str);
	if (config_lookup_string(cc, "server.transport.tcp_host", &str))
		copy_string(cfg->tcp_host, sizeof(cfg->tcp_host), str);
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
		if (!strcmp(method, "bearer")) {
			cfg->auth_bearer = 1;
			cfg->auth_cookie = 0;
		} else if (!strcmp(method, "cookie")) {
			cfg->auth_bearer = 0;
			cfg->auth_cookie = 1;
		} else {
			fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: "
			        "server.auth.method must be \"bearer\" or "
			        "\"cookie\", got \"%s\"\n", method);
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
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: out of memory "
		        "loading %d API key(s)\n", count);
		return -1;
	}

	for (i = 0; i < count; i++) {
		const char *key_str;
		const char *user_str;

		elem = config_setting_get_elem(keys, (unsigned)i);
		if (!config_setting_lookup_string(elem, "key", &key_str) ||
		    !config_setting_lookup_string(elem, "user", &user_str)) {
			fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: "
			        "server.auth.api_keys[%d] needs both \"key\" "
			        "and \"user\"\n", i);
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

/* ---------------------------------------------------------------------- load_acl
 */

static int
load_acl(config_t *cc, struct mcp_config *cfg)
{
	config_setting_t *modules;
	int                b;
	int                i;
	int                count;
	size_t             total_len;
	char              *joined;

	if (config_lookup_bool(cc, "server.acl.enabled", &b))
		cfg->acl_enabled = b;
	if (config_lookup_bool(cc, "server.acl.enable_nacm", &b))
		cfg->acl_enable_nacm = b;
	if (config_lookup_bool(cc, "server.acl.enable_module_filter", &b))
		cfg->acl_enable_module_filter = b;
	if (config_lookup_bool(cc, "server.acl.enable_operation_filter", &b))
		cfg->acl_enable_operation_filter = b;
	if (config_lookup_bool(cc, "server.acl.enable_write_protection", &b))
		cfg->acl_enable_write_protection = b;

	modules = config_lookup(cc, "server.acl.allowed_modules");
	if (modules == NULL)
		return 0; /* absent: keep the "all modules" default */

	count = config_setting_length(modules);
	if (count == 0)
		return 0;

	/* Join the array back into the single comma-separated string that
	 * SYSREPO_MCP_SERVER_ACL_ALLOWED_MODULES always was, so callers that
	 * still expect that format (module allow-list check) don't change. */
	total_len = 0;
	for (i = 0; i < count; i++)
		total_len += strlen(config_setting_get_string_elem(modules, i)) + 1;

	joined = malloc(total_len);
	if (joined == NULL) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: out of memory "
		        "joining server.acl.allowed_modules\n");
		return -1;
	}
	joined[0] = '\0';
	for (i = 0; i < count; i++) {
		if (i > 0)
			strcat(joined, ",");
		strcat(joined, config_setting_get_string_elem(modules, i));
	}

	free(cfg->acl_allowed_modules);
	cfg->acl_allowed_modules = joined;
	return 0;
}

/* ---------------------------------------------------------------------- load_log
 */

static int
load_log(config_t *cc, struct mcp_config *cfg)
{
	const char *str;
	int         b;
	int         level;

	if (config_lookup_bool(cc, "server.log.syslog_enabled", &b))
		cfg->syslog_enabled = b;
	if (config_lookup_int(cc, "server.log.level", &level)) {
		if (check_range_uint("server.log.level", level,
		                     MCP_RANGE_LOG_LEVEL_MIN,
		                     MCP_RANGE_LOG_LEVEL_MAX))
			return -1;
		cfg->log_level = level;
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
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: %s:%d: %s\n",
		        path, config_error_line(&cc), config_error_text(&cc));
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
		rc = load_acl(&cc, cfg);
	if (rc == 0)
		rc = load_log(&cc, cfg);

	config_destroy(&cc);

	if (rc == 0 && cfg->transport_unix && cfg->transport_tcp) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: %s: "
		        "server.transport.mode cannot be both unix and tcp\n",
		        path);
		return -1;
	}
	if (rc == 0 && cfg->auth_bearer && cfg->auth_cookie) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": libconfig: %s: "
		        "server.auth.method cannot be both bearer and cookie\n",
		        path);
		return -1;
	}

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
	free(cfg->acl_allowed_modules);
	cfg->acl_allowed_modules = NULL;
}

/* ---------------------------------------------------------- mcp_config_find_key
 */

const char *
mcp_config_find_key(const struct mcp_config *cfg, const char *key)
{
	size_t i;

	for (i = 0; i < cfg->api_key_count; i++) {
		if (!strcmp(cfg->api_keys[i].key, key))
			return cfg->api_keys[i].user;
	}
	return NULL;
}
