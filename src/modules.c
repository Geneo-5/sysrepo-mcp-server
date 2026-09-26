/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Module management: list, install, uninstall.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/modules.h>

/* ---------------------------------------------------------------- sr_list_modules
 *
 * List all YANG modules known to the datastore (or only the implemented ones
 * when `implemented_only` is true). Each entry carries name, revision,
 * namespace, prefix, and whether it is implemented.
 */

struct json_object *
tool_sr_list_modules(struct tool_ctx *ctx, struct json_object *args,
                     struct mcp_err *err)
{
	const struct ly_ctx     *ly;
	const struct lys_module *mod;
	struct json_object      *res;
	struct json_object      *list;
	uint32_t                 index = 0;
	int                      implemented_only =
		arg_bool(args, "implemented_only", 1);

	(void)err;

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "no libyang context on the session");
		return NULL;
	}

	list = json_object_new_array();

	while ((mod = ly_ctx_get_module_iter(ly, &index))) {
		struct json_object *entry;
		struct json_object *features;
		size_t feature_index;

		if (implemented_only && !mod->implemented)
			continue;

		entry = json_object_new_object();
		json_object_object_add(entry, "name",
		                       json_object_new_string(mod->name));
		json_object_object_add(entry, "revision",
		                       json_object_new_string(
		                               mod->revision ?
		                               mod->revision : ""));
		json_object_object_add(entry, "namespace",
		                       json_object_new_string(
		                               mod->ns ? mod->ns : ""));
		json_object_object_add(entry, "prefix",
		                       json_object_new_string(
		                               mod->prefix ? mod->prefix : ""));
		json_object_object_add(entry, "implemented",
		                       json_object_new_boolean(
		                               mod->implemented ? 1 : 0));
		features = json_object_new_array();
		if (mod->compiled && mod->compiled->features) {
			for (feature_index = 0;
			     mod->compiled->features[feature_index];
			     feature_index++) {
				json_object_array_add(features,
					json_object_new_string(
						mod->compiled->features[feature_index]));
			}
		}
		json_object_object_add(entry, "features", features);
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

/* --------------------------------------------------------------- sr_module_install
 *
 * Install a YANG module into the datastore. This is a connection-level
 * operation: it takes `g_conn` and not the per-request session.
 */

struct json_object *
tool_sr_module_install(struct tool_ctx *ctx, struct json_object *args,
                       struct mcp_err *err)
{
	const char         *file;
	const char         *search_dirs = NULL;
	struct json_object *features;
	struct json_object *res;
	const char        **feature_list = NULL;
	size_t              feature_count = 0;
	int                 rc;

	(void)ctx;

	file = arg_string(args, "yang_file");
	if (!file) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "yang_file is required");
		return NULL;
	}

	search_dirs = arg_string(args, "search_dirs");

	if (arg_object(args, "features") &&
	    json_object_is_type(arg_object(args, "features"),
				json_type_array)) {
		size_t i;

		feature_count =
			json_object_array_length(arg_object(args, "features"));
		feature_list = calloc(feature_count + 1, sizeof(*feature_list));
		if (!feature_list) {
			mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
			            "out of memory");
			return NULL;
		}

		for (i = 0; i < feature_count; i++) {
			feature_list[i] = json_object_get_string(
				json_object_array_get_idx(
					arg_object(args, "features"), i));
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

/* ------------------------------------------------------------- sr_module_uninstall
 *
 * Remove a YANG module and its data from the datastore. Optional force flag
 * removes dependent modules as well.
 */

struct json_object *
tool_sr_module_uninstall(struct tool_ctx *ctx, struct json_object *args,
                         struct mcp_err *err)
{
	const char *module = arg_string(args, "module");
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
	json_object_object_add(res, "ok", json_object_new_boolean(1));
	json_object_object_add(res, "module", json_object_new_string(module));

	return res;
}
