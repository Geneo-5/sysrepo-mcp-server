/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Configuration datastore tools: sr_get_config, sr_edit_config,
 * sr_delete_config.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/config_tools.h>

/* ---------------------------------------------------------------- sr_get_config
 */
struct json_object *
tool_sr_get_config(struct tool_ctx *ctx, struct json_object *args,
                   struct mcp_err *err)
{
	const char         *xpath = arg_xpath(args, err);
	const char         *ds_obj;
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
	                 CONFIG_SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS, 0, &data);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		return NULL;
	}

	payload = tree_to_json(data ? data->tree : NULL, err);
	sr_release_data(data);

	if (!payload)
		return NULL;

	ds_obj = arg_string(args, "datastore");

	res = json_object_new_object();
	json_object_object_add(res, "data", payload);
	json_object_object_add(res, "xpath", json_object_new_string(xpath));
	json_object_object_add(res, "datastore",
	                       json_object_new_string(ds_obj ? ds_obj :
		                                              "running"));

	return res;
}

/* ----------------------------------------------------------- sr_edit_config
 *
 * Parse the config tree, stage it, commit it. sr_set_item_str() is not usable
 * here: it sets a single node from its string value and cannot be handed a
 * serialised JSON document.
 */
struct json_object *
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
			    ly_err_last(ly));
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

	rc = sr_apply_changes(ctx->sess, CONFIG_SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS);
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
struct json_object *
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
	 * argument is there for an agent that needs to know.
	 *
	 * sr_delete_item with SR_EDIT_STRICT does not fail for an absent node
	 * in sysrepo 5.x, so we check existence ourselves. */
	if (strict) {
		sr_data_t *data = NULL;
		int        rc2;

		rc2 = sr_get_data(ctx->sess, xpath, 0,
		                   CONFIG_SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS, 0, &data);
		if (rc2 != SR_ERR_OK) {
			mcp_err_from_session(err, ctx->sess, rc2,
			                     "sr_get_data");
			return NULL;
		}
		if (data == NULL || data->tree == NULL) {
			sr_release_data(data);
			mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
			             "no node matches the xpath");
			return NULL;
		}
		sr_release_data(data);
	}

	rc = sr_delete_item(ctx->sess, xpath,
	                    strict ? SR_EDIT_STRICT : 0);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_delete_item");
		sr_discard_changes(ctx->sess);
		return NULL;
	}

	rc = sr_apply_changes(ctx->sess, CONFIG_SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS);
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
