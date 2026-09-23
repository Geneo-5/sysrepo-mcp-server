/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Operational datastore read tool.
 */

#include <stdio.h>
#include <stdlib.h>

#include <json-c/json.h>

#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/operational.h>
#include <sysrepo/mcp/libconfig.h>

/* ----------------------------------------------------- sr_get_operational
 *
 * Always reads SR_DS_OPERATIONAL. The datastore of a session is not implicit:
 * a session left on SR_DS_RUNNING keeps answering from running, which is the
 * usual cause of an empty operational read.
 */
struct json_object *
tool_sr_get_operational(struct tool_ctx *ctx, struct json_object *args,
                        struct mcp_err *err)
{
	const char         *xpath = arg_xpath(args, err);
	sr_data_t          *data = NULL;
	struct json_object *res;
	struct json_object *payload;
	int                 max_depth = arg_int(args, "max_depth", 0);
	int                 timeout = arg_int(args, "timeout_ms",
	                                      mcp_config_get()->default_timeout_ms);
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
