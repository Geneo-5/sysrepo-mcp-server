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
#include <sysrepo/mcp/diff.h>

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
		"Read configuration data from a datastore (running by default, or "
		"startup or candidate). xpath is absolute and starts with the module "
		"NAME, not its YANG prefix. Examples: /oven:oven, "
		"/oven:oven/temperature, "
		"/ietf-interfaces:interfaces/interface[name='eth0']. Returns data, "
		"a JSON tree whose top-level keys carry the module name, for "
		"example {\"oven:oven\":{\"temperature\":200}}. An empty data object "
		"means nothing is stored at that path: it is not an error, and a "
		"leaf that was never set is absent even when its model defines a "
		"default. Only configuration is read: use sr_get_operational for "
		"state data.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Absolute path starting with the module name, e.g. /oven:oven.\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\","
		"\"description\":\"Datastore to read. Default running.\"},"
		"\"max_depth\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,"
		"\"description\":\"Maximum depth of the returned subtree; 0 means "
		"no limit.\"},"
		"\"options\":{\"type\":\"array\",\"items\":{"
		"\"type\":\"string\",\"enum\":[\"trim-defaults\","
		"\"all-defaults\"]},"
		"\"description\":\"Optional default-value handling, at most one "
		"value: trim-defaults omits leaves that hold their default, "
		"all-defaults prints default values too.\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_get_config, 1
	},
	{
		"sr_edit_config",
		"Create or modify configuration data. The edit is validated and "
		"applied immediately as one transaction on the chosen datastore "
		"(running by default). config is a JSON tree whose top-level keys "
		"carry the module name, for example "
		"{\"oven:oven\":{\"temperature\":200,\"turned-on\":true}}. A list "
		"is an array of objects that include their key leaves. Values must "
		"match the schema (type, range, mandatory leaves): an unknown node "
		"or an invalid value is refused and nothing is written. With the "
		"default operation merge, only the nodes you send are added or "
		"updated; replace overwrites them instead. This tool cannot "
		"delete: use sr_delete_config. To review a change before applying "
		"it, edit candidate, compare with sr_diff_config, then promote it "
		"with sr_copy_config (source candidate, destination running). "
		"Returns ok, operation and edit_nodes, the number of nodes "
		"submitted.",
		"{\"type\":\"object\",\"properties\":{"
		"\"config\":{\"type\":\"object\",\"description\":"
		"\"Data to apply, as a JSON tree. Top-level keys are "
		"module-prefixed node names such as oven:oven; nested keys are "
		"plain node names.\"},"
		"\"operation\":{\"type\":\"string\","
		"\"enum\":[\"merge\",\"replace\",\"none\"],\"default\":\"merge\","
		"\"description\":\"merge: add or update only the nodes sent. "
		"replace: overwrite the nodes sent. none: no default operation, so "
		"nothing changes by itself. Default merge.\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\","
		"\"description\":\"Datastore to edit. Default running.\"}},"
		"\"required\":[\"config\"]}",
		tool_sr_edit_config, 1
	},
	{
		"sr_delete_config",
		"Delete configuration data selected by an XPath, applied "
		"immediately. It is the only tool that can delete. "
		"/oven:oven/temperature removes one leaf, /oven:oven removes the "
		"container and everything below it, a list path without predicate "
		"removes every entry, and "
		"/ietf-interfaces:interfaces/interface[name='eth0'] removes one "
		"entry. There is no confirmation and no undo. Deleting data that "
		"does not exist succeeds silently unless strict is true. Returns "
		"ok and xpath.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Absolute path of the data to remove, starting with the module "
		"name; select one list entry with [key='value'].\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\","
		"\"description\":\"Datastore to edit. Default running.\"},"
		"\"strict\":{\"type\":\"boolean\",\"default\":false,"
		"\"description\":\"Fail with not found when nothing matches. "
		"Default false.\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_delete_config, 1
	},
	{
		"sr_copy_config",
		"Replace ALL data of the destination datastore with the contents "
		"of the source datastore, for every module: there is no module or "
		"path filter. Typical uses: copy candidate to running to apply "
		"changes edited and checked in candidate; copy running to startup "
		"so the running configuration survives a restart; copy startup to "
		"running to go back to the saved configuration. Source and "
		"destination must differ. Returns ok, source and destination.",
		"{\"type\":\"object\",\"properties\":{"
		"\"source\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"description\":\"Datastore to copy from.\"},"
		"\"destination\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"description\":\"Datastore that is overwritten. Must differ "
		"from source.\"}},"
		"\"required\":[\"source\",\"destination\"]}",
		tool_sr_copy_config, 1
	},
	{
		"sr_diff_config",
		"Compare two datastores and list what differs, going from source "
		"to destination. Typical use: review the edits made in candidate "
		"before applying them, with source running and destination "
		"candidate. Each entry of diff has xpath, operation (created: "
		"only in destination; deleted: only in source; replaced: the value "
		"differs), value (destination side, absent for deleted) and "
		"previous_value (source side, absent for created). changed is the "
		"number of entries and an empty diff means no difference. The "
		"reply also echoes source, target (the destination you gave) and "
		"xpath. Only configuration data is compared. Source and "
		"destination must differ.",
		"{\"type\":\"object\",\"properties\":{"
		"\"source\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"description\":\"Datastore taken as the starting point.\"},"
		"\"destination\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"description\":\"Datastore compared against source. Must "
		"differ from it.\"},"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Subtree to compare, starting with the module name, e.g. "
		"/oven:oven. Default: every module.\"},"
		"\"max_depth\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,"
		"\"description\":\"Maximum depth read on each side; 0 means no "
		"limit.\"}},"
		"\"required\":[\"source\",\"destination\"]}",
		tool_sr_diff_config, 1
	},
	{
		"sr_get_operational",
		"Read operational data: the running configuration merged with "
		"state data, the read-only nodes such as /oven:oven-state that "
		"running applications publish. State exists only while an "
		"application provides it, so an empty data object means nothing "
		"matches or no application is running; it is not an error. xpath "
		"is absolute and starts with the module NAME. The result is a "
		"snapshot: call again to follow a value. Returns data, a JSON "
		"tree whose top-level keys carry the module name.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Absolute path starting with the module name, e.g. "
		"/oven:oven-state.\"},"
		"\"max_depth\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,"
		"\"description\":\"Maximum depth of the returned subtree; 0 means "
		"no limit.\"},"
		"\"timeout_ms\":{\"type\":\"integer\",\"default\":5000,"
		"\"description\":\"Milliseconds to wait for the applications that "
		"provide state. Default 5000.\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_get_operational, 1
	},
	/* RPC and actions */
	{
		"sr_execute_rpc",
		"Call a YANG RPC, an operation defined at the top level of an "
		"installed module, for example /oven:insert-food. RPC paths are "
		"listed in the entries of sr_list_modules, and their input leaves "
		"are described by get_schema on the RPC path. input is a flat JSON "
		"object: each key is an input leaf name and each value is its "
		"value, for example {\"time\":\"on-oven-ready\"}. Values are sent as "
		"text and nested objects are not supported. Returns output, a JSON "
		"tree that is empty when the RPC returns nothing. It fails when no "
		"running application handles the RPC. Standard NETCONF operations "
		"such as get-config are not available here: use the sr_*_config "
		"tools.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Path of the RPC, /module:rpc-name.\"},"
		"\"input\":{\"type\":\"object\",\"description\":"
		"\"Flat object mapping input leaf names to their values. Omit it "
		"when the RPC takes no input.\"},"
		"\"timeout_ms\":{\"type\":\"integer\",\"default\":5000,"
		"\"description\":\"Milliseconds to wait for the handler. Default "
		"5000.\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_execute_rpc, 1
	},
	{
		"sr_action",
		"Call a YANG action, an operation attached to one instance of a "
		"data node. xpath is the full data path, including the keys of "
		"every list on the way, for example "
		"/module:container/list[key='value']/action-name. Actions are "
		"found with get_schema on the parent node. input has the same flat "
		"format as in sr_execute_rpc. Returns output, a JSON tree. It "
		"fails when no running application handles the action.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Full data path of the action, with the keys of every list, e.g. "
		"/module:list[key='value']/action-name.\"},"
		"\"input\":{\"type\":\"object\",\"description\":"
		"\"Flat object mapping input leaf names to their values. Omit it "
		"when the action takes no input.\"},"
		"\"timeout_ms\":{\"type\":\"integer\",\"default\":5000,"
		"\"description\":\"Milliseconds to wait for the handler. Default "
		"5000.\"}},"
		"\"required\":[\"xpath\"]}",
		tool_sr_action, 1
	},
	/* Notifications */
	{
		"sr_notif_subscribe",
		"Start watching a module's YANG notifications on the current "
		"session. It needs an MCP session: call initialize first and send "
		"the Mcp-Session-Id header it returns. Notifications are not "
		"pushed to you: the server queues them and you collect them with "
		"sr_notif_poll. Give xpath to keep only one notification, for "
		"example /oven:oven-ready; without it every notification of the "
		"module is queued. Returns subscription_id, module and "
		"session_id. The queue is bounded, so poll regularly. Not "
		"available to clients of the stateless 2026-07-28 protocol.",
		"{\"type\":\"object\",\"properties\":{"
		"\"module\":{\"type\":\"string\",\"description\":"
		"\"Name of the module whose notifications to watch, e.g. oven.\"},"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Only queue this notification: absolute path starting with the "
		"module name. Default: all of the module.\"},"
		"\"replay_start\":{\"type\":\"integer\",\"minimum\":0,"
		"\"description\":\"Unix timestamp in seconds: also deliver stored "
		"notifications from that moment. Needs replay enabled for the "
		"module in sysrepo; otherwise only new notifications arrive.\"}},"
		"\"required\":[\"module\"]}",
		tool_sr_notif_subscribe, 0
	},
	{
		"sr_notif_unsubscribe",
		"Stop watching notifications on the current session: one "
		"subscription when subscription_id is given, all of them "
		"otherwise. It needs an MCP session. Notifications already queued "
		"stay available to sr_notif_poll. Returns removed, the number of "
		"subscriptions dropped. An unknown subscription_id is a not found "
		"error.",
		"{\"type\":\"object\",\"properties\":{"
		"\"subscription_id\":{\"type\":\"integer\",\"minimum\":1,"
		"\"description\":\"Subscription to drop, as returned by "
		"sr_notif_subscribe. Omit it to drop all subscriptions of the "
		"session.\"}}}",
		tool_sr_notif_unsubscribe, 0
	},
	{
		"sr_notif_list_subscriptions",
		"List the notification subscriptions of the current session: "
		"subscription_id, module, xpath filter if any, and how many "
		"notifications each one received. It also reports how many "
		"notifications are pending. It needs an MCP session.",
		"{\"type\":\"object\",\"properties\":{}}",
		tool_sr_notif_list_subscriptions, 0
	},
	{
		"sr_notif_poll",
		"Collect the notifications queued on the current session since "
		"the last poll, and remove them from the queue. It needs an MCP "
		"session. Each item has xpath, kind (realtime for a live event, "
		"replay for a replayed one, replay-complete when the replay ends, "
		"and terminated, modified, suspended or resumed for subscription "
		"lifecycle), timestamp in unix seconds, and data, the notification "
		"content. pending is what remains queued and dropped counts events "
		"lost because the queue overflowed, so poll more often when it is "
		"not 0. An empty notifications array means nothing new. Events "
		"are only received between requests, so an event you just sent "
		"may need a second poll.",
		"{\"type\":\"object\",\"properties\":{"
		"\"max\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,"
		"\"description\":\"Return at most this many notifications; 0 "
		"returns everything pending.\"},"
		"\"peek\":{\"type\":\"boolean\",\"default\":false,"
		"\"description\":\"Return the notifications without removing them "
		"from the queue. Default false.\"}}}",
		tool_sr_notif_poll, 0
	},
	{
		"sr_notif_send",
		"Emit a YANG notification so that subscribers receive it, for "
		"example /oven:oven-ready. Mostly useful to test a subscription. "
		"xpath is the notification path, listed in the entries of "
		"sr_list_modules. input is a flat JSON object of the notification's "
		"leaves, in the same format as the input of sr_execute_rpc. It "
		"returns as soon as the event is published. Returns ok and xpath. "
		"No session is needed.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\",\"description\":"
		"\"Path of the notification, /module:notification-name.\"},"
		"\"input\":{\"type\":\"object\",\"description\":"
		"\"Flat object mapping the notification's leaf names to their "
		"values. Omit it when the notification has no content.\"}},"
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
		"Explore compiled YANG schema. Start without xpath and with "
		"max_depth=1 to discover exact paths, then inspect a subtree.",
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
	mcp_log_debug("receive signal %d", signum);
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
	int              rc;
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

	if ((rc = mcp_config_load(config_path, &cfg)) < 0) {
		if (rc == -ENOENT) {
			/* Aucun fichier de configuration : retomber sur les défauts
			 * intégrés. C'est le seul cas documenté où l'absence n'est pas
			 * une erreur — la valeur par défaut, sans authentification,
			 * se comporte comme l'ancien server non authentifié. */
			mcp_log_warn("config file %s not found; using built-in defaults",
		             config_path);
		} else {
			/* Syntaxe ou validation ratée : échoué fermé. Ne pas continuer
			 * avec les défauts, sinon tout le propos du serveur — et
			 * l'authentification configurée, todo.rst P0.2 — serait désactivé
			 * silencieusement. Fermer avant sysrepo_open(), avant de lier
			 * un listener, avant toute exposition. */
			mcp_config_free(&cfg);
			mcp_log_close();
			mcp_log_err("config file %s failed to load; shutting down",
	                    config_path);
			return EXIT_FAILURE;
		}
	}
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
