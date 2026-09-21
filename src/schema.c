/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Schema introspection: get_tree, get_help, schema_node_to_json, basetype_name.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/schema.h>

/* ---------------------------------------------------------------- schema_node_to_json
 *
 * Recursively serialize a YANG schema node to JSON. When `flat` is non-NULL,
 * also append a flat entry (xpath + type + config) to the flat array. The
 * `parent_path` carries the parent's path so children can be addressed as
 * absolute XPaths.
 *
 * Children are grouped under a `children` object keyed by name. A depth limit
 * (CONFIG_SYSREPO_MCP_SERVER_MAX_TREE_DEPTH) prevents unbounded recursion on deeply nested schemas.
 */

struct json_object *
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

	if (depth < CONFIG_SYSREPO_MCP_SERVER_MAX_TREE_DEPTH) {
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

/* ------------------------------------------------------------------- get_tree
 *
 * Return the YANG schema tree of a module. When `xpath` is provided, return
 * only the subtree rooted at that path (which may be a single node). Without
 * xpath, all data nodes, RPCs and notifications are returned across three
 * separate lists merged under `nodes`.
 */

struct json_object *
tool_get_tree(struct tool_ctx *ctx, struct json_object *args,
              struct mcp_err *err)
{
	const char              *module;
	const char              *xpath;

	module = arg_string(args, "module");
	if (!module) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
			    "module is required");
		return NULL;
	}

	xpath = arg_string(args, "xpath");
	if (!xpath) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
			    "xpath is required");
		return NULL;
	}

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
const char *
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

/* ------------------------------------------------------------------- get_help
 *
 * Document one YANG schema node found by xpath. Returns node_type, mandatory,
 * config, description, reference, module, namespace, and for leaf/leaflist
 * nodes: base_type, values (for enum), and units.
 *
 * TODO: range, length, pattern restrictions, and default value.
 */

struct json_object *
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
