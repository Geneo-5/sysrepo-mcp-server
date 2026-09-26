/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * HTTP/RPC plumbing, MCP methods, request dispatch, FastCGI loop.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <json-c/json.h>
#include <curl/curl.h>

#include <sysrepo.h>
#include <sysrepo/netconf_acm.h>
#include <fcgiapp.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/sessions.h>
#include <sysrepo/mcp/transport.h>
#include <sysrepo/mcp/libconfig.h>
#include <sysrepo/mcp/log.h>

/* External state from main.c. */

/* ------------------------------------------------------------------- http_send
 *
 * Write an HTTP response with a JSON content type and the given body. The
 * body length is computed once for the status line and the body itself.
 */

void
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

	mcp_err_set(&err, code, message, "%s", detail ? detail : "");
	rpc_send_error(req, 200, id, &err);
}

static void
modern_fail(FCGX_Request *req, int status, struct json_object *id,
	    int code, const char *message, const char *field,
	    const char *value)
{
	struct json_object *env = rpc_envelope(id);
	struct json_object *error = json_object_new_object();
	struct json_object *data = json_object_new_object();

	json_object_object_add(error, "code", json_object_new_int(code));
	json_object_object_add(error, "message", json_object_new_string(message));
	if (field && value)
		json_object_object_add(data, field, json_object_new_string(value));
	if (code == -32022) {
		struct json_object *versions = json_object_new_array();
		json_object_array_add(versions,
		                      json_object_new_string(MCP_PROTOCOL_VERSION));
		json_object_array_add(versions,
		                      json_object_new_string(
	                              MCP_LEGACY_PROTOCOL_VERSION));
		json_object_object_add(data, "supported", versions);
	}
	if (json_object_object_length(data))
		json_object_object_add(error, "data", data);
	else
		json_object_put(data);
	json_object_object_add(env, "error", error);
	rpc_send(req, status, NULL, env);
}

static void
method_server_discover(FCGX_Request *req, struct json_object *id)
{
	struct json_object *result = json_object_new_object();
	struct json_object *versions = json_object_new_array();
	struct json_object *capabilities = json_object_new_object();
	struct json_object *tools_cap = json_object_new_object();
	struct json_object *meta = json_object_new_object();
	struct json_object *info = json_object_new_object();

	json_object_array_add(versions,
	                      json_object_new_string(MCP_PROTOCOL_VERSION));
	json_object_array_add(versions,
	                      json_object_new_string(MCP_LEGACY_PROTOCOL_VERSION));
	json_object_object_add(result, "resultType",
	                       json_object_new_string("complete"));
	json_object_object_add(result, "ttlMs", json_object_new_int(0));
	json_object_object_add(result, "cacheScope",
	                       json_object_new_string("private"));
	json_object_object_add(result, "supportedVersions", versions);
	json_object_object_add(capabilities, "tools", tools_cap);
	json_object_object_add(result, "capabilities", capabilities);
	json_object_object_add(info, "name",
	                       json_object_new_string(CONFIG_PACKAGE_NAME));
	json_object_object_add(info, "version",
	                       json_object_new_string(CONFIG_PACKAGE_VERSION));
	json_object_object_add(meta, "io.modelcontextprotocol/serverInfo", info);
	json_object_object_add(result, "_meta", meta);
	rpc_send_result(req, NULL, id, result);
}

/******************************************************************************
 * MCP methods
 ******************************************************************************/

static void
method_initialize(FCGX_Request *req, struct json_object *id,
                  const char *user)
{
	struct json_object *result;
	struct json_object *caps;
	struct json_object *tools_cap;
	struct json_object *info;
	struct mcp_session *sess;
	char                header[128];
	const struct mcp_config *cfg = mcp_config_get();

	/* Auth: if authentication is on but no user was resolved, deny. */
	if (cfg->auth_method > 0 && !user) {
		struct mcp_err err;

		mcp_err_set(&err, MCP_ERR_INTERNAL, "Unauthorized",
			    "missing or invalid API key");
		rpc_send_error(req, 401, id, &err);
		return;
	}

	sess = session_create(user);
	if (!sess) {
		struct mcp_err err;

		mcp_err_set(&err, MCP_ERR_SERVER, "Server error",
		            "the maximum of %d concurrent sessions is reached",
		            mcp_config_get()->max_sessions);
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
	                       json_object_new_string(CONFIG_PACKAGE_NAME));
	json_object_object_add(info, "version",
	                       json_object_new_string(CONFIG_PACKAGE_VERSION));

	json_object_object_add(result, "protocolVersion",
	                       json_object_new_string(
	                               MCP_LEGACY_PROTOCOL_VERSION));
	json_object_object_add(result, "capabilities", caps);
	json_object_object_add(result, "serverInfo", info);

	/* The identifier travels in the header, not in the body: that is where
	 * the Streamable HTTP binding puts it, and where a client looks. */
	snprintf(header, sizeof(header), "Mcp-Session-Id: %s\r\n", sess->id);

	mcp_log_info("session %s created", sess->id);

	rpc_send_result(req, header, id, result);
}

static void
method_tools_list(FCGX_Request *req, struct json_object *id, int modern)
{
	struct json_object *result = json_object_new_object();
	struct json_object *list = json_object_new_array();
	size_t              i;

	for (i = 0; i < TOOL_COUNT; i++) {
		struct json_object *entry;
		if (modern && (!strcmp(tools[i].name, "sr_notif_subscribe") ||
		               !strcmp(tools[i].name, "sr_notif_unsubscribe") ||
		               !strcmp(tools[i].name, "sr_notif_list_subscriptions") ||
		               !strcmp(tools[i].name, "sr_notif_poll")))
			continue;
		entry = json_object_new_object();
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

	if (modern) {
		json_object_object_add(result, "resultType",
		                       json_object_new_string("complete"));
		/* Keep the catalogue scoped to this authorization context and
		 * immediately stale; this is conservative if per-user filtering is
		 * added later and avoids requiring list-change invalidation. */
		json_object_object_add(result, "ttlMs", json_object_new_int(0));
		json_object_object_add(result, "cacheScope",
		                       json_object_new_string("private"));
	}
	json_object_object_add(result, "tools", list);
	rpc_send_result(req, NULL, id, result);
}

static void
method_tools_call(FCGX_Request *req, struct json_object *id,
                  struct json_object *params, struct mcp_session *mcp,
                  const char *user, int modern)
{
	const struct tool_desc *desc;
	const struct mcp_config *cfg = mcp_config_get();
	struct json_object     *name_obj;
	struct json_object     *args = NULL;
	struct json_object     *write_target = NULL;
	struct json_object     *payload;
	struct tool_ctx         ctx = { NULL, NULL };
	struct mcp_err          err;
	const char             *name;
	int                     rc;
	const char             *effective_user = mcp ? mcp->user : user;

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
	if (modern && (!strcmp(name, "sr_notif_subscribe") ||
	               !strcmp(name, "sr_notif_unsubscribe") ||
	               !strcmp(name, "sr_notif_list_subscriptions") ||
	               !strcmp(name, "sr_notif_poll"))) {
		rpc_fail(req, id, MCP_ERR_METHOD, "Method not found",
		         "this session-scoped tool is unavailable in stateless MCP");
		return;
	}

	/* Block module install / uninstall when auth is on: these change the
	 * schema for the entire sysrepo instance and destroying a module wipes
	 * its data. */
	if (cfg->auth_method > 0 &&
	    (!strcmp(desc->name, "sr_module_install") ||
	     !strcmp(desc->name, "sr_module_uninstall"))) {
			mcp_err_set(&err, MCP_ERR_DENIED, "Not permitted",
			            "module install / uninstall is not permitted");
			rpc_send_error(req, 403, id, &err);
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
			mcp_err_from_session(&err, NULL, rc, "sr_session_start");
			rpc_send_error(req, 503, id, &err);
			return;
		}

		/* NACM : lier l'identité à la session via
		 * ``sr_nacm_set_user()`` ; le contrôle d'accès se fait dans
		 * ``rpc_common()`` en ``rpc.c`` avant ``sr_rpc_send_tree()``. */
		if (cfg->auth_method > 0 && effective_user) {
			rc = sr_nacm_set_user(ctx.sess, effective_user);
			if (rc != SR_ERR_OK) {
				mcp_err_from_session(&err, NULL, rc, "sr_nacm_set_user");
				rpc_send_error(req, 503, id, &err);
				return;
			}
		}
	}

	/* P0.8: log the identity of the requester with every operation. */
	mcp_log_info("%s: %s called by %s",
	            mcp ? mcp->id : "(no session)", desc->name,
	            effective_user ? effective_user : "(anonymous)");

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

	{
		struct json_object *result = tool_content(payload, 0);
		if (modern)
			json_object_object_add(result, "resultType",
			                       json_object_new_string("complete"));
		rpc_send_result(req, NULL, id, result);
	}
}

/******************************************************************************
 * Request dispatch
 ******************************************************************************/

void
dispatch(FCGX_Request *req, const char *body, size_t len,
         struct mcp_session *mcp, const char *user, int modern)
{
	struct json_tokener *tok;
	struct json_object  *root;
	struct json_object  *id = NULL;
	struct json_object  *method_obj;
	struct json_object  *params = NULL;
	struct json_object  *meta = NULL;
	struct json_object  *version_obj = NULL;
	struct json_object  *caps_obj = NULL;
	struct json_object  *client_obj = NULL;
	const char          *method;
	const char          *body_version;
	const char          *header_version;
	const char          *header_method;
	const char          *header_name;
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
	if (modern) {
		header_version = FCGX_GetParam("HTTP_MCP_PROTOCOL_VERSION",
		                               req->envp);
		header_method = FCGX_GetParam("HTTP_MCP_METHOD", req->envp);
		if (!params || !json_object_object_get_ex(params, "_meta", &meta) ||
		    !json_object_is_type(meta, json_type_object) ||
		    !json_object_object_get_ex(meta,
		        "io.modelcontextprotocol/protocolVersion", &version_obj) ||
		    !json_object_is_type(version_obj, json_type_string) ||
		    !json_object_object_get_ex(meta,
	        "io.modelcontextprotocol/clientCapabilities", &caps_obj) ||
		    !json_object_is_type(caps_obj, json_type_object) ||
		    !json_object_object_get_ex(meta,
		        "io.modelcontextprotocol/clientInfo", &client_obj) ||
		    !json_object_is_type(client_obj, json_type_object)) {
			modern_fail(req, 400, has_id ? id : NULL, -32020,
			            "Missing or malformed modern request metadata",
			            NULL, NULL);
			json_object_put(root);
			return;
		}
		body_version = json_object_get_string(version_obj);
		if (!header_version || strcmp(header_version, body_version) ||
		    !header_method || strcmp(header_method, method)) {
			modern_fail(req, 400, has_id ? id : NULL, -32020,
			            "HTTP headers do not match request metadata",
			            NULL, NULL);
			json_object_put(root);
			return;
		}
		if (strcmp(body_version, MCP_PROTOCOL_VERSION)) {
			modern_fail(req, 400, has_id ? id : NULL, -32022,
			            "Unsupported protocol version", "requested",
			            body_version);
			json_object_put(root);
			return;
		}
		if (!strcmp(method, "tools/call")) {
			struct json_object *tool_name;
			const char *body_name = NULL;
			header_name = FCGX_GetParam("HTTP_MCP_NAME", req->envp);
			if (params && json_object_object_get_ex(params, "name",
			                                        &tool_name) &&
			    json_object_is_type(tool_name, json_type_string))
				body_name = json_object_get_string(tool_name);
			if (!header_name || !body_name || strcmp(header_name, body_name)) {
				modern_fail(req, 400, has_id ? id : NULL, -32020,
				            "Mcp-Name does not match params.name", NULL, NULL);
				json_object_put(root);
				return;
			}
		}
	}

	/*
	 * A JSON-RPC notification has no id and must never be answered with a
	 * JSON-RPC response. Acknowledge the HTTP request and stop there.
	 */
	if (!has_id) {
		http_send(req, 202, NULL, NULL);
		json_object_put(root);
		return;
	}

	if (modern && !strcmp(method, "server/discover"))
		method_server_discover(req, id);
	else if (modern && !strcmp(method, "initialize"))
		modern_fail(req, 400, id, -32022,
		            "initialize is not part of the stateless protocol",
		            "requested", MCP_PROTOCOL_VERSION);
	else if (!strcmp(method, "initialize"))
		method_initialize(req, id, user);
	else if (!strcmp(method, "tools/list"))
		method_tools_list(req, id, modern);
	else if (!strcmp(method, "tools/call"))
		method_tools_call(req, id, params, mcp, user, modern);
	else if (!strcmp(method, "ping")) {
		struct json_object *result = json_object_new_object();
		if (modern)
			json_object_object_add(result, "resultType",
			                       json_object_new_string("complete"));
		rpc_send_result(req, NULL, id, result);
	}
	else if (modern) {
		struct mcp_err err;
		mcp_err_set(&err, MCP_ERR_METHOD, "Method not found", "%s", method);
		rpc_send_error(req, 404, id, &err);
	} else
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

static const char *
extract_credential(FCGX_Request *req)
{
	const struct mcp_config *cfg = mcp_config_get();
	const char              *auth_header;
	const char              *credential = NULL;

	/* 1. Authorization: Bearer <token> */
	auth_header = FCGX_GetParam("HTTP_AUTHORIZATION", req->envp);
	if (auth_header && strncmp(auth_header, "Bearer ", 7) == 0) {
		credential = auth_header + 7;
		/* Trim trailing whitespace / CRLF. */
		while (*credential &&
		       (*credential == ' ' || *credential == '\t' ||
		        *credential == '\r' || *credential == '\n'))
			credential++;
		char *end = strchr(credential, ' ');
		if (end)
			*end = '\0';
		end = strchr(credential, '\r');
		if (end)
			*end = '\0';
		end = strchr(credential, '\n');
		if (end)
			*end = '\0';
		return credential;
	}

	/* 2. Cookie <name>=<value> */
	if (cfg->auth_method == 2) { /* == "cookie" */

		const char *cookie_hdr =
			FCGX_GetParam("HTTP_COOKIE", req->envp);
		const char *needle = cfg->cookie_name;
		size_t      needle_len = strlen(needle);
		if (cookie_hdr) {
			const char *found = strstr(cookie_hdr, needle);
			if (found &&
			    (found[needle_len] == '=' ||
			     found[needle_len] == '%')) {
				found += needle_len + 1;
				char *end = strchr(found, '&');
				if (end)
					*end = '\0';
				end = strchr(found, ';');
				if (end)
					*end = '\0';
				/* URL-decode (simple case). */
				return found;
			}
		}
	}

	return NULL;
}

void
serve(FCGX_Request *req)
{
	const char         *method = FCGX_GetParam("REQUEST_METHOD", req->envp);
	const char         *sid = FCGX_GetParam("HTTP_MCP_SESSION_ID",
	                                        req->envp);
	const struct mcp_config *cfg = mcp_config_get();
	struct mcp_session     *mcp = NULL;
	char                   *body;
	size_t                  len;
	int                     status;
	const char             *credential = NULL;
	const char             *user = NULL;
	const char             *protocol_header = FCGX_GetParam(
	                                "HTTP_MCP_PROTOCOL_VERSION", req->envp);
	const char             *method_header = FCGX_GetParam(
	                                "HTTP_MCP_METHOD", req->envp);
	const char             *origin_header = FCGX_GetParam(
	                                "HTTP_ORIGIN", req->envp);
	int                     modern = protocol_header || method_header;

	/* Housekeeping, once per request: retire what has timed out, then run
	 * the notification callbacks that arrived since the last request. */
	sessions_expire();
	sessions_process_events();

	/* Extract the credential (Bearer token or cookie) before dispatch,
	 * so that method_initialize() can authenticate the new session. */
	credential = extract_credential(req);
	/* There is no browser-origin allow-list in this server. Deny requests
	 * carrying Origin by default; native MCP clients normally omit it. */
	if (origin_header && *origin_header) {
		send_plain_error(req, 403, MCP_ERR_DENIED,
		                 "Origin is not allowed");
		return;
	}
	if (modern) {
		if (cfg->auth_method > 0 && credential)
			user = mcp_config_find_key(cfg, credential);
		if (cfg->auth_method > 0 && !user) {
			struct mcp_err err;
			mcp_err_set(&err, MCP_ERR_INTERNAL, "Unauthorized",
			            "missing or invalid API key");
			rpc_send_error(req, 401, NULL, &err);
			return;
		}
		if (!method || strcmp(method, "POST")) {
			FCGX_FPrintF(req->out,
			             "Status: 405\r\nAllow: POST\r\n\r\n");
			return;
		}
		status = read_body(req, &body, &len);
		if (status) {
			send_plain_error(req, status, MCP_ERR_INVALID_REQUEST,
			                 status == 415 ? "Content-Type must be application/json" :
			                 "Malformed request");
			return;
		}
		dispatch(req, body, len, NULL, user, 1);
		free(body);
		return;
	}

	/*
	 * A supplied session identifier must resolve. HTTP 404 is what the
	 * Streamable HTTP binding uses to tell a client its session is gone
	 * and that it should call initialize again.
	 */
	if (sid && *sid) {
		mcp = session_find(sid);
		if (!mcp) {
			/* New credential on a dead session: allow re-initialize. */
			if (cfg->auth_method > 0 && credential)
				user = mcp_config_find_key(cfg, credential);
			send_plain_error(req, 404, MCP_ERR_NO_SESSION,
			                 "Unknown or expired Mcp-Session-Id; "
			                 "call initialize again");
			return;
		}
		mcp->last_activity = time(NULL);
		/* Session already authenticated during initialize; ignore
		 * re-sent credentials — the session user is authoritative. */
	} else {
		/* No session yet: a new session is being created (initialize).
		 * Look up the credential so that method_initialize() can store
		 * the user. */
		if (cfg->auth_method > 0 && credential)
			user = mcp_config_find_key(cfg, credential);
	}

	/* If Authentication is configured, deny every request without a valid
	 * credential — the server requires authentication. */
	if (cfg->auth_method > 0 && !sid && !mcp && !user) {
		struct mcp_err err;

		mcp_err_set(&err, MCP_ERR_INTERNAL, "Unauthorized",
		            "missing or invalid API key");
		rpc_send_error(req, 401, NULL, &err);
		return;
	}

	/* DELETE terminates a session explicitly, rather than waiting for the
	 * idle timeout to free its subscriptions. */
	if (method && !strcmp(method, "DELETE")) {
		if (!mcp) {
			send_plain_error(req, 404, MCP_ERR_NO_SESSION,
			                 "DELETE needs a valid Mcp-Session-Id");
			return;
		}

		mcp_log_info("session %s deleted", mcp->id);
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

	dispatch(req, body, len, mcp, user, 0);
	free(body);
}
