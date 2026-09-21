/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Cross-cutting helpers shared across all tool modules: mcp_err,
 * mcp_code_from_sr, tree_to_json, nodetype_to_json, argument extraction,
 * tool catalogue, and the MCP content envelope.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>

/* ----------------------------------------------------------------------- mcp_err
 */

static void
mcp_err_clear(struct mcp_err *err)
{
	err->code = 0;
	err->message[0] = '\0';
	err->detail[0] = '\0';
	err->sr_code = SR_ERR_OK;
}

void
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
int
mcp_code_from_sr(int rc)
{
	switch (rc) {
	case SR_ERR_NOT_FOUND:
		return MCP_ERR_NOT_FOUND;
	case SR_ERR_INVAL_ARG:
		/* An invalid argument to sysrepo can mean two things: the
		 * caller passed wrong-typed arguments to the API (MCP_ERR_PARAMS),
		 * or the data they submitted violates a schema constraint
		 * (MCP_ERR_VALIDATION).  Leave the base mapping as validation;
		 * mcp_err_from_session() restores MCP_ERR_PARAMS when the
		 * message says "expected" (API misuse) rather than
		 * "does not satisfy" (data validation). */
		return MCP_ERR_VALIDATION;
	case SR_ERR_LY:
		/* libyang can return SR_ERR_LY for two distinct reasons:
		 * "cannot resolve" (the path does not exist in the schema)
		 * which is MCP_ERR_NOT_FOUND, and "does not satisfy" (range,
		 * type, mandatory leaf) which is MCP_ERR_VALIDATION.  Leave
		 * the base mapping as MCP_ERR_VALIDATION; mcp_err_from_session()
		 * restores MCP_ERR_NOT_FOUND when the message says so. */
		return MCP_ERR_VALIDATION;
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

/* ----------------------------------------------------------------- tool_find
 *
 * Look up a tool by name (linear search).
 *
 * Defined in utilities.c because the header declares it and because it
 * references `tools` and `TOOL_COUNT` which are declared extern in the
 * header but defined in main.c.
 */

const struct tool_desc *
tool_find(const char *name)
{
	size_t i;

	for (i = 0; i < TOOL_COUNT; i++) {
		if (!strcmp(tools[i].name, name))
			return &tools[i];
	}

	return NULL;
}

/* ---------------------------------------------------------------- mcp_err_from_session
 *
 * sr_strerror() only names the code; sr_session_get_error() carries the
 * message that says which node was rejected and why, which is the part an
 * agent needs.
 */

void
mcp_err_from_session(struct mcp_err *err, sr_session_ctx_t *sess, int rc,
		     const char *what)
{
	const sr_error_info_t *info = NULL;
	const char	    *msg = NULL;

	if (sess && (sr_session_get_error(sess, &info) == SR_ERR_OK) &&
	    info && info->err_count && info->err[0].message) {
		msg = info->err[0].message;
	}

	err->code = mcp_code_from_sr(rc);
	err->sr_code = rc;
	snprintf(err->message, sizeof(err->message), "%s", sr_strerror(rc));
	snprintf(err->detail, sizeof(err->detail), "%s: %s", what,
		 msg ? msg : sr_strerror(rc));

	/* "cannot resolve" means the target path does not exist in the schema;
	 * the original MCP_ERR_NOT_FOUND set by the caller is correct but gets
	 * overwritten by mcp_code_from_sr(SR_ERR_LY).  Restore it. */
	if (msg != NULL && strstr(msg, "cannot resolve") != NULL)
		err->code = MCP_ERR_NOT_FOUND;

	/* "Expected" (a value was expected) or "Unexpected" (malformed input)
	 * means the API call itself was wrong, not the data.  Restore
	 * MCP_ERR_PARAMS. */
	if (msg != NULL &&
	    (strstr(msg, "Expected") != NULL || strstr(msg, "Unexpected") != NULL))
		err->code = MCP_ERR_PARAMS;
}

/* ----------------------------------------------------------------- tree_to_json
 */

/*
 * Serialise a libyang tree into a json-c object.
 *
 * A NULL tree is an empty result, not a failure: an XPath matching nothing is
 * a legitimate answer.
 */
struct json_object *
tree_to_json(const struct lyd_node *tree, struct mcp_err *err)
{
	struct json_object *obj;
	char *text = NULL;

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

/* ---------------------------------------------------------------- nodetype_to_json
 */

/*
 * Node type name, lowercased.
 *
 * lys_nodetype2str() spells a few of them in upper case ("RPC"), which would
 * leak an inconsistency into the API: an agent matching on "rpc" would miss
 * them. One spelling, everywhere.
 */
struct json_object *
nodetype_to_json(uint16_t nodetype)
{
	const char *name = lys_nodetype2str(nodetype);
	char lower[32];
	size_t i;

	if (!name)
		return json_object_new_string("unknown");

	for (i = 0; name[i] && i < sizeof(lower) - 1; i++) {
		lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ?
		           (char)(name[i] - 'A' + 'a') : name[i];
	}
	lower[i] = '\0';

	return json_object_new_string(lower);
}

/* ------------------------------------------------------------------ arg_string
 */

const char *
arg_string(struct json_object *args, const char *name)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return NULL;
	if (!json_object_is_type(val, json_type_string))
		return NULL;

	return json_object_get_string(val);
}

/* ------------------------------------------------------------------- arg_object
 */

struct json_object *
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

/*  --------------------------------------------------------------------- arg_int
 */

int
arg_int(struct json_object *args, const char *name, int fallback)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return fallback;
	if (!json_object_is_type(val, json_type_int))
		return fallback;

	return json_object_get_int(val);
}

/* ------------------------------------------------------------------- arg_int64
 */

int64_t
arg_int64(struct json_object *args, const char *name, int64_t fallback)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return fallback;
	if (!json_object_is_type(val, json_type_int))
		return fallback;

	return json_object_get_int64(val);
}

/*  --------------------------------------------------------------------- arg_bool
 */

int
arg_bool(struct json_object *args, const char *name, int fallback)
{
	struct json_object *val;

	if (!args || !json_object_object_get_ex(args, name, &val))
		return fallback;
	if (!json_object_is_type(val, json_type_boolean))
		return fallback;

	return json_object_get_boolean(val) ? 1 : 0;
}

/* ----------------------------------------------------------------- xpath_wellformed
 */

/*
 * Reject a path that cannot possibly address YANG data.
 *
 * Handed a relative or unprefixed path, sysrepo either matches nothing or
 * fails with a libyang error. Neither tells the agent the one thing it needs
 * to know, which is that its path was malformed rather than its data absent.
 * The contract is an absolute path whose first segment carries a module
 * prefix, so that is what is checked.
 */
int
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

/* ---------------------------------------------------------------------- arg_xpath
 */

/* Fetch and validate a required xpath argument. Returns NULL on failure. */
const char *
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

/* ------------------------------------------------------------------ arg_datastore
 */

/* Resolve a datastore name. Returns 0 on success. */
int
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

/* ---------------------------------------------------------------- tool_content
 */

/*
 * Wrap a tool payload in the MCP content envelope.
 *
 * An MCP client expects result.content, an array of content blocks, not the
 * raw payload. A tool-level failure is reported with isError, as opposed to a
 * protocol-level JSON-RPC error.
 */
struct json_object *
tool_content(struct json_object *payload, int is_error)
{
	struct json_object *result = json_object_new_object();
	struct json_object *content = json_object_new_array();
	struct json_object *block = json_object_new_object();
	const char *text = json_object_to_json_string_ext(
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
