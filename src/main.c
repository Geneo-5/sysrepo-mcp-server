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
#include <getopt.h>

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

/* --------------------------------------------------------------------- globals
 *
 * These variables are declared extern in sessions.c as well; main.c owns
 * the storage. External modules only read g_sessions via the declarations in
 * sessions.h.
 */

sr_conn_ctx_t         *g_conn;
static const struct ly_ctx  *ly_ctx;
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
		"\"default\":\"running\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_get_config, 1
	},
	{
		"sr_edit_config",
		"Apply a configuration change.",
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
		"get_tree",
		"Return the YANG schema tree of a module.",
		"{\"type\":\"object\",\"properties\":{"
		"\"module\":{\"type\":\"string\"},"
		"\"xpath\":{\"type\":\"string\"},"
		"\"with_descriptions\":{\"type\":\"boolean\","
		"\"default\":false}},\"required\":[\"module\"]}",
		tool_get_tree, 1
	},
	{
		"get_help",
		"Document one YANG schema node.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"}},\"required\":[\"xpath\"]}",
		tool_get_help, 1
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

static void
usage(FILE *out)
{
	fprintf(out,
		CONFIG_PACKAGE_NAME " " CONFIG_PACKAGE_VERSION
		" - MCP server for the sysrepo datastore\n"
		"\n"
		"Usage: " CONFIG_PACKAGE_NAME " [ -f <file> ] [ --config <file> ]\n"
		"             [ --help ] [ --version ]\n"
		"\n"
		"  -f, --config    path to the libconfig file\n"
		"                  (default: /etc/sysrepo-mcp/" CONFIG_PACKAGE_NAME
		".conf)\n"
		"  --help      print this message and exit\n"
		"  --version   print the version and exit\n"
		"\n"
		"With no argument, " CONFIG_PACKAGE_NAME " expects to be started as a\n"
		"FastCGI application by a web server. Sessions are held in this\n"
		"process, so the FastCGI configuration must use max-procs = 1.\n");
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

	if ((rc = sr_connect(0, &g_conn)) != SR_ERR_OK) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": sr_connect: %s\n",
		        sr_strerror(rc));
		return rc;
	}
	ly_ctx = sr_acquire_context(g_conn);
	if (ly_ctx == NULL) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": sr_acquire_context: failed\n");
		sr_disconnect(g_conn);
		g_conn = NULL;
		return -1;
	}

	/* NACM : initialiser le contrôle d'accès une fois pour toutes les sessions. */

	g_nacm_sub = NULL;

	rc = sr_session_start(g_conn, SR_DS_RUNNING, &sess);
	if (rc == SR_ERR_OK)
		rc = sr_nacm_init(sess, 0, &g_nacm_sub);
	if (rc != SR_ERR_OK) {
		if (sess)
			sr_session_stop(sess);
		fprintf(stderr, CONFIG_PACKAGE_NAME
			": warning: sr_nacm_init: %s (NACM disabled)\n",
			sr_strerror(rc));
	}

	return SR_ERR_OK;
}

static void
sysrepo_close(void)
{
	if (g_conn) {
		sr_nacm_destroy();
		sr_release_context(g_conn);
		sr_disconnect(g_conn);
		g_conn = NULL;
	}
}

int
main(int argc, char *argv[])
{
	FCGX_Request     req;
	struct sigaction sa;
	const char       *config_path = "/etc/sysrepo-mcp/sysrepo-mcp.conf";
	int              opt;
	static struct option long_options[] = {
		{"config", required_argument, NULL, 'f'},
		{"help",   no_argument,       NULL, 'h'},
		{"version",no_argument,       NULL, 'V'},
		{NULL,     0,                 NULL,  0  }
	};

	while ((opt = getopt_long(argc, argv, "f:hV", long_options, NULL)) != -1) {
		fprintf(stderr, "%c %s\n", opt, optarg);
		switch (opt) {
		case 'f':
			config_path = optarg;
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

	if (FCGX_Init()) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": FCGX_Init failed\n");
		return EXIT_FAILURE;
	}

	/*
	 * FCGX_IsCGI() is the only correct test. Probing getenv() cannot work:
	 * under FastCGI the request parameters arrive per request in
	 * req.envp, never in the process environment.
	 */
	if (FCGX_IsCGI()) {
		fprintf(stderr,
		        CONFIG_PACKAGE_NAME ": not started as a FastCGI application.\n"
		        "Run it behind a reverse proxy, or use --help.\n");
		return EXIT_FAILURE;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": sigaction: %s\n",
		        strerror(errno));
		return EXIT_FAILURE;
	}
	signal(SIGPIPE, SIG_IGN);

	if (sysrepo_open() != SR_ERR_OK)
		return EXIT_FAILURE;

	g_start_time = time(NULL);

	/* Load runtime configuration from libconfig file. */

	struct mcp_config cfg;

	mcp_config_set_defaults(&cfg);

	if (mcp_config_load(config_path, &cfg) < 0)
		fprintf(stderr, CONFIG_PACKAGE_NAME
			": warning: could not load config file, "
			"using built-in defaults\n");
	mcp_config_set(&cfg);

	if (sessions_init(mcp_config_get()->max_sessions) < 0) {
		mcp_config_free(&cfg);
		sysrepo_close();
		return EXIT_FAILURE;
	}

	mcp_config_free(&cfg);

	if (FCGX_InitRequest(&req, 0, 0)) {
		fprintf(stderr, CONFIG_PACKAGE_NAME ": FCGX_InitRequest failed\n");
		sysrepo_close();
		return EXIT_FAILURE;
	}

	fprintf(stderr, CONFIG_PACKAGE_NAME " " CONFIG_PACKAGE_VERSION ": ready\n");

	while (!stopping && FCGX_Accept_r(&req) >= 0) {
		serve(&req);
		FCGX_Finish_r(&req);
	}

	sessions_free();

	FCGX_Free(&req, 1);
	sysrepo_close();

	fprintf(stderr, CONFIG_PACKAGE_NAME ": stopped\n");

	return EXIT_SUCCESS;
}
