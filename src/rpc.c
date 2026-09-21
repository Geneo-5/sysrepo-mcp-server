/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * RPC and action tools: sr_execute_rpc, sr_action, rpc_common.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/rpc.h>

/*
 * Both sr_execute_rpc and sr_action share the same implementation.
 *
 * They build an operation node via lyd_new_path (NULL module — resolves
 * from the current session context), add input parameters as child nodes,
 * then call sr_rpc_send_tree.  An action is identical except the xpath
 * points to a data node rather than a global RPC.
 */

/* ---------------------------------------------------------------- rpc_common
 *
 * Build the operation tree, call sr_rpc_send_tree, extract and return the
 * result payload.
 */
struct json_object *
rpc_common(struct tool_ctx *ctx, struct json_object *args, struct mcp_err *err)
{
	const char          *xpath;
	struct json_object  *input;
	const struct ly_ctx *ly;
	struct lyd_node     *op = NULL;
	sr_data_t           *output = NULL;
	struct json_object  *res;
	struct json_object  *payload;
	int                  timeout = arg_int(args, "timeout_ms",
	                                       CONFIG_SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS);
	int                  rc;

	xpath = arg_xpath(args, err);
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
			    ly_err_last(ly));
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	if (input && json_object_is_type(input, json_type_object)) {
		json_object_object_foreach(input, key, val) {
			const char *text = json_object_get_string(val);

			if (lyd_new_path(op, NULL, key, text, 0, NULL) !=
			    LY_SUCCESS) {
				mcp_err_set(err, MCP_ERR_PARAMS,
					    "Invalid params",
					    "cannot add input parameter \"%s\": "
					    "%s", key, ly_err_last(ly));
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
		mcp_err_from_session(err, ctx->sess, rc, "sr_rpc_send");
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

/* ----------------------------------------------------------- sr_execute_rpc
 *
 * Execute a NETCONF RPC.  The input must contain an "input" object whose
 * keys become children of the RPC operation node.
 */
struct json_object *
tool_sr_execute_rpc(struct tool_ctx *ctx, struct json_object *args,
                    struct mcp_err *err)
{
	return rpc_common(ctx, args, err);
}

/* ------------------------------------------------------------------- sr_action
 *
 * Execute a YANG action.  Actions use the same call path as RPCs; the
 * difference is purely semantic: an action's xpath points to a data node
 * rather than a global operation.
 */
struct json_object *
tool_sr_action(struct tool_ctx *ctx, struct json_object *args,
               struct mcp_err *err)
{
	return rpc_common(ctx, args, err);
}
