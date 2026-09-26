/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Entry point: sysrepo connection, FastCGI loop, signal handling, tools[]
 * catalogue.  All tool implementations, HTTP/RPC plumbing, MCP methods, and
 * request dispatch have been split into separate source files.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <json-c/json.h>

#include <sysrepo.h>
#include <sysrepo/netconf_acm.h>
#include <fcgiapp.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/sessions.h>
#include <sysrepo/mcp/transport.h>
#include <sysrepo/mcp/config_tools.h>
#include <sysrepo/mcp/libconfig.h>
#include <sysrepo/mcp/operational.h>
#include <sysrepo/mcp/rpc.h>
#include <sysrepo/mcp/modules.h>
#include <sysrepo/mcp/notifications.h>
#include <sysrepo/mcp/schema.h>
#include <sysrepo/mcp/status.h>
#include <sysrepo/mcp/log.h>

/* --------------------------------------------------------------------- globals
 *
 * These variables are declared extern in sessions.c as well; main.c owns
 * the storage. External modules only read g_sessions via the declarations in
 * sessions.h.
 */

sr_conn_ctx_t         *g_conn;
static sr_subscription_ctx_t *g_nacm_sub;
time_t                 g_start_time;
volatile sig_atomic_t stopping;

/* ------------------------------------------------------------------ Tool catalogue
 *
 * Description, input schema, handler, and session requirement for each tool.
 * See the individual source files for implementation details.
 */

const struct tool_desc tools[] = {
	/* Datastore tools */
	{
		"sr_get_config",
		"Read an entire datastore subtree.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\"},"
		"\"options\":{\"type\":\"array\",\"items\":{"
		"\"type\":\"string\",\"enum\":[\"trim-defaults\","
		"\"all-defaults\"]}}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_get_config, 1
	},
	{
		"sr_edit_config",
		"Apply a configuration change and report the number of edit nodes.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\"},"
		"\"strict\":{\"type\":\"boolean\",\"default\":true}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_edit_config, 1
	},
	{
		"sr_delete_config",
		"Delete a configuration subtree.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\"},"
		"\"strict\":{\"type\":\"boolean\",\"default\":false}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_delete_config, 1
	},
	{
		"sr_copy_config",
		"Replace one datastore with the contents of another.",
		"{\"type\":\"object\",\"properties\":{"
		"\"source\":{\"type\":\"string\",\"enum\":["
		"\"running\",\"startup\",\"candidate\"]},"
		"\"destination\":{\"type\":\"string\",\"enum\":["
		"\"running\",\"startup\",\"candidate\"]}},"
		"\"required\":[\"source\",\"destination\"]}",
		tool_sr_copy_config, 1
	},
	{
		"sr_get_operational",
		"Read operational state data.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"max_depth\":{\"type\":\"integer\",\"minimum\":0,"
		"\"default\":0},"
		"\"timeout_ms\":{\"type\":\"integer\",\"default\":5000}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_get_operational, 1
	},
	/* RPC and actions */
	{
		"sr_execute_rpc",
		"Invoke a YANG RPC.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"input\":{\"type\":\"object\"},"
		"\"timeout_ms\":{\"type\":\"integer\",\"default\":5000}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_execute_rpc, 1
	},
	{
		"sr_action",
		"Invoke a YANG action on a data node instance.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"input\":{\"type\":\"object\"},"
		"\"timeout_ms\":{\"type\":\"integer\",\"default\":5000}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_action, 1
	},
	/* Notifications */
	{
		"sr_notif_subscribe",
		"Watch a module's YANG notifications on the current session.",
		"{\"type\":\"object\",\"properties\":{"
		"\"module\":{\"type\":\"string\"},"
		"\"xpath\":{\"type\":\"string\"},"
		"\"replay_start\":{\"type\":\"integer\",\"minimum\":0}},"
		"\"required\":[\"module\"]}",
		tool_sr_notif_subscribe, 0
	},
	{
		"sr_notif_unsubscribe",
		"Stop watching notifications; all subscriptions when no id is given.",
		"{\"type\":\"object\",\"properties\":{"
		"\"subscription_id\":{\"type\":\"integer\",\"minimum\":1}}}",
		tool_sr_notif_unsubscribe, 0
	},
	{
		"sr_notif_list_subscriptions",
		"List the notification subscriptions of the current session.",
		"{\"type\":\"object\",\"properties\":{}}",
		tool_sr_notif_list_subscriptions, 0
	},
	{
		"sr_notif_poll",
		"Return the notifications received on the current session since the last poll.",
		"{\"type\":\"object\",\"properties\":{"
		"\"max\":{\"type\":\"integer\",\"minimum\":0,\"default\":0},"
		"\"peek\":{\"type\":\"boolean\",\"default\":false}}}",
		tool_sr_notif_poll, 0
	},
	{
		"sr_notif_send",
		"Send a YANG notification.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"input\":{\"type\":\"object\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_notif_send, 1
	},
	/* Module management */
	{
		"sr_list_modules",
		"List the YANG modules known to the datastore.",
		"{\"type\":\"object\",\"properties\":{"
		"\"implemented_only\":{\"type\":\"boolean\",\"default\":true}}}",
		tool_sr_list_modules, 1
	},
	{
		"sr_module_install",
		"Install a YANG module into the datastore.",
		"{\"type\":\"object\",\"properties\":{"
		"\"yang_file\":{\"type\":\"string\"},"
		"\"search_dirs\":{\"type\":\"string\"},"
		"\"features\":{\"type\":\"array\","
		"\"items\":{\"type\":\"string\"}}},"
		"\"required\":[\"yang_file\"]}",
		tool_sr_module_install, 0
	},
	{
		"sr_module_uninstall",
		"Remove a YANG module and its data from the datastore.",
		"{\"type\":\"object\",\"properties\":{"
		"\"module\":{\"type\":\"string\"},"
		"\"force\":{\"type\":\"boolean\",\"default\":false}},"
		"\"required\":[\"module\"]}",
		tool_sr_module_uninstall, 0
	},
	/* Schema introspection */
	{
		"get_schema",
		"Explore compiled YANG schema, optionally from one XPath.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"max_depth\":{\"type\":\"integer\",\"minimum\":0,"
		"\"default\":0}}}",
		tool_get_schema, 1
	},
	/* Status */
	{
		"get_status",
		"Return server health: version, uptime, session counters.",
		"{\"type\":\"object\",\"properties\":{"
		"\"verbose\":{\"type\":\"boolean\",\"default\":false}}}",
		tool_get_status, 0
	},
};

const size_t TOOL_COUNT = sizeof(tools) / sizeof(tools[0]);

/* ------------------------------------------------------------------- on_signal
 *
 * Set the stopping flag and request FastCGI shutdown. Called by signal handler.
 */

static void
on_signal(int signum)
{
	(void)signum;
	stopping = 1;
	FCGX_ShutdownPending();
}

static int
open_tcp_listener(const char *host, int port, int backlog)
{
	struct sockaddr_in address;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
	    listen(fd, backlog)) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	return fd;
}

static void
usage(FILE *out)
{
	fprintf(out,
		CONFIG_PACKAGE_NAME " " CONFIG_PACKAGE_VERSION
		" - MCP server for the sysrepo datastore\n"
		"\n"
		"Usage: " CONFIG_PACKAGE_NAME " [ -f <file> ] [ --config <file> ]\n"
		"             [ -l <severity> ] [ --log-level <severity> ]\n"
		"             [ --help ] [ --version ]\n"
		"\n"
		"  -f, --config    path to the libconfig file\n"
		"                  (default: /etc/sysrepo-mcp/" CONFIG_PACKAGE_NAME
		".conf)\n"
		"  -l, --log-level severity override (emerg..debug)\n"
		"  --help      print this message and exit\n"
		"  --version   print the version and exit\n"
		"\n"
		"server.transport.mode selects proxy (default), unix, or tcp. In\n"
		"proxy mode a web server starts this FastCGI responder; unix and tcp\n"
		"modes create a listening socket directly. Sessions are process-local,\n"
		"so run one process per datastore.\n");
}

/* ---------------------------------------------------------------- sysrepo_open/close
 *
 * Wrappers around the sysrepo 5.x API (sr_connect/sr_disconnect) to preserve
 * the original 3.x interface.  Called once at startup and shutdown.
 */

static int
sysrepo_open(void)
{
	int rc;
	sr_session_ctx_t *sess;

	const char *repository_path = getenv("SYSREPO_REPOSITORY_PATH");

	if ((!repository_path || !*repository_path) &&
	    setenv("SYSREPO_REPOSITORY_PATH",
	           CONFIG_SYSREPO_MCP_SERVER_SYSREPO_DATASTORE_DIR, 1)) {
		mcp_log_err("cannot set SYSREPO_REPOSITORY_PATH: %s",
		            strerror(errno));
		return -1;
	}

	if ((rc = sr_connect(0, &g_conn)) != SR_ERR_OK) {
		mcp_log_err("sr_connect: %s", sr_strerror(rc));
		return rc;
	}
	/* NACM : initialiser le contrôle d'accès une fois pour toutes les sessions. */

	g_nacm_sub = NULL;

	rc = sr_session_start(g_conn, SR_DS_RUNNING, &sess);
	if (rc == SR_ERR_OK)
		rc = sr_nacm_init(sess, 0, &g_nacm_sub);
	if (rc != SR_ERR_OK) {
		if (sess)
			sr_session_stop(sess);
		mcp_log_warn("sr_nacm_init: %s (NACM disabled)", sr_strerror(rc));
	}

	return SR_ERR_OK;
}

static void
sysrepo_close(void)
{
	if (g_conn) {
		sr_nacm_destroy();
		sr_disconnect(g_conn);
		g_conn = NULL;
	}
}

int
main(int argc, char *argv[])
{
	FCGX_Request     req;
	struct sigaction sa;
	const struct mcp_config *runtime_cfg;
	const char       *config_path = "/etc/sysrepo-mcp/sysrepo-mcp.conf";
	char             listener[160];
	int              listener_fd = 0;
	int              standalone;
	int              opt;
	static struct option long_options[] = {
		{"config", required_argument, NULL, 'f'},
		{"log-level", required_argument, NULL, 'l'},
		{"help",   no_argument,       NULL, 'h'},
		{"version",no_argument,       NULL, 'V'},
		{NULL,     0,                 NULL,  0  }
	};

	const char       *log_level_arg = NULL;
	while ((opt = getopt_long(argc, argv, "f:l:hV", long_options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			config_path = optarg;
			break;
		case 'l':
			log_level_arg = optarg;
			break;
		case 'h':
			usage(stdout);
			return EXIT_SUCCESS;
		case 'V':
			printf(CONFIG_PACKAGE_NAME " " CONFIG_PACKAGE_VERSION "\n");
			return EXIT_SUCCESS;
		default:
			usage(stderr);
			return EXIT_FAILURE;
		}
	}


	struct mcp_config cfg, bootstrap;
	mcp_config_set_defaults(&cfg);
	bootstrap = cfg;
	bootstrap.log_file[0] = '\0';
	if (mcp_log_init(&bootstrap) < 0)
		return EXIT_FAILURE;

	if (mcp_config_load(config_path, &cfg) < 0)
		mcp_log_warn("could not load config file %s; using available defaults",
		             config_path);
	if (cfg.log_verbose && !log_level_arg)
		cfg.log_level = ELOG_DEBUG_SEVERITY;
	if (log_level_arg) {
		struct elog_syslog_conf defaults = {
			.super.severity = cfg.log_level,
			.format = ELOG_PID_FMT,
			.facility = LOG_DAEMON,
		};
		struct elog_syslog_conf parsed = defaults;
		struct elog_parse parse;
		int parse_rc;

		elog_init_syslog_parse(&parse, &parsed, &defaults);
		parse_rc = elog_parse_syslog_severity(&parse, &parsed,
		                                     log_level_arg);
		if (!parse_rc)
			parse_rc = elog_realize_parse(&parse, &parsed.super);
		if (parse_rc) {
			mcp_log_err("invalid --log-level '%s': %s", log_level_arg,
			            parse.error ? parse.error : "invalid severity");
			elog_fini_parse(&parse);
			mcp_config_free(&cfg);
			mcp_log_close();
			return EXIT_FAILURE;
		}
		cfg.log_level = parsed.super.severity;
		elog_fini_parse(&parse);
	}
	mcp_config_set(&cfg);
	if (mcp_log_init(mcp_config_get()) < 0) {
		mcp_config_free(&cfg);
		return EXIT_FAILURE;
	}
	mcp_config_free(&cfg);
	runtime_cfg = mcp_config_get();
	standalone = runtime_cfg->transport_mode != MCP_TRANSPORT_PROXY;

	if (FCGX_Init()) {
		mcp_log_err("FCGX_Init failed");
		mcp_log_close();
		return EXIT_FAILURE;
	}

	/*
	 * FCGX_IsCGI() is the only correct test. Probing getenv() cannot work:
	 * under FastCGI the request parameters arrive per request in
	 * req.envp, never in the process environment.
	 */
	if (!standalone && FCGX_IsCGI()) {
		mcp_log_err("not started as a FastCGI application; run behind a "
		            "reverse proxy, or configure a unix/tcp listener");
		mcp_log_close();
		return EXIT_FAILURE;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) {
		mcp_log_err("sigaction: %s", strerror(errno));
		mcp_log_close();
		return EXIT_FAILURE;
	}
	signal(SIGPIPE, SIG_IGN);

	if (sysrepo_open() != SR_ERR_OK) {
		mcp_log_close();
		return EXIT_FAILURE;
	}

	g_start_time = time(NULL);

	if (sessions_init(mcp_config_get()->max_sessions) < 0) {
		sysrepo_close();
		mcp_log_close();
		return EXIT_FAILURE;
	}

	if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX) {
		struct stat st;

		if (!lstat(runtime_cfg->unix_socket_path, &st)) {
			if (!S_ISSOCK(st.st_mode) || unlink(runtime_cfg->unix_socket_path)) {
				mcp_log_err("refusing to replace non-socket path or remove "
				            "existing socket %s: %s",
				            runtime_cfg->unix_socket_path, strerror(errno));
				sessions_free();
				sysrepo_close();
				mcp_log_close();
				return EXIT_FAILURE;
			}
		} else if (errno != ENOENT) {
			mcp_log_err("cannot inspect socket path %s: %s",
			            runtime_cfg->unix_socket_path, strerror(errno));
			sessions_free();
			sysrepo_close();
			mcp_log_close();
			return EXIT_FAILURE;
		}
		snprintf(listener, sizeof(listener), "%s", runtime_cfg->unix_socket_path);
	}
	if (standalone) {
		if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX)
			listener_fd = FCGX_OpenSocket(listener, 128);
		else
			listener_fd = open_tcp_listener(runtime_cfg->tcp_host,
			                                runtime_cfg->tcp_port, 128);
		if (listener_fd < 0) {
			if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX)
				mcp_log_err("cannot open FastCGI listener %s: %s", listener,
				            strerror(errno));
			else
				mcp_log_err("cannot open FastCGI listener %s:%d: %s",
				            runtime_cfg->tcp_host, runtime_cfg->tcp_port,
				            strerror(errno));
			if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX)
				unlink(runtime_cfg->unix_socket_path);
			sessions_free();
			sysrepo_close();
			mcp_log_close();
			return EXIT_FAILURE;
		}
		if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX &&
		    chmod(runtime_cfg->unix_socket_path, 0660)) {
			mcp_log_err("cannot set permissions on %s: %s",
			            runtime_cfg->unix_socket_path, strerror(errno));
			close(listener_fd);
			unlink(runtime_cfg->unix_socket_path);
			sessions_free();
			sysrepo_close();
			mcp_log_close();
			return EXIT_FAILURE;
		}
	}

	if (FCGX_InitRequest(&req, listener_fd, 0)) {
		mcp_log_err("FCGX_InitRequest failed");
		if (standalone) {
			close(listener_fd);
			if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX)
				unlink(runtime_cfg->unix_socket_path);
		}
		sysrepo_close();
		mcp_log_close();
		return EXIT_FAILURE;
	}

	mcp_log_info("%s ready", CONFIG_PACKAGE_VERSION);

	while (!stopping && FCGX_Accept_r(&req) >= 0) {
		serve(&req);
		FCGX_Finish_r(&req);
	}

	sessions_free();

	FCGX_Free(&req, 1);
	if (standalone) {
		close(listener_fd);
		if (runtime_cfg->transport_mode == MCP_TRANSPORT_UNIX)
			unlink(runtime_cfg->unix_socket_path);
	}
	sysrepo_close();

	mcp_log_info("stopped");
	mcp_log_close();

	return EXIT_SUCCESS;
}
