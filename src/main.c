/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * MCP server exposing the sysrepo datastore over FastCGI.
 *
 * Transport
 * ---------
 * The FCGX_* API is used throughout. <fcgi_stdio.h> is deliberately NOT
 * included: it remaps printf() onto its own stream, which is not the
 * FCGX_Request accepted by the loop below, so a response written with printf()
 * would never reach the client.
 *
 * Data model
 * ----------
 * Data crosses the sysrepo API as libyang trees (struct lyd_node), never as
 * the deprecated sr_val_t arrays: an sr_val_t array cannot represent a tree,
 * and rebuilding the hierarchy by hand is a dead end.
 *
 * Sessions and notifications are PROCESS-LOCAL
 * --------------------------------------------
 * A session, its notification subscriptions and its notification queue all
 * live in this process. With a FastCGI max-procs above 1, consecutive
 * requests from one agent land in different processes and the session is not
 * found: the deployment MUST therefore use max-procs = 1 until the session
 * store moves to shared storage. See sphinx/todo.rst, milestone 4.
 *
 * Notification events are processed synchronously, at the start of every
 * request, with SR_SUBSCR_NO_THREAD plus sr_subscription_process_events().
 * That keeps the whole server single-threaded: no locking, and a queue that
 * can only change between requests, never during one.
 *
 * Not implemented
 * ---------------
 * Authentication and NACM. Every request is served with the rights of the
 * system user running this process. Do not expose this build to an untrusted
 * agent.
 *
 ******************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <fcgiapp.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

/******************************************************************************
 * Build-time identity and limits
 ******************************************************************************/

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "sysrepo-mcp"
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "0.0.0-unknown"
#endif

/* MCP revision this server implements. */
#define MCP_PROTOCOL_VERSION "2025-06-18"

/* Largest request body accepted, in bytes. */
#define MCP_MAX_BODY (1024 * 1024)

/* Default sysrepo operation timeout, in milliseconds. */
#define MCP_DEFAULT_TIMEOUT_MS 5000

/* Hard cap on schema recursion depth, so a pathological model cannot blow the
 * stack. */
#define MCP_MAX_TREE_DEPTH 32

/* Kconfig supplies these; the fallbacks keep the file compilable on its own. */
#ifndef CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS
#define CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS 64
#endif

#ifndef CONFIG_SYSREPO_MCP_SERVER_SESSION_TTL
#define CONFIG_SYSREPO_MCP_SERVER_SESSION_TTL 1800
#endif

#ifndef CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE
#define CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE 256
#endif

#define MCP_MAX_SESSIONS   CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS
#define MCP_SESSION_TTL    CONFIG_SYSREPO_MCP_SERVER_SESSION_TTL
#define MCP_NOTIF_CAPACITY CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE

/* 16 random bytes, hex encoded. */
#define MCP_SESSION_ID_LEN 33

/******************************************************************************
 * JSON-RPC and MCP error codes
 *
 * The implementation-defined range is documented in sphinx/api.rst; keep the
 * two in sync.
 ******************************************************************************/

enum mcp_code {
	MCP_ERR_PARSE           = -32700,
	MCP_ERR_INVALID_REQUEST = -32600,
	MCP_ERR_METHOD          = -32601,
	MCP_ERR_PARAMS          = -32602,
	MCP_ERR_INTERNAL        = -32603,
	MCP_ERR_SERVER          = -32000,
	MCP_ERR_UNAUTHENTICATED = -32001,
	MCP_ERR_DENIED          = -32002,
	MCP_ERR_NOT_FOUND       = -32003,
	MCP_ERR_UNSUPPORTED     = -32004,
	MCP_ERR_VALIDATION      = -32005,
	MCP_ERR_LOCKED          = -32006,
	MCP_ERR_TIMEOUT         = -32007,
	MCP_ERR_NO_SESSION      = -32008,
};

struct mcp_err {
	int  code;
	char message[128];
	char detail[512];
	int  sr_code;           /* SR_ERR_* when the failure came from sysrepo */
};

static void
mcp_err_clear(struct mcp_err *err)
{
	err->code = 0;
	err->message[0] = '\0';
	err->detail[0] = '\0';
	err->sr_code = SR_ERR_OK;
}

static void
mcp_err_set(struct mcp_err *err, int code, const char *message,
            const char *fmt, ...)
{
	va_list ap;

	err->code = code;
	err->sr_code = SR_ERR_OK;
	snprintf(err->message, sizeof(err->message), "%s", message);

	if (fmt) {
		va_start(ap, fmt);
		vsnprintf(err->detail, sizeof(err->detail), fmt, ap);
		va_end(ap);
	} else {
		err->detail[0] = '\0';
	}
}

/*
 * Map a sysrepo return code onto a JSON-RPC code.
 *
 * Collapsing every sysrepo failure onto -32603 would lose the distinction
 * between "your request is wrong", which an agent can act on, and "the server
 * is broken", which it cannot. SR_ERR_LY in particular always originates from
 * something the client supplied: an XPath libyang could not compile, or data
 * that does not match the schema.
 */
static int
mcp_code_from_sr(int rc)
{
	switch (rc) {
	case SR_ERR_NOT_FOUND:
		return MCP_ERR_NOT_FOUND;
	case SR_ERR_INVAL_ARG:
	case SR_ERR_LY:
		return MCP_ERR_PARAMS;
	case SR_ERR_VALIDATION_FAILED:
	case SR_ERR_EXISTS:
		return MCP_ERR_VALIDATION;
	case SR_ERR_UNAUTHORIZED:
		return MCP_ERR_DENIED;
	case SR_ERR_LOCKED:
		return MCP_ERR_LOCKED;
	case SR_ERR_TIME_OUT:
		return MCP_ERR_TIMEOUT;
	case SR_ERR_UNSUPPORTED:
		return MCP_ERR_UNSUPPORTED;
	case SR_ERR_OPERATION_FAILED:
	case SR_ERR_CALLBACK_FAILED:
		return MCP_ERR_SERVER;
	default:
		return MCP_ERR_INTERNAL;
	}
}

/******************************************************************************
 * sysrepo connection
 *
 * One connection per process, for the lifetime of the process: opening one is
 * expensive and reusing one is cheap.
 ******************************************************************************/

static sr_conn_ctx_t *g_conn;
static time_t         g_start_time;

static void
sr_log_forward(sr_log_level_t level, const char *message)
{
	const char *tag;

	switch (level) {
	case SR_LL_ERR:  tag = "ERR";  break;
	case SR_LL_WRN:  tag = "WRN";  break;
	case SR_LL_INF:  tag = "INF";  break;
	case SR_LL_DBG:  tag = "DBG";  break;
	default:         tag = "???";  break;
	}

	/* TODO: route through elog once it is wired in (see sphinx/todo.rst).
	 * Under FastCGI stderr is captured by the web server, so this is the
	 * proxy error log, not a terminal. */
	fprintf(stderr, PACKAGE_NAME ": sysrepo [%s] %s\n", tag, message);
}

static int
sysrepo_open(void)
{
	int rc;

	sr_log_set_cb(sr_log_forward);

	rc = sr_connect(SR_CONN_DEFAULT, &g_conn);
	if (rc != SR_ERR_OK) {
		fprintf(stderr, PACKAGE_NAME ": sr_connect: %s\n",
		        sr_strerror(rc));
		return rc;
	}

	return SR_ERR_OK;
}

static void
sysrepo_close(void)
{
	if (g_conn) {
		sr_disconnect(g_conn);
		g_conn = NULL;
	}
}

/*
 * Copy the sysrepo error message of a failed operation into err.
 *
 * sr_strerror() only names the code; sr_session_get_error() carries the
 * message that says which node was rejected and why, which is the part an
 * agent needs.
 */
static void
mcp_err_from_session(struct mcp_err *err, sr_session_ctx_t *sess, int rc,
                     const char *what)
{
	const sr_error_info_t *info = NULL;
	const char            *msg = NULL;

	if (sess && (sr_session_get_error(sess, &info) == SR_ERR_OK) &&
	    info && info->err_count && info->err[0].message) {
		msg = info->err[0].message;
	}

	err->code = mcp_code_from_sr(rc);
	err->sr_code = rc;
	snprintf(err->message, sizeof(err->message), "%s", sr_strerror(rc));
	snprintf(err->detail, sizeof(err->detail), "%s: %s", what,
	         msg ? msg : sr_strerror(rc));
}

/*
 * Last libyang error message of a context.
 *
 * Wrapped in one place on purpose: the libyang error accessors have been
 * renamed more than once across major versions, so a rename costs one edit
 * here rather than one per call site.
 */
static const char *
ly_error_text(const struct ly_ctx *ly)
{
	const struct ly_err_item *item = ly_err_first(ly);

	if (item && item->msg)
		return item->msg;

	return "unknown libyang error";
}

/******************************************************************************
 * Sessions
 ******************************************************************************/

struct mcp_notif {
	char    *path;          /* data path of the notification */
	char    *json;          /* the notification tree, as libyang JSON */
	char    *kind;          /* realtime, replay, replay-complete, ... */
	int64_t  timestamp;     /* seconds since the epoch */
};

struct mcp_subscription {
	struct mcp_subscription *next;
	struct mcp_session      *owner;
	uint32_t                 id;
	char                    *module;
	char                    *xpath;
	sr_subscription_ctx_t   *sub;
	uint64_t                 received;
};

struct mcp_session {
	int      in_use;
	char     id[MCP_SESSION_ID_LEN];
	time_t   created;
	time_t   last_activity;

	/* sysrepo session owning every notification subscription below. It
	 * must outlive them, so it is stopped only when the session dies. */
	sr_session_ctx_t        *sr_sess;
	struct mcp_subscription *subs;
	uint32_t                 next_sub_id;

	/* Ring buffer of notifications waiting to be polled. */
	struct mcp_notif queue[MCP_NOTIF_CAPACITY];
	size_t           head;
	size_t           count;
	uint64_t         total_received;
	uint64_t         total_dropped;
};

static struct mcp_session g_sessions[MCP_MAX_SESSIONS];
static unsigned           g_session_count;

static void
notif_clear(struct mcp_notif *notif)
{
	free(notif->path);
	free(notif->json);
	free(notif->kind);
	notif->path = NULL;
	notif->json = NULL;
	notif->kind = NULL;
	notif->timestamp = 0;
}

static void
session_drain(struct mcp_session *sess)
{
	size_t i;

	for (i = 0; i < sess->count; i++)
		notif_clear(&sess->queue[(sess->head + i) % MCP_NOTIF_CAPACITY]);

	sess->head = 0;
	sess->count = 0;
}

static void
session_destroy(struct mcp_session *sess)
{
	struct mcp_subscription *sub;
	struct mcp_subscription *next;

	if (!sess->in_use)
		return;

	/* Unsubscribe before stopping the sysrepo session: a subscription
	 * outliving the session it was made on is undefined. */
	for (sub = sess->subs; sub; sub = next) {
		next = sub->next;
		if (sub->sub)
			sr_unsubscribe(sub->sub);
		free(sub->module);
		free(sub->xpath);
		free(sub);
	}
	sess->subs = NULL;

	if (sess->sr_sess) {
		sr_session_stop(sess->sr_sess);
		sess->sr_sess = NULL;
	}

	session_drain(sess);

	sess->in_use = 0;
	sess->id[0] = '\0';
	sess->next_sub_id = 0;
	sess->total_received = 0;
	sess->total_dropped = 0;

	if (g_session_count)
		g_session_count--;
}

static void
sessions_expire(void)
{
	time_t now = time(NULL);
	size_t i;

	for (i = 0; i < MCP_MAX_SESSIONS; i++) {
		if (!g_sessions[i].in_use)
			continue;
		if (now - g_sessions[i].last_activity < MCP_SESSION_TTL)
			continue;

		fprintf(stderr, PACKAGE_NAME ": session %s expired\n",
		        g_sessions[i].id);
		session_destroy(&g_sessions[i]);
	}
}

static void
session_generate_id(char *out)
{
	static uint64_t counter;
	unsigned char   raw[16];
	FILE           *urandom;
	size_t          got = 0;
	size_t          i;

	urandom = fopen("/dev/urandom", "rb");
	if (urandom) {
		got = fread(raw, 1, sizeof(raw), urandom);
		fclose(urandom);
	}

	if (got != sizeof(raw)) {
		/* Never expected. Still better than a predictable identifier:
		 * anyone able to guess one can use another agent's session. */
		uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32)
		                ^ ++counter;

		for (i = 0; i < sizeof(raw); i++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			raw[i] = (unsigned char)(seed >> 33);
		}
	}

	for (i = 0; i < sizeof(raw); i++)
		snprintf(out + 2 * i, 3, "%02x", raw[i]);

	out[2 * sizeof(raw)] = '\0';
}

static struct mcp_session *
session_create(void)
{
	size_t i;

	for (i = 0; i < MCP_MAX_SESSIONS; i++) {
		struct mcp_session *sess = &g_sessions[i];

		if (sess->in_use)
			continue;

		memset(sess, 0, sizeof(*sess));
		session_generate_id(sess->id);
		sess->in_use = 1;
		sess->created = time(NULL);
		sess->last_activity = sess->created;
		g_session_count++;

		return sess;
	}

	return NULL;
}

static struct mcp_session *
session_find(const char *id)
{
	size_t i;

	if (!id || !*id)
		return NULL;

	for (i = 0; i < MCP_MAX_SESSIONS; i++) {
		if (g_sessions[i].in_use && !strcmp(g_sessions[i].id, id))
			return &g_sessions[i];
	}

	return NULL;
}

/*
 * Run every pending notification callback, on this thread.
 *
 * Subscriptions are created with SR_SUBSCR_NO_THREAD, so sysrepo never calls
 * back on its own; without this, events would simply pile up in the pipe.
 * Calling it once per request means a poll always sees everything that had
 * arrived when the request came in.
 */
static void
sessions_process_events(void)
{
	struct mcp_subscription *sub;
	size_t                   i;

	for (i = 0; i < MCP_MAX_SESSIONS; i++) {
		if (!g_sessions[i].in_use)
			continue;

		for (sub = g_sessions[i].subs; sub; sub = sub->next) {
			if (sub->sub)
				sr_subscription_process_events(sub->sub, NULL,
				                               NULL);
		}
	}
}

static void
session_push_notif(struct mcp_session *sess, char *path, char *json,
                   const char *kind, int64_t timestamp)
{
	struct mcp_notif *slot;

	if (sess->count == MCP_NOTIF_CAPACITY) {
		/* A full queue means the agent stopped polling. Dropping the
		 * oldest keeps the most recent events, which are the ones it
		 * is behind on; the count is reported so it knows. */
		notif_clear(&sess->queue[sess->head]);
		sess->head = (sess->head + 1) % MCP_NOTIF_CAPACITY;
		sess->count--;
		sess->total_dropped++;
	}

	slot = &sess->queue[(sess->head + sess->count) % MCP_NOTIF_CAPACITY];
	slot->path = path;
	slot->json = json;
	slot->kind = kind ? strdup(kind) : NULL;
	slot->timestamp = timestamp;

	sess->count++;
	sess->total_received++;
}

static const char *
notif_kind_str(sr_ev_notif_type_t type)
{
	switch (type) {
	case SR_EV_NOTIF_REALTIME:        return "realtime";
	case SR_EV_NOTIF_REPLAY:          return "replay";
	case SR_EV_NOTIF_REPLAY_COMPLETE: return "replay-complete";
	case SR_EV_NOTIF_TERMINATED:      return "terminated";
	case SR_EV_NOTIF_MODIFIED:        return "modified";
	case SR_EV_NOTIF_SUSPENDED:       return "suspended";
	case SR_EV_NOTIF_RESUMED:         return "resumed";
	default:                          return "unknown";
	}
}

static void
notif_callback(sr_session_ctx_t *session, uint32_t sub_id,
               const sr_ev_notif_type_t type, const struct lyd_node *notif,
               struct timespec *timestamp, void *private_data)
{
	struct mcp_subscription *sub = private_data;
	char                    *json = NULL;
	char                    *path = NULL;

	(void)session;
	(void)sub_id;

	if (!sub || !sub->owner || !sub->owner->in_use)
		return;

	/* notif is NULL for the lifecycle events: replay-complete,
	 * terminated, suspended, resumed. They are queued too, because an
	 * agent replaying history needs to know where the replay ends. */
	if (notif) {
		if (lyd_print_mem(&json, notif, LYD_JSON,
		                  LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK) !=
		    LY_SUCCESS)
			json = NULL;

		path = lyd_path(notif, LYD_PATH_STD, NULL, 0);
	}

	sub->received++;
	session_push_notif(sub->owner, path, json, notif_kind_str(type),
	                   timestamp ? (int64_t)timestamp->tv_sec :
	                   (int64_t)time(NULL));
}

/*
 * The sysrepo session a session's subscriptions are made on, created lazily.
 */
static int
session_sysrepo(struct mcp_session *sess, struct mcp_err *err)
{
	int rc;

	if (sess->sr_sess)
		return 0;

	rc = sr_session_start(g_conn, SR_DS_RUNNING, &sess->sr_sess);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, NULL, rc, "sr_session_start");
		sess->sr_sess = NULL;
		return -1;
	}

	return 0;
}

/******************************************************************************
 * Argument helpers
 ******************************************************************************/

static const char *
arg_string(struct json_object *args, const char *name)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return NULL;
	if (!json_object_is_type(val, json_type_string))
		return NULL;

	return json_object_get_string(val);
}

static struct json_object *
arg_object(struct json_object *args, const char *name)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return NULL;
	if (!json_object_is_type(val, json_type_object) &&
	    !json_object_is_type(val, json_type_array))
		return NULL;

	return val;
}

static int
arg_int(struct json_object *args, const char *name, int fallback)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return fallback;
	if (!json_object_is_type(val, json_type_int))
		return fallback;

	return json_object_get_int(val);
}

static int64_t
arg_int64(struct json_object *args, const char *name, int64_t fallback)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return fallback;
	if (!json_object_is_type(val, json_type_int))
		return fallback;

	return json_object_get_int64(val);
}

static int
arg_bool(struct json_object *args, const char *name, int fallback)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return fallback;
	if (!json_object_is_type(val, json_type_boolean))
		return fallback;

	return json_object_get_boolean(val) ? 1 : 0;
}

/*
 * Reject a path that cannot possibly address YANG data.
 *
 * Handed a relative or unprefixed path, sysrepo either matches nothing or
 * fails with a libyang error. Neither tells the agent the one thing it needs
 * to know, which is that its path was malformed rather than its data absent.
 * The contract is an absolute path whose first segment carries a module
 * prefix, so that is what is checked.
 */
static int
xpath_wellformed(const char *xpath)
{
	const char *p;

	if (!xpath || xpath[0] != '/')
		return 0;

	for (p = xpath + 1; *p && *p != '/'; p++) {
		if (*p == ':')
			return 1;
	}

	return 0;
}

/* Fetch and validate a required xpath argument. Returns NULL on failure. */
static const char *
arg_xpath(struct json_object *args, struct mcp_err *err)
{
	const char *xpath = arg_string(args, "xpath");

	if (!xpath) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "xpath is required and must be a string");
		return NULL;
	}
	if (!xpath_wellformed(xpath)) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "xpath must be absolute and its first segment must "
		            "carry a module prefix, as in \"/oven:oven\"; got "
		            "\"%s\"", xpath);
		return NULL;
	}

	return xpath;
}

/* Resolve a datastore name. Returns 0 on success. */
static int
arg_datastore(struct json_object *args, sr_datastore_t *out,
              struct mcp_err *err)
{
	const char *name = arg_string(args, "datastore");

	if (!name) {
		*out = SR_DS_RUNNING;
		return 0;
	}
	if (!strcmp(name, "running")) {
		*out = SR_DS_RUNNING;
		return 0;
	}
	if (!strcmp(name, "startup")) {
		*out = SR_DS_STARTUP;
		return 0;
	}
	if (!strcmp(name, "candidate")) {
		*out = SR_DS_CANDIDATE;
		return 0;
	}
	if (!strcmp(name, "operational")) {
		*out = SR_DS_OPERATIONAL;
		return 0;
	}

	mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
	            "unknown datastore \"%s\"", name);

	return -1;
}

/*
 * Serialise a libyang tree into a json-c object.
 *
 * A NULL tree is an empty result, not a failure: an XPath matching nothing is
 * a legitimate answer.
 */
static struct json_object *
tree_to_json(const struct lyd_node *tree, struct mcp_err *err)
{
	struct json_object *obj;
	char               *text = NULL;

	if (!tree)
		return json_object_new_object();

	if (lyd_print_mem(&text, tree, LYD_JSON, LYD_PRINT_SIBLINGS) !=
	    LY_SUCCESS) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "lyd_print_mem failed");
		return NULL;
	}

	obj = json_tokener_parse(text);
	free(text);

	if (!obj) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "libyang produced output json-c cannot parse");
		return NULL;
	}

	return obj;
}

/*
 * Node type name, lowercased.
 *
 * lys_nodetype2str() spells a few of them in upper case ("RPC"), which would
 * leak an inconsistency into the API: an agent matching on "rpc" would miss
 * them. One spelling, everywhere.
 */
static struct json_object *
nodetype_to_json(uint16_t nodetype)
{
	const char *name = lys_nodetype2str(nodetype);
	char        lower[32];
	size_t      i;

	if (!name)
		return json_object_new_string("unknown");

	for (i = 0; name[i] && i < sizeof(lower) - 1; i++) {
		lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ?
		           (char)(name[i] - 'A' + 'a') : name[i];
	}
	lower[i] = '\0';

	return json_object_new_string(lower);
}

/******************************************************************************
 * Tool handlers
 *
 * A handler returns the tool payload on success, or NULL with *err filled in.
 ******************************************************************************/

struct tool_ctx {
	sr_session_ctx_t   *sess;       /* per-request sysrepo session */
	struct mcp_session *mcp;        /* MCP session, NULL when sessionless */
};

typedef struct json_object *(*tool_fn)(struct tool_ctx *ctx,
                                       struct json_object *args,
                                       struct mcp_err *err);

/* ---------------------------------------------------------------- get_status
 */
static struct json_object *
tool_get_status(struct tool_ctx *ctx, struct json_object *args,
                struct mcp_err *err)
{
	struct json_object *res;

	(void)err;

	res = json_object_new_object();
	json_object_object_add(res, "version",
	                       json_object_new_string(PACKAGE_VERSION));
	json_object_object_add(res, "uptime_seconds",
	                       json_object_new_int64(
	                               (int64_t)(time(NULL) - g_start_time)));
	json_object_object_add(res, "active_sessions",
	                       json_object_new_int((int)g_session_count));
	json_object_object_add(res, "max_sessions",
	                       json_object_new_int(MCP_MAX_SESSIONS));
	json_object_object_add(res, "session_ttl_seconds",
	                       json_object_new_int(MCP_SESSION_TTL));

	if (arg_bool(args, "verbose", 0)) {
		struct json_object *list = json_object_new_array();
		time_t              now = time(NULL);
		size_t              i;

		for (i = 0; i < MCP_MAX_SESSIONS; i++) {
			struct mcp_session *sess = &g_sessions[i];
			struct json_object *entry;
			struct mcp_subscription *sub;
			unsigned            subs = 0;

			if (!sess->in_use)
				continue;

			for (sub = sess->subs; sub; sub = sub->next)
				subs++;

			entry = json_object_new_object();
			json_object_object_add(entry, "session_id",
			                       json_object_new_string(sess->id));
			json_object_object_add(entry, "created",
			                       json_object_new_int64(
			                               (int64_t)sess->created));
			json_object_object_add(entry, "idle_seconds",
			                       json_object_new_int64(
			                               (int64_t)(now - sess->last_activity)));
			json_object_object_add(entry, "subscriptions",
			                       json_object_new_int((int)subs));
			json_object_object_add(entry, "pending_notifications",
			                       json_object_new_int((int)sess->count));
			json_object_object_add(entry, "current",
			                       json_object_new_boolean(
			                               ctx->mcp == sess));
			json_object_array_add(list, entry);
		}

		json_object_object_add(res, "sessions", list);
	}

	return res;
}

/* ------------------------------------------------------------ sr_get_config
 */
static struct json_object *
tool_sr_get_config(struct tool_ctx *ctx, struct json_object *args,
                   struct mcp_err *err)
{
	const char         *xpath = arg_xpath(args, err);
	const char         *ds_name;
	sr_datastore_t      ds;
	sr_data_t          *data = NULL;
	struct json_object *res;
	struct json_object *payload;
	int                 max_depth = arg_int(args, "max_depth", 0);
	int                 rc;

	if (!xpath)
		return NULL;
	if (arg_datastore(args, &ds, err))
		return NULL;
	if (ds == SR_DS_OPERATIONAL) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "use sr_get_operational for the operational "
		            "datastore");
		return NULL;
	}
	if (max_depth < 0) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "max_depth must not be negative");
		return NULL;
	}

	rc = sr_session_switch_ds(ctx->sess, ds);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc,
		                     "sr_session_switch_ds");
		return NULL;
	}

	rc = sr_get_data(ctx->sess, xpath, (uint32_t)max_depth,
	                 MCP_DEFAULT_TIMEOUT_MS, 0, &data);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		return NULL;
	}

	payload = tree_to_json(data ? data->tree : NULL, err);
	sr_release_data(data);

	if (!payload)
		return NULL;

	ds_name = arg_string(args, "datastore");

	res = json_object_new_object();
	json_object_object_add(res, "data", payload);
	json_object_object_add(res, "xpath", json_object_new_string(xpath));
	json_object_object_add(res, "datastore",
	                       json_object_new_string(ds_name ? ds_name :
	                                              "running"));

	return res;
}

/* ----------------------------------------------------- sr_get_operational
 *
 * Always reads SR_DS_OPERATIONAL. The datastore of a session is not implicit:
 * a session left on SR_DS_RUNNING keeps answering from running, which is the
 * usual cause of an empty operational read.
 */
static struct json_object *
tool_sr_get_operational(struct tool_ctx *ctx, struct json_object *args,
                        struct mcp_err *err)
{
	const char         *xpath = arg_xpath(args, err);
	sr_data_t          *data = NULL;
	struct json_object *res;
	struct json_object *payload;
	int                 max_depth = arg_int(args, "max_depth", 0);
	int                 timeout = arg_int(args, "timeout_ms",
	                                      MCP_DEFAULT_TIMEOUT_MS);
	int                 rc;

	if (!xpath)
		return NULL;
	if (max_depth < 0 || timeout < 0) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "max_depth and timeout_ms must not be negative");
		return NULL;
	}

	rc = sr_session_switch_ds(ctx->sess, SR_DS_OPERATIONAL);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc,
		                     "sr_session_switch_ds");
		return NULL;
	}

	rc = sr_get_data(ctx->sess, xpath, (uint32_t)max_depth,
	                 (uint32_t)timeout, 0, &data);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		return NULL;
	}

	payload = tree_to_json(data ? data->tree : NULL, err);
	sr_release_data(data);

	if (!payload)
		return NULL;

	res = json_object_new_object();
	json_object_object_add(res, "data", payload);

	return res;
}

/* ----------------------------------------------------------- sr_edit_config
 *
 * Parse the config tree, stage it, commit it. sr_set_item_str() is not usable
 * here: it sets a single node from its string value and cannot be handed a
 * serialised JSON document.
 */
static struct json_object *
tool_sr_edit_config(struct tool_ctx *ctx, struct json_object *args,
                    struct mcp_err *err)
{
	struct json_object   *config = arg_object(args, "config");
	const char           *operation = arg_string(args, "operation");
	const char           *text;
	const struct ly_ctx  *ly;
	struct lyd_node      *edit = NULL;
	struct json_object   *res;
	sr_datastore_t        ds;
	int                   rc;

	if (!config) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "config is required and must be an object");
		return NULL;
	}
	if (!operation)
		operation = "merge";
	if (strcmp(operation, "merge") && strcmp(operation, "replace") &&
	    strcmp(operation, "none")) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "operation must be merge, replace or none");
		return NULL;
	}
	if (arg_datastore(args, &ds, err))
		return NULL;
	if (ds == SR_DS_OPERATIONAL) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "the operational datastore is not editable here");
		return NULL;
	}

	rc = sr_session_switch_ds(ctx->sess, ds);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc,
		                     "sr_session_switch_ds");
		return NULL;
	}

	text = json_object_to_json_string_ext(config, JSON_C_TO_STRING_PLAIN);
	if (!text) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "cannot serialise the config argument");
		return NULL;
	}

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	/* LYD_PARSE_STRICT is what makes an unknown node an error. Without
	 * it libyang silently drops data it has no schema for, and an agent
	 * that misspelled a leaf is told its edit succeeded. */
	if (lyd_parse_data_mem(ly, text, LYD_JSON,
	                       LYD_PARSE_ONLY | LYD_PARSE_STRICT, 0,
	                       &edit) != LY_SUCCESS) {
		mcp_err_set(err, MCP_ERR_VALIDATION, "Validation failed",
		            "config does not match the YANG schema: %s",
		            ly_error_text(ly));
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	sr_session_release_context(ctx->sess);

	if (!edit) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "config produced an empty edit");
		return NULL;
	}

	rc = sr_edit_batch(ctx->sess, edit, operation);
	lyd_free_all(edit);

	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_edit_batch");
		sr_discard_changes(ctx->sess);
		return NULL;
	}

	rc = sr_apply_changes(ctx->sess, MCP_DEFAULT_TIMEOUT_MS);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_apply_changes");
		sr_discard_changes(ctx->sess);
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "ok", json_object_new_boolean(1));
	json_object_object_add(res, "operation",
	                       json_object_new_string(operation));

	return res;
}

/* --------------------------------------------------------- sr_delete_config
 *
 * The only way to remove data. sr_edit_config can create and modify, but a
 * merge cannot express a deletion and a replace cannot express "nothing",
 * so without this tool an agent can fill a datastore and never empty it.
 */
static struct json_object *
tool_sr_delete_config(struct tool_ctx *ctx, struct json_object *args,
                      struct mcp_err *err)
{
	const char         *xpath = arg_xpath(args, err);
	struct json_object *res;
	sr_datastore_t      ds;
	int                 strict = arg_bool(args, "strict", 0);
	int                 rc;

	if (!xpath)
		return NULL;
	if (arg_datastore(args, &ds, err))
		return NULL;
	if (ds == SR_DS_OPERATIONAL) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "the operational datastore is not editable here");
		return NULL;
	}

	rc = sr_session_switch_ds(ctx->sess, ds);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc,
		                     "sr_session_switch_ds");
		return NULL;
	}

	/* Without SR_EDIT_STRICT, deleting something that is not there
	 * succeeds. That is the useful default for a cleanup, and the strict
	 * argument is there for an agent that needs to know. */
	rc = sr_delete_item(ctx->sess, xpath,
	                    strict ? SR_EDIT_STRICT : 0);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_delete_item");
		sr_discard_changes(ctx->sess);
		return NULL;
	}

	rc = sr_apply_changes(ctx->sess, MCP_DEFAULT_TIMEOUT_MS);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_apply_changes");
		sr_discard_changes(ctx->sess);
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "ok", json_object_new_boolean(1));
	json_object_object_add(res, "xpath", json_object_new_string(xpath));

	return res;
}

/* -------------------------------------------------- sr_execute_rpc/sr_action
 *
 * An action differs from an RPC only in being anchored to a data node, which
 * is why its xpath must identify a concrete instance. Both are sent with
 * sr_rpc_send_tree(), so the two tools share this implementation.
 */
static struct json_object *
rpc_common(struct tool_ctx *ctx, struct json_object *args,
           struct mcp_err *err)
{
	const char          *xpath = arg_xpath(args, err);
	struct json_object  *input;
	const struct ly_ctx *ly;
	struct lyd_node     *op = NULL;
	sr_data_t           *output = NULL;
	struct json_object  *res;
	struct json_object  *payload;
	int                  timeout = arg_int(args, "timeout_ms",
	                                       MCP_DEFAULT_TIMEOUT_MS);
	int                  rc;

	if (!xpath)
		return NULL;

	input = arg_object(args, "input");

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	if (lyd_new_path(NULL, ly, xpath, NULL, 0, &op) != LY_SUCCESS) {
		mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
		            "cannot resolve \"%s\": %s", xpath,
		            ly_error_text(ly));
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	/* Input parameters are added as relative paths under the operation
	 * node, so libyang types and validates each of them against the
	 * schema. Building sr_val_t structures by hand cannot do that. */
	if (input && json_object_is_type(input, json_type_object)) {
		json_object_object_foreach(input, key, val) {
			const char *text = json_object_get_string(val);

			if (lyd_new_path(op, NULL, key, text, 0, NULL) !=
			    LY_SUCCESS) {
				mcp_err_set(err, MCP_ERR_VALIDATION,
				            "Validation failed",
				            "input \"%s\" rejected: %s", key,
				            ly_error_text(ly));
				lyd_free_all(op);
				sr_session_release_context(ctx->sess);
				return NULL;
			}
		}
	}

	sr_session_release_context(ctx->sess);

	rc = sr_rpc_send_tree(ctx->sess, op, (uint32_t)timeout, &output);
	lyd_free_all(op);

	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_rpc_send_tree");
		return NULL;
	}

	payload = tree_to_json(output ? output->tree : NULL, err);
	sr_release_data(output);

	if (!payload)
		return NULL;

	res = json_object_new_object();
	json_object_object_add(res, "output", payload);

	return res;
}

static struct json_object *
tool_sr_execute_rpc(struct tool_ctx *ctx, struct json_object *args,
                    struct mcp_err *err)
{
	return rpc_common(ctx, args, err);
}

static struct json_object *
tool_sr_action(struct tool_ctx *ctx, struct json_object *args,
               struct mcp_err *err)
{
	return rpc_common(ctx, args, err);
}

/******************************************************************************
 * Notification tools
 *
 * The whole point of a session: a subscription and its queue belong to one
 * agent, and survive between its requests.
 ******************************************************************************/

static struct mcp_subscription *
subscription_find(struct mcp_session *sess, uint32_t id)
{
	struct mcp_subscription *sub;

	for (sub = sess->subs; sub; sub = sub->next) {
		if (sub->id == id)
			return sub;
	}

	return NULL;
}

/* Require an MCP session, and say how to get one. */
static int
need_session(struct tool_ctx *ctx, struct mcp_err *err, const char *tool)
{
	if (ctx->mcp)
		return 0;

	mcp_err_set(err, MCP_ERR_NO_SESSION, "Session required",
	            "%s keeps state between requests: call initialize, then "
	            "send the Mcp-Session-Id header it returns", tool);

	return -1;
}

/* ------------------------------------------------------- sr_notif_subscribe
 */
static struct json_object *
tool_sr_notif_subscribe(struct tool_ctx *ctx, struct json_object *args,
                        struct mcp_err *err)
{
	const char              *module = arg_string(args, "module");
	const char              *xpath = arg_string(args, "xpath");
	struct mcp_subscription *sub;
	struct json_object      *res;
	struct timespec          start;
	struct timespec         *start_ptr = NULL;
	int64_t                  replay_start;
	int                      rc;

	if (need_session(ctx, err, "sr_notif_subscribe"))
		return NULL;
	if (!module) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "module is required");
		return NULL;
	}
	if (xpath && !xpath_wellformed(xpath)) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "xpath must be absolute and carry a module prefix; "
		            "got \"%s\"", xpath);
		return NULL;
	}

	/* Replaying history needs the module to have replay support enabled
	 * in sysrepo; without it the subscription starts at "now". */
	replay_start = arg_int64(args, "replay_start", 0);
	if (replay_start > 0) {
		start.tv_sec = (time_t)replay_start;
		start.tv_nsec = 0;
		start_ptr = &start;
	}

	if (session_sysrepo(ctx->mcp, err))
		return NULL;

	sub = calloc(1, sizeof(*sub));
	if (!sub) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "out of memory");
		return NULL;
	}

	sub->owner = ctx->mcp;
	sub->id = ++ctx->mcp->next_sub_id;
	sub->module = strdup(module);
	sub->xpath = xpath ? strdup(xpath) : NULL;

	/* SR_SUBSCR_NO_THREAD: sysrepo must not call back on a thread of its
	 * own. Events are drained at the start of each request instead, which
	 * keeps the server single-threaded and the queue free of locking. */
	rc = sr_notif_subscribe_tree(ctx->mcp->sr_sess, module, xpath,
	                             start_ptr, NULL, notif_callback, sub,
	                             SR_SUBSCR_NO_THREAD, &sub->sub);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->mcp->sr_sess, rc,
		                     "sr_notif_subscribe_tree");
		free(sub->module);
		free(sub->xpath);
		free(sub);
		ctx->mcp->next_sub_id--;
		return NULL;
	}

	sub->next = ctx->mcp->subs;
	ctx->mcp->subs = sub;

	res = json_object_new_object();
	json_object_object_add(res, "subscription_id",
	                       json_object_new_int64((int64_t)sub->id));
	json_object_object_add(res, "module", json_object_new_string(module));
	if (xpath)
		json_object_object_add(res, "xpath",
		                       json_object_new_string(xpath));
	json_object_object_add(res, "session_id",
	                       json_object_new_string(ctx->mcp->id));

	return res;
}

/* ----------------------------------------------------- sr_notif_unsubscribe
 */
static struct json_object *
tool_sr_notif_unsubscribe(struct tool_ctx *ctx, struct json_object *args,
                          struct mcp_err *err)
{
	struct mcp_subscription  *sub;
	struct mcp_subscription **link;
	struct json_object       *res;
	int                       removed = 0;
	int                       id = arg_int(args, "subscription_id", 0);

	if (need_session(ctx, err, "sr_notif_unsubscribe"))
		return NULL;

	if (id > 0) {
		sub = subscription_find(ctx->mcp, (uint32_t)id);
		if (!sub) {
			mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
			            "no subscription %d on this session", id);
			return NULL;
		}
	}

	link = &ctx->mcp->subs;
	while (*link) {
		sub = *link;

		if (id > 0 && sub->id != (uint32_t)id) {
			link = &sub->next;
			continue;
		}

		*link = sub->next;
		if (sub->sub)
			sr_unsubscribe(sub->sub);
		free(sub->module);
		free(sub->xpath);
		free(sub);
		removed++;
	}

	res = json_object_new_object();
	json_object_object_add(res, "removed", json_object_new_int(removed));

	return res;
}

/* ----------------------------------------------- sr_notif_list_subscriptions
 */
static struct json_object *
tool_sr_notif_list_subscriptions(struct tool_ctx *ctx,
                                 struct json_object *args,
                                 struct mcp_err *err)
{
	struct mcp_subscription *sub;
	struct json_object      *res;
	struct json_object      *list;

	(void)args;

	if (need_session(ctx, err, "sr_notif_list_subscriptions"))
		return NULL;

	list = json_object_new_array();

	for (sub = ctx->mcp->subs; sub; sub = sub->next) {
		struct json_object *entry = json_object_new_object();

		json_object_object_add(entry, "subscription_id",
		                       json_object_new_int64((int64_t)sub->id));
		json_object_object_add(entry, "module",
		                       json_object_new_string(sub->module));
		if (sub->xpath)
			json_object_object_add(entry, "xpath",
			                       json_object_new_string(sub->xpath));
		json_object_object_add(entry, "received",
		                       json_object_new_int64(
		                               (int64_t)sub->received));
		json_object_array_add(list, entry);
	}

	res = json_object_new_object();
	json_object_object_add(res, "subscriptions", list);
	json_object_object_add(res, "pending",
	                       json_object_new_int((int)ctx->mcp->count));
	json_object_object_add(res, "session_id",
	                       json_object_new_string(ctx->mcp->id));

	return res;
}

/* ------------------------------------------------------------ sr_notif_poll
 *
 * Everything received since the last call. Draining is the default: an agent
 * calling it in a loop must not be handed the same event twice.
 */
static struct json_object *
tool_sr_notif_poll(struct tool_ctx *ctx, struct json_object *args,
                   struct mcp_err *err)
{
	struct json_object *res;
	struct json_object *list;
	int                 max = arg_int(args, "max", 0);
	int                 peek = arg_bool(args, "peek", 0);
	size_t              taken = 0;
	size_t              available;

	if (need_session(ctx, err, "sr_notif_poll"))
		return NULL;
	if (max < 0) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "max must not be negative");
		return NULL;
	}

	available = ctx->mcp->count;
	if (max > 0 && (size_t)max < available)
		available = (size_t)max;

	list = json_object_new_array();

	while (taken < available) {
		size_t            index = (ctx->mcp->head + taken) %
		                          MCP_NOTIF_CAPACITY;
		struct mcp_notif *notif = &ctx->mcp->queue[index];
		struct json_object *entry = json_object_new_object();

		if (notif->path)
			json_object_object_add(entry, "xpath",
			                       json_object_new_string(notif->path));
		json_object_object_add(entry, "kind",
		                       json_object_new_string(
		                               notif->kind ? notif->kind :
		                               "unknown"));
		json_object_object_add(entry, "timestamp",
		                       json_object_new_int64(notif->timestamp));

		if (notif->json) {
			struct json_object *data =
			        json_tokener_parse(notif->json);

			json_object_object_add(entry, "data",
			                       data ? data :
			                       json_object_new_object());
		}

		json_object_array_add(list, entry);
		taken++;
	}

	if (!peek) {
		size_t i;

		for (i = 0; i < taken; i++)
			notif_clear(&ctx->mcp->queue[(ctx->mcp->head + i) %
			                             MCP_NOTIF_CAPACITY]);

		ctx->mcp->head = (ctx->mcp->head + taken) % MCP_NOTIF_CAPACITY;
		ctx->mcp->count -= taken;
	}

	res = json_object_new_object();
	json_object_object_add(res, "notifications", list);
	json_object_object_add(res, "returned", json_object_new_int((int)taken));
	json_object_object_add(res, "pending",
	                       json_object_new_int((int)ctx->mcp->count));
	json_object_object_add(res, "total_received",
	                       json_object_new_int64(
	                               (int64_t)ctx->mcp->total_received));
	/* Non-zero means the queue overflowed: the agent is polling too
	 * slowly, or the filter is too wide. */
	json_object_object_add(res, "dropped",
	                       json_object_new_int64(
	                               (int64_t)ctx->mcp->total_dropped));

	return res;
}

/* ------------------------------------------------------------ sr_notif_send
 *
 * Emitting a notification is what makes the subscription tools testable
 * without waiting for a device to produce one on its own.
 */
static struct json_object *
tool_sr_notif_send(struct tool_ctx *ctx, struct json_object *args,
                   struct mcp_err *err)
{
	const char          *xpath = arg_xpath(args, err);
	struct json_object  *input;
	const struct ly_ctx *ly;
	struct lyd_node     *notif = NULL;
	struct json_object  *res;
	int                  rc;

	if (!xpath)
		return NULL;

	input = arg_object(args, "input");

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	if (lyd_new_path(NULL, ly, xpath, NULL, 0, &notif) != LY_SUCCESS) {
		mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
		            "cannot resolve \"%s\": %s", xpath,
		            ly_error_text(ly));
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	if (input && json_object_is_type(input, json_type_object)) {
		json_object_object_foreach(input, key, val) {
			const char *text = json_object_get_string(val);

			if (lyd_new_path(notif, NULL, key, text, 0, NULL) !=
			    LY_SUCCESS) {
				mcp_err_set(err, MCP_ERR_VALIDATION,
				            "Validation failed",
				            "input \"%s\" rejected: %s", key,
				            ly_error_text(ly));
				lyd_free_all(notif);
				sr_session_release_context(ctx->sess);
				return NULL;
			}
		}
	}

	sr_session_release_context(ctx->sess);

	/* wait = 0 on purpose. Waiting would block on this process's own
	 * subscriptions, whose events are only processed between requests:
	 * the server would deadlock against itself. */
	rc = sr_notif_send_tree(ctx->sess, notif, 0, 0);
	lyd_free_all(notif);

	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_notif_send_tree");
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "ok", json_object_new_boolean(1));
	json_object_object_add(res, "xpath", json_object_new_string(xpath));

	return res;
}

/******************************************************************************
 * Module management
 ******************************************************************************/

static struct json_object *
tool_sr_list_modules(struct tool_ctx *ctx, struct json_object *args,
                     struct mcp_err *err)
{
	const struct ly_ctx     *ly;
	const struct lys_module *mod;
	struct json_object      *res;
	struct json_object      *list;
	uint32_t                 index = 0;
	int                      implemented_only = arg_bool(args,
	                                                     "implemented_only",
	                                                     1);

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	list = json_object_new_array();

	while ((mod = ly_ctx_get_module_iter(ly, &index))) {
		struct json_object *entry;

		if (implemented_only && !mod->implemented)
			continue;

		entry = json_object_new_object();
		json_object_object_add(entry, "name",
		                       json_object_new_string(mod->name));
		json_object_object_add(entry, "revision",
		                       json_object_new_string(
		                               mod->revision ? mod->revision :
		                               ""));
		json_object_object_add(entry, "namespace",
		                       json_object_new_string(
		                               mod->ns ? mod->ns : ""));
		json_object_object_add(entry, "prefix",
		                       json_object_new_string(
		                               mod->prefix ? mod->prefix : ""));
		json_object_object_add(entry, "implemented",
		                       json_object_new_boolean(
		                               mod->implemented ? 1 : 0));
		json_object_array_add(list, entry);
	}

	sr_session_release_context(ctx->sess);

	res = json_object_new_object();
	json_object_object_add(res, "modules", list);
	json_object_object_add(res, "count",
	                       json_object_new_int(
	                               (int)json_object_array_length(list)));

	return res;
}

static struct json_object *
tool_sr_module_install(struct tool_ctx *ctx, struct json_object *args,
                       struct mcp_err *err)
{
	const char         *file = arg_string(args, "yang_file");
	const char         *search_dirs = arg_string(args, "search_dirs");
	struct json_object *features = arg_object(args, "features");
	struct json_object *res;
	const char        **feature_list = NULL;
	size_t              feature_count = 0;
	int                 rc;

	(void)ctx;

	if (!file) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "yang_file is required");
		return NULL;
	}

	if (features && json_object_is_type(features, json_type_array)) {
		size_t i;

		feature_count = json_object_array_length(features);
		feature_list = calloc(feature_count + 1, sizeof(*feature_list));
		if (!feature_list) {
			mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
			            "out of memory");
			return NULL;
		}

		for (i = 0; i < feature_count; i++) {
			feature_list[i] = json_object_get_string(
			        json_object_array_get_idx(features, i));
		}
		feature_list[feature_count] = NULL;
	}

	rc = sr_install_module(g_conn, file, search_dirs, feature_list);
	free(feature_list);

	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, NULL, rc, "sr_install_module");
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "ok", json_object_new_boolean(1));
	json_object_object_add(res, "yang_file", json_object_new_string(file));

	return res;
}

static struct json_object *
tool_sr_module_uninstall(struct tool_ctx *ctx, struct json_object *args,
                         struct mcp_err *err)
{
	const char         *module = arg_string(args, "module");
	struct json_object *res;
	int                 force = arg_bool(args, "force", 0);
	int                 rc;

	(void)ctx;

	if (!module) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "module is required");
		return NULL;
	}

	rc = sr_remove_module(g_conn, module, force);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, NULL, rc, "sr_remove_module");
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "removed", json_object_new_boolean(1));
	json_object_object_add(res, "module", json_object_new_string(module));

	return res;
}

/******************************************************************************
 * Schema introspection
 ******************************************************************************/

static struct json_object *
schema_node_to_json(const struct lysc_node *node, int with_desc, int depth,
                    struct json_object *flat, const char *parent_path)
{
	struct json_object     *obj = json_object_new_object();
	const struct lysc_node *child;
	char                    path[1024];

	if (parent_path && *parent_path) {
		snprintf(path, sizeof(path), "%s/%s", parent_path, node->name);
	} else {
		/* A top-level node is addressed by /<module>:<name>, and that
		 * prefix is what makes the flat list usable as XPaths. */
		snprintf(path, sizeof(path), "/%s:%s",
		         node->module ? node->module->name : "", node->name);
	}

	json_object_object_add(obj, "type", nodetype_to_json(node->nodetype));
	json_object_object_add(obj, "config",
	                       json_object_new_boolean(
	                               (node->flags & LYS_CONFIG_W) ? 1 : 0));

	if (with_desc && node->dsc)
		json_object_object_add(obj, "description",
		                       json_object_new_string(node->dsc));

	if (flat) {
		struct json_object *entry = json_object_new_object();

		json_object_object_add(entry, "xpath",
		                       json_object_new_string(path));
		json_object_object_add(entry, "type",
		                       nodetype_to_json(node->nodetype));
		json_object_object_add(entry, "config",
		                       json_object_new_boolean(
		                               (node->flags & LYS_CONFIG_W) ?
		                               1 : 0));
		json_object_array_add(flat, entry);
	}

	if (depth < MCP_MAX_TREE_DEPTH) {
		struct json_object *children = NULL;

		for (child = lysc_node_child(node); child;
		     child = child->next) {
			if (!children)
				children = json_object_new_object();
			json_object_object_add(children, child->name,
			                       schema_node_to_json(
			                               child, with_desc,
			                               depth + 1, flat,
			                               path));
		}

		if (children)
			json_object_object_add(obj, "children", children);
	}

	return obj;
}

static struct json_object *
tool_get_tree(struct tool_ctx *ctx, struct json_object *args,
              struct mcp_err *err)
{
	const char              *module = arg_string(args, "module");
	const char              *xpath = arg_string(args, "xpath");
	const struct ly_ctx     *ly;
	const struct lys_module *mod;
	const struct lysc_node  *node;
	struct json_object      *res;
	struct json_object      *tree;
	struct json_object      *nodes;
	struct json_object      *roots;
	int                      with_desc = arg_bool(args,
	                                              "with_descriptions", 0);

	if (!module) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "module is required");
		return NULL;
	}
	if (xpath && strcmp(xpath, "/") && !xpath_wellformed(xpath)) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "xpath must be \"/\" or an absolute path with a "
		            "module prefix; got \"%s\"", xpath);
		return NULL;
	}

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	mod = ly_ctx_get_module_implemented(ly, module);
	if (!mod || !mod->compiled) {
		mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
		            "module \"%s\" is not implemented", module);
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	tree = json_object_new_object();
	nodes = json_object_new_array();
	roots = json_object_new_object();

	json_object_object_add(tree, "module",
	                       json_object_new_string(mod->name));
	json_object_object_add(tree, "namespace",
	                       json_object_new_string(mod->ns ? mod->ns : ""));
	json_object_object_add(tree, "prefix",
	                       json_object_new_string(mod->prefix ?
	                                              mod->prefix : ""));
	json_object_object_add(tree, "revision",
	                       json_object_new_string(mod->revision ?
	                                              mod->revision : ""));

	if (xpath && strcmp(xpath, "/")) {
		node = lys_find_path(ly, NULL, xpath, 0);
		if (!node) {
			mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
			            "no schema node matches \"%s\"", xpath);
			json_object_put(tree);
			json_object_put(nodes);
			json_object_put(roots);
			sr_session_release_context(ctx->sess);
			return NULL;
		}
		json_object_object_add(roots, node->name,
		                       schema_node_to_json(node, with_desc, 0,
		                                           nodes, NULL));
	} else {
		/* Data nodes, RPCs and notifications are three separate lists
		 * in a compiled module; an agent needs all three. */
		for (node = mod->compiled->data; node; node = node->next)
			json_object_object_add(roots, node->name,
			                       schema_node_to_json(
			                               node, with_desc, 0,
			                               nodes, NULL));

		for (node = (const struct lysc_node *)mod->compiled->rpcs;
		     node; node = node->next)
			json_object_object_add(roots, node->name,
			                       schema_node_to_json(
			                               node, with_desc, 0,
			                               nodes, NULL));

		for (node = (const struct lysc_node *)mod->compiled->notifs;
		     node; node = node->next)
			json_object_object_add(roots, node->name,
			                       schema_node_to_json(
			                               node, with_desc, 0,
			                               nodes, NULL));
	}

	sr_session_release_context(ctx->sess);

	json_object_object_add(tree, "nodes", roots);

	res = json_object_new_object();
	json_object_object_add(res, "tree", tree);
	json_object_object_add(res, "nodes", nodes);
	json_object_object_add(res, "imports", json_object_new_array());

	return res;
}

/* Base type name of a compiled leaf type. */
static const char *
basetype_name(LY_DATA_TYPE type)
{
	switch (type) {
	case LY_TYPE_BINARY:      return "binary";
	case LY_TYPE_UINT8:       return "uint8";
	case LY_TYPE_UINT16:      return "uint16";
	case LY_TYPE_UINT32:      return "uint32";
	case LY_TYPE_UINT64:      return "uint64";
	case LY_TYPE_STRING:      return "string";
	case LY_TYPE_BITS:        return "bits";
	case LY_TYPE_BOOL:        return "boolean";
	case LY_TYPE_DEC64:       return "decimal64";
	case LY_TYPE_EMPTY:       return "empty";
	case LY_TYPE_ENUM:        return "enumeration";
	case LY_TYPE_IDENT:       return "identityref";
	case LY_TYPE_INST:        return "instance-identifier";
	case LY_TYPE_LEAFREF:     return "leafref";
	case LY_TYPE_UNION:       return "union";
	case LY_TYPE_INT8:        return "int8";
	case LY_TYPE_INT16:       return "int16";
	case LY_TYPE_INT32:       return "int32";
	case LY_TYPE_INT64:       return "int64";
	default:                  return "unknown";
	}
}

static struct json_object *
tool_get_help(struct tool_ctx *ctx, struct json_object *args,
              struct mcp_err *err)
{
	const char             *xpath = arg_xpath(args, err);
	const struct ly_ctx    *ly;
	const struct lysc_node *node;
	struct json_object     *res;

	if (!xpath)
		return NULL;

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	node = lys_find_path(ly, NULL, xpath, 0);
	if (!node) {
		mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
		            "no schema node matches \"%s\"", xpath);
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "xpath", json_object_new_string(xpath));
	json_object_object_add(res, "node_type",
	                       nodetype_to_json(node->nodetype));
	json_object_object_add(res, "mandatory",
	                       json_object_new_boolean(
	                               (node->flags & LYS_MAND_TRUE) ? 1 : 0));
	json_object_object_add(res, "config",
	                       json_object_new_boolean(
	                               (node->flags & LYS_CONFIG_W) ? 1 : 0));

	if (node->dsc)
		json_object_object_add(res, "description",
		                       json_object_new_string(node->dsc));
	if (node->ref)
		json_object_object_add(res, "reference",
		                       json_object_new_string(node->ref));
	if (node->module) {
		json_object_object_add(res, "module",
		                       json_object_new_string(
		                               node->module->name));
		if (node->module->ns)
			json_object_object_add(res, "namespace",
			                       json_object_new_string(
			                               node->module->ns));
	}

	if (node->nodetype & (LYS_LEAF | LYS_LEAFLIST)) {
		const struct lysc_node_leaf *leaf =
		        (const struct lysc_node_leaf *)node;

		if (leaf->type) {
			json_object_object_add(res, "base_type",
			                       json_object_new_string(
			                               basetype_name(
			                                       leaf->type->basetype)));

			if (leaf->type->basetype == LY_TYPE_ENUM) {
				const struct lysc_type_enum *enums =
				        (const struct lysc_type_enum *)leaf->type;
				struct json_object *values =
				        json_object_new_array();
				LY_ARRAY_COUNT_TYPE i;

				LY_ARRAY_FOR(enums->enums, i) {
					json_object_array_add(
					        values,
					        json_object_new_string(
					                enums->enums[i].name));
				}

				json_object_object_add(res, "values", values);
			}
		}

		if (leaf->units)
			json_object_object_add(res, "units",
			                       json_object_new_string(
			                               leaf->units));
	}

	/* TODO: range, length and pattern restrictions, and the default
	 * value. They live behind LY_ARRAY-encoded lysc_range structures; see
	 * sphinx/todo.rst. */

	sr_session_release_context(ctx->sess);

	return res;
}

/******************************************************************************
 * Tool catalogue
 *
 * The description and the input schema are what tools/list returns, and are
 * how an agent learns to call a tool. They are part of the interface, not
 * decoration.
 ******************************************************************************/

struct tool_desc {
	const char *name;
	const char *description;
	const char *schema;             /* JSON Schema, as a literal */
	tool_fn     handler;
	int         needs_session;      /* needs a per-request sysrepo session */
};

static const struct tool_desc tools[] = {
	{
		"get_status",
		"Return server health: version, uptime and session counters.",
		"{\"type\":\"object\",\"properties\":{"
		"\"verbose\":{\"type\":\"boolean\",\"default\":false}}}",
		tool_get_status, 0
	},
	{
		"sr_get_config",
		"Read configuration data from a sysrepo datastore.",
		"{\"type\":\"object\",\"properties\":{"
		"\"xpath\":{\"type\":\"string\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\"},"
		"\"max_depth\":{\"type\":\"integer\",\"minimum\":0,"
		"\"default\":0}},\"required\":[\"xpath\"]}",
		tool_sr_get_config, 1
	},
	{
		"sr_edit_config",
		"Apply a configuration change and commit it.",
		"{\"type\":\"object\",\"properties\":{"
		"\"config\":{\"type\":\"object\"},"
		"\"datastore\":{\"type\":\"string\","
		"\"enum\":[\"running\",\"startup\",\"candidate\"],"
		"\"default\":\"running\"},"
		"\"operation\":{\"type\":\"string\","
		"\"enum\":[\"merge\",\"replace\",\"none\"],"
		"\"default\":\"merge\"}},\"required\":[\"config\"]}",
		tool_sr_edit_config, 1
	},
	{
		"sr_delete_config",
		"Delete the configuration data selected by an XPath.",
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
		"Return the notifications received since the last poll.",
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
};

#define TOOL_COUNT (sizeof(tools) / sizeof(tools[0]))

static const struct tool_desc *
tool_find(const char *name)
{
	size_t i;

	for (i = 0; i < TOOL_COUNT; i++) {
		if (!strcmp(tools[i].name, name))
			return &tools[i];
	}

	return NULL;
}

/******************************************************************************
 * HTTP and JSON-RPC plumbing
 ******************************************************************************/

static void
http_send(FCGX_Request *req, int status, const char *extra_headers,
          const char *body)
{
	size_t len = body ? strlen(body) : 0;

	FCGX_FPrintF(req->out,
	             "Status: %d\r\n"
	             "Content-Type: application/json\r\n"
	             "Content-Length: %lu\r\n"
	             "Cache-Control: no-store\r\n"
	             "%s"
	             "\r\n",
	             status, (unsigned long)len,
	             extra_headers ? extra_headers : "");

	if (len)
		FCGX_PutStr(body, (int)len, req->out);
}

/*
 * Serialise and send a JSON-RPC envelope.
 *
 * The string returned by json_object_to_json_string_ext() belongs to the
 * json_object and dies with it: it must not be freed, and must be used before
 * json_object_put().
 */
static void
rpc_send(FCGX_Request *req, int status, const char *extra_headers,
         struct json_object *envelope)
{
	const char *text = json_object_to_json_string_ext(
	                           envelope, JSON_C_TO_STRING_PLAIN);

	http_send(req, status, extra_headers, text ? text : "{}");
	json_object_put(envelope);
}

/* Build the common envelope. id is borrowed and retained here. */
static struct json_object *
rpc_envelope(struct json_object *id)
{
	struct json_object *env = json_object_new_object();

	json_object_object_add(env, "jsonrpc", json_object_new_string("2.0"));
	json_object_object_add(env, "id", id ? json_object_get(id) : NULL);

	return env;
}

static void
rpc_send_result(FCGX_Request *req, const char *extra_headers,
                struct json_object *id, struct json_object *result)
{
	struct json_object *env = rpc_envelope(id);

	/* json_object_object_add() takes ownership of result; the caller must
	 * not put it afterwards. */
	json_object_object_add(env, "result", result);
	rpc_send(req, 200, extra_headers, env);
}

static void
rpc_send_error(FCGX_Request *req, int status, struct json_object *id,
               const struct mcp_err *err)
{
	struct json_object *env = rpc_envelope(id);
	struct json_object *obj = json_object_new_object();
	struct json_object *data = NULL;

	json_object_object_add(obj, "code", json_object_new_int(err->code));
	json_object_object_add(obj, "message",
	                       json_object_new_string(err->message[0] ?
	                                              err->message :
	                                              "Server error"));

	if (err->detail[0]) {
		data = json_object_new_object();
		json_object_object_add(data, "detail",
		                       json_object_new_string(err->detail));
	}
	if (err->sr_code != SR_ERR_OK) {
		if (!data)
			data = json_object_new_object();
		json_object_object_add(data, "sysrepo",
		                       json_object_new_string(
		                               sr_strerror(err->sr_code)));
	}
	if (data)
		json_object_object_add(obj, "data", data);

	json_object_object_add(env, "error", obj);
	rpc_send(req, status, NULL, env);
}

static void
rpc_fail(FCGX_Request *req, struct json_object *id, int code,
         const char *message, const char *detail)
{
	struct mcp_err err;

	mcp_err_clear(&err);
	mcp_err_set(&err, code, message, "%s", detail ? detail : "");
	rpc_send_error(req, 200, id, &err);
}

/******************************************************************************
 * MCP methods
 ******************************************************************************/

static void
method_initialize(FCGX_Request *req, struct json_object *id)
{
	struct json_object *result;
	struct json_object *caps;
	struct json_object *tools_cap;
	struct json_object *info;
	struct mcp_session *sess;
	char                header[128];

	sess = session_create();
	if (!sess) {
		struct mcp_err err;

		mcp_err_clear(&err);
		mcp_err_set(&err, MCP_ERR_SERVER, "Server error",
		            "the maximum of %d concurrent sessions is reached",
		            MCP_MAX_SESSIONS);
		rpc_send_error(req, 503, id, &err);
		return;
	}

	result = json_object_new_object();
	caps = json_object_new_object();
	tools_cap = json_object_new_object();
	info = json_object_new_object();

	json_object_object_add(tools_cap, "listChanged",
	                       json_object_new_boolean(0));
	json_object_object_add(caps, "tools", tools_cap);

	json_object_object_add(info, "name",
	                       json_object_new_string(PACKAGE_NAME));
	json_object_object_add(info, "version",
	                       json_object_new_string(PACKAGE_VERSION));

	json_object_object_add(result, "protocolVersion",
	                       json_object_new_string(MCP_PROTOCOL_VERSION));
	json_object_object_add(result, "capabilities", caps);
	json_object_object_add(result, "serverInfo", info);

	/* The identifier travels in the header, not in the body: that is where
	 * the Streamable HTTP binding puts it, and where a client looks. */
	snprintf(header, sizeof(header), "Mcp-Session-Id: %s\r\n", sess->id);

	fprintf(stderr, PACKAGE_NAME ": session %s created\n", sess->id);

	rpc_send_result(req, header, id, result);
}

static void
method_tools_list(FCGX_Request *req, struct json_object *id)
{
	struct json_object *result = json_object_new_object();
	struct json_object *list = json_object_new_array();
	size_t              i;

	for (i = 0; i < TOOL_COUNT; i++) {
		struct json_object *entry = json_object_new_object();
		struct json_object *schema =
		        json_tokener_parse(tools[i].schema);

		json_object_object_add(entry, "name",
		                       json_object_new_string(tools[i].name));
		json_object_object_add(entry, "description",
		                       json_object_new_string(
		                               tools[i].description));
		json_object_object_add(entry, "inputSchema",
		                       schema ? schema :
		                       json_object_new_object());
		json_object_array_add(list, entry);
	}

	json_object_object_add(result, "tools", list);
	rpc_send_result(req, NULL, id, result);
}

/*
 * Wrap a tool payload in the MCP content envelope.
 *
 * An MCP client expects result.content, an array of content blocks, not the
 * raw payload. A tool-level failure is reported with isError, as opposed to a
 * protocol-level JSON-RPC error.
 */
static struct json_object *
tool_content(struct json_object *payload, int is_error)
{
	struct json_object *result = json_object_new_object();
	struct json_object *content = json_object_new_array();
	struct json_object *block = json_object_new_object();
	const char         *text = json_object_to_json_string_ext(
	                                   payload, JSON_C_TO_STRING_PRETTY);

	json_object_object_add(block, "type", json_object_new_string("text"));
	json_object_object_add(block, "text",
	                       json_object_new_string(text ? text : "{}"));
	json_object_array_add(content, block);

	json_object_object_add(result, "content", content);
	json_object_object_add(result, "structuredContent",
	                       json_object_get(payload));
	json_object_object_add(result, "isError",
	                       json_object_new_boolean(is_error));

	json_object_put(payload);

	return result;
}

static void
method_tools_call(FCGX_Request *req, struct json_object *id,
                  struct json_object *params, struct mcp_session *mcp)
{
	const struct tool_desc *desc;
	struct json_object     *name_obj;
	struct json_object     *args = NULL;
	struct json_object     *payload;
	struct tool_ctx         ctx = { NULL, NULL };
	struct mcp_err          err;
	const char             *name;
	int                     rc;

	mcp_err_clear(&err);
	ctx.mcp = mcp;

	if (!params ||
	    !json_object_object_get_ex(params, "name", &name_obj) ||
	    !json_object_is_type(name_obj, json_type_string)) {
		rpc_fail(req, id, MCP_ERR_PARAMS, "Invalid params",
		         "params.name is required");
		return;
	}

	name = json_object_get_string(name_obj);
	desc = tool_find(name);
	if (!desc) {
		rpc_fail(req, id, MCP_ERR_METHOD, "Method not found",
		         "unknown tool");
		return;
	}

	/* arguments is optional: a tool may take none. */
	json_object_object_get_ex(params, "arguments", &args);
	if (args && !json_object_is_type(args, json_type_object)) {
		rpc_fail(req, id, MCP_ERR_PARAMS, "Invalid params",
		         "params.arguments must be an object");
		return;
	}

	if (desc->needs_session) {
		rc = sr_session_start(g_conn, SR_DS_RUNNING, &ctx.sess);
		if (rc != SR_ERR_OK) {
			mcp_err_from_session(&err, NULL, rc,
			                     "sr_session_start");
			rpc_send_error(req, 503, id, &err);
			return;
		}
	}

	payload = desc->handler(&ctx, args, &err);

	if (ctx.sess)
		sr_session_stop(ctx.sess);

	if (!payload) {
		if (!err.code)
			mcp_err_set(&err, MCP_ERR_INTERNAL, "Internal error",
			            "tool returned no result and no error");
		rpc_send_error(req, 200, id, &err);
		return;
	}

	rpc_send_result(req, NULL, id, tool_content(payload, 0));
}

/******************************************************************************
 * Request dispatch
 ******************************************************************************/

static void
dispatch(FCGX_Request *req, const char *body, size_t len,
         struct mcp_session *mcp)
{
	struct json_tokener *tok;
	struct json_object  *root;
	struct json_object  *id = NULL;
	struct json_object  *method_obj;
	struct json_object  *params = NULL;
	const char          *method;
	int                  has_id;

	tok = json_tokener_new();
	if (!tok) {
		rpc_fail(req, NULL, MCP_ERR_INTERNAL, "Internal error",
		         "out of memory");
		return;
	}

	root = json_tokener_parse_ex(tok, body, (int)len);
	if (!root || json_tokener_get_error(tok) != json_tokener_success) {
		json_tokener_free(tok);
		if (root)
			json_object_put(root);
		rpc_fail(req, NULL, MCP_ERR_PARSE, "Parse error",
		         "request body is not valid JSON");
		return;
	}
	json_tokener_free(tok);

	if (!json_object_is_type(root, json_type_object)) {
		json_object_put(root);
		rpc_fail(req, NULL, MCP_ERR_INVALID_REQUEST, "Invalid request",
		         "a JSON-RPC request must be an object");
		return;
	}

	has_id = json_object_object_get_ex(root, "id", &id);

	if (!json_object_object_get_ex(root, "method", &method_obj) ||
	    !json_object_is_type(method_obj, json_type_string)) {
		rpc_fail(req, has_id ? id : NULL, MCP_ERR_INVALID_REQUEST,
		         "Invalid request", "method is missing");
		json_object_put(root);
		return;
	}

	method = json_object_get_string(method_obj);
	json_object_object_get_ex(root, "params", &params);

	/*
	 * A JSON-RPC notification has no id and must never be answered with a
	 * JSON-RPC response. Acknowledge the HTTP request and stop there.
	 */
	if (!has_id) {
		http_send(req, 202, NULL, NULL);
		json_object_put(root);
		return;
	}

	if (!strcmp(method, "initialize"))
		method_initialize(req, id);
	else if (!strcmp(method, "tools/list"))
		method_tools_list(req, id);
	else if (!strcmp(method, "tools/call"))
		method_tools_call(req, id, params, mcp);
	else if (!strcmp(method, "ping"))
		rpc_send_result(req, NULL, id, json_object_new_object());
	else
		rpc_fail(req, id, MCP_ERR_METHOD, "Method not found", method);

	json_object_put(root);
}

/* Read the request body. Returns 0 on success, or an HTTP status on failure. */
static int
read_body(FCGX_Request *req, char **body, size_t *len)
{
	const char *value;
	long        content_length;
	char       *buf;
	int         got;

	*body = NULL;
	*len = 0;

	value = FCGX_GetParam("CONTENT_TYPE", req->envp);
	if (!value || !strstr(value, "application/json"))
		return 415;

	value = FCGX_GetParam("CONTENT_LENGTH", req->envp);
	if (!value)
		return 400;

	errno = 0;
	content_length = strtol(value, NULL, 10);
	if (errno || content_length <= 0)
		return 400;
	if (content_length > MCP_MAX_BODY)
		return 413;

	buf = malloc((size_t)content_length + 1);
	if (!buf)
		return 503;

	got = FCGX_GetStr(buf, (int)content_length, req->in);
	if (got < 0) {
		free(buf);
		return 400;
	}
	buf[got] = '\0';

	*body = buf;
	*len = (size_t)got;

	return 0;
}

static void
send_plain_error(FCGX_Request *req, int status, int code, const char *message)
{
	struct json_object *env = rpc_envelope(NULL);
	struct json_object *obj = json_object_new_object();

	json_object_object_add(obj, "code", json_object_new_int(code));
	json_object_object_add(obj, "message",
	                       json_object_new_string(message));
	json_object_object_add(env, "error", obj);

	rpc_send(req, status, NULL, env);
}

static void
serve(FCGX_Request *req)
{
	const char         *method = FCGX_GetParam("REQUEST_METHOD", req->envp);
	const char         *sid = FCGX_GetParam("HTTP_MCP_SESSION_ID",
	                                        req->envp);
	struct mcp_session *mcp = NULL;
	char               *body;
	size_t              len;
	int                 status;

	/* Housekeeping, once per request: retire what has timed out, then run
	 * the notification callbacks that arrived since the last request. */
	sessions_expire();
	sessions_process_events();

	/*
	 * A supplied session identifier must resolve. HTTP 404 is what the
	 * Streamable HTTP binding uses to tell a client its session is gone
	 * and that it should call initialize again.
	 */
	if (sid && *sid) {
		mcp = session_find(sid);
		if (!mcp) {
			send_plain_error(req, 404, MCP_ERR_NO_SESSION,
			                 "Unknown or expired Mcp-Session-Id; "
			                 "call initialize again");
			return;
		}
		mcp->last_activity = time(NULL);
	}

	/* DELETE terminates a session explicitly, rather than waiting for the
	 * idle timeout to free its subscriptions. */
	if (method && !strcmp(method, "DELETE")) {
		if (!mcp) {
			send_plain_error(req, 404, MCP_ERR_NO_SESSION,
			                 "DELETE needs a valid Mcp-Session-Id");
			return;
		}

		fprintf(stderr, PACKAGE_NAME ": session %s deleted\n", mcp->id);
		session_destroy(mcp);
		FCGX_FPrintF(req->out, "Status: 204\r\n\r\n");
		return;
	}

	if (!method || strcmp(method, "POST")) {
		FCGX_FPrintF(req->out,
		             "Status: 405\r\nAllow: POST, DELETE\r\n\r\n");
		return;
	}

	status = read_body(req, &body, &len);

	switch (status) {
	case 0:
		break;
	case 413:
		send_plain_error(req, 413, MCP_ERR_INVALID_REQUEST,
		                 "Request body too large");
		return;
	case 415:
		send_plain_error(req, 415, MCP_ERR_INVALID_REQUEST,
		                 "Content-Type must be application/json");
		return;
	case 503:
		send_plain_error(req, 503, MCP_ERR_INTERNAL, "Out of memory");
		return;
	default:
		send_plain_error(req, 400, MCP_ERR_INVALID_REQUEST,
		                 "Malformed request");
		return;
	}

	dispatch(req, body, len, mcp);
	free(body);
}

/******************************************************************************
 * Entry point
 ******************************************************************************/

static volatile sig_atomic_t stopping;

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
	        PACKAGE_NAME " " PACKAGE_VERSION
	        " - MCP server for the sysrepo datastore\n"
	        "\n"
	        "Usage: " PACKAGE_NAME " [--help] [--version]\n"
	        "\n"
	        "  --help     print this message and exit\n"
	        "  --version  print the version and exit\n"
	        "\n"
	        "With no argument, " PACKAGE_NAME " expects to be started as a\n"
	        "FastCGI application by a web server. Sessions are held in this\n"
	        "process, so the FastCGI configuration must use max-procs = 1.\n");
}

int
main(int argc, char *argv[])
{
	FCGX_Request     req;
	struct sigaction sa;
	size_t           i;
	int              arg;

	for (arg = 1; arg < argc; arg++) {
		if (!strcmp(argv[arg], "--help") || !strcmp(argv[arg], "-h")) {
			usage(stdout);
			return EXIT_SUCCESS;
		}
		if (!strcmp(argv[arg], "--version")) {
			printf(PACKAGE_NAME " " PACKAGE_VERSION "\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, PACKAGE_NAME ": unknown option \"%s\"\n",
		        argv[arg]);
		usage(stderr);
		return EXIT_FAILURE;
	}

	if (FCGX_Init()) {
		fprintf(stderr, PACKAGE_NAME ": FCGX_Init failed\n");
		return EXIT_FAILURE;
	}

	/*
	 * FCGX_IsCGI() is the only correct test. Probing getenv() cannot work:
	 * under FastCGI the request parameters arrive per request in
	 * req.envp, never in the process environment.
	 */
	if (FCGX_IsCGI()) {
		fprintf(stderr,
		        PACKAGE_NAME ": not started as a FastCGI application.\n"
		        "Run it behind a reverse proxy, or use --help.\n");
		return EXIT_FAILURE;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) {
		fprintf(stderr, PACKAGE_NAME ": sigaction: %s\n",
		        strerror(errno));
		return EXIT_FAILURE;
	}
	signal(SIGPIPE, SIG_IGN);

	if (sysrepo_open() != SR_ERR_OK)
		return EXIT_FAILURE;

	g_start_time = time(NULL);

	if (FCGX_InitRequest(&req, 0, 0)) {
		fprintf(stderr, PACKAGE_NAME ": FCGX_InitRequest failed\n");
		sysrepo_close();
		return EXIT_FAILURE;
	}

	fprintf(stderr, PACKAGE_NAME " " PACKAGE_VERSION ": ready\n");

	while (!stopping && FCGX_Accept_r(&req) >= 0) {
		serve(&req);
		FCGX_Finish_r(&req);
	}

	for (i = 0; i < MCP_MAX_SESSIONS; i++)
		session_destroy(&g_sessions[i]);

	FCGX_Free(&req, 1);
	sysrepo_close();

	fprintf(stderr, PACKAGE_NAME ": stopped\n");

	return EXIT_SUCCESS;
}
