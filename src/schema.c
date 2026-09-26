/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Schema introspection: get_schema and compiled node details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/schema.h>
#include <sysrepo/mcp/libconfig.h>

/* The walker owns recursion and depth policy. Each schema tool supplies a
 * callback that adds the fields its output contract needs to a visited node. */
typedef void (*schema_node_callback)(struct json_object *object,
				     const struct lysc_node *node,
				     const char *xpath, void *data);

struct schema_walk {
	int depth_limit;
	schema_node_callback callback;
	void *callback_data;
};

static struct json_object *
schema_walk_node(const struct lysc_node *node, const char *xpath, int depth,
		 const struct schema_walk *walk)
{
	struct json_object *obj = json_object_new_object();
	const struct lysc_node *child;

	walk->callback(obj, node, xpath, walk->callback_data);
	if (depth < walk->depth_limit) {
		struct json_object *children = NULL;
		char child_xpath[1024];

		for (child = lysc_node_child(node); child; child = child->next) {
			if (child->nodetype == LYS_INPUT ||
			    child->nodetype == LYS_OUTPUT) {
				snprintf(child_xpath, sizeof(child_xpath), "%s", xpath);
			} else if (child->module != node->module) {
				snprintf(child_xpath, sizeof(child_xpath), "%s/%s:%s",
				         xpath, child->module->name, child->name);
			} else {
				snprintf(child_xpath, sizeof(child_xpath), "%s/%s",
				         xpath, child->name);
			}
			if (!children)
				children = json_object_new_object();
			json_object_object_add(children, child->name,
			                       schema_walk_node(child, child_xpath,
			                                        depth + 1, walk));
		}
		if (children)
			json_object_object_add(obj, "children", children);
	}
	return obj;
}

static void add_leaf_help(struct json_object *res,
			  const struct lysc_type *type);

static void
tree_add_defaults(struct json_object *obj, const struct lysc_node *node)
{
	LY_ARRAY_COUNT_TYPE i;
	const struct ly_ctx *ly = node->module->ctx;

	if (node->nodetype == LYS_LEAF) {
		const struct lysc_node_leaf *leaf =
			(const struct lysc_node_leaf *)node;
		if (node->flags & LYS_SET_DFLT) {
			const char *canonical = NULL;
			if (leaf->dflt.str && lyd_value_validate_dflt(
			        node, leaf->dflt.str, leaf->dflt.prefixes,
			        NULL, NULL, &canonical) == LY_SUCCESS) {
				json_object_object_add(obj, "default",
				                       json_object_new_string(canonical));
				lydict_remove(ly, canonical);
			}
		}
	} else if (node->nodetype == LYS_LEAFLIST) {
		const struct lysc_node_leaflist *leaf =
			(const struct lysc_node_leaflist *)node;
		if (node->flags & LYS_SET_DFLT) {
			struct json_object *defaults = json_object_new_array();
			LY_ARRAY_FOR(leaf->dflts, i) {
				const char *canonical = NULL;
				if (leaf->dflts[i].str && lyd_value_validate_dflt(
				        node, leaf->dflts[i].str,
				        leaf->dflts[i].prefixes, NULL, NULL,
				        &canonical) == LY_SUCCESS) {
					json_object_array_add(defaults,
					                     json_object_new_string(canonical));
					lydict_remove(ly, canonical);
				}
			}
			json_object_object_add(obj, "default", defaults);
		}
	}
}

static void
tree_node_callback(struct json_object *obj, const struct lysc_node *node,
		   const char *xpath, void *data)
{
	struct lysc_must *must;
	struct lysc_when **when;
	struct json_object *musts;
	struct json_object *whens;
	LY_ARRAY_COUNT_TYPE i;
	(void)data;

	json_object_object_add(obj, "type", nodetype_to_json(node->nodetype));
	json_object_object_add(obj, "xpath", json_object_new_string(xpath));
	json_object_object_add(obj, "config",
	                       json_object_new_boolean(
	                               (node->flags & LYS_CONFIG_W) ? 1 : 0));
	json_object_object_add(obj, "mandatory",
	                       json_object_new_boolean(
	                               (node->flags & LYS_MAND_TRUE) ? 1 : 0));
	json_object_object_add(obj, "augmented",
	                       json_object_new_boolean(
	                               node->parent && node->module !=
	                               node->parent->module));
	if (node->nodetype == LYS_CONTAINER) {
		json_object_object_add(obj, "presence",
		                       json_object_new_boolean(
	                               (node->flags & LYS_PRESENCE) ? 1 : 0));
	}
	if (node->nodetype == LYS_LIST || node->nodetype == LYS_LEAFLIST) {
		uint32_t min, max;
		struct json_object *keys = NULL;
		if (node->nodetype == LYS_LIST) {
			const struct lysc_node_list *list =
				(const struct lysc_node_list *)node;
			const struct lysc_node *key;
			min = list->min;
			max = list->max;
			keys = json_object_new_array();
			for (key = list->child; key; key = key->next) {
				if (lysc_is_key(key))
					json_object_array_add(keys,
						json_object_new_string(key->name));
			}
			json_object_object_add(obj, "keys", keys);
		} else {
			const struct lysc_node_leaflist *leaflist =
				(const struct lysc_node_leaflist *)node;
			min = leaflist->min;
			max = leaflist->max;
		}
		json_object_object_add(obj, "min-elements",
		                       json_object_new_int64(min));
		if (max)
			json_object_object_add(obj, "max-elements",
			                       json_object_new_int64(max));
		else
			json_object_object_add(obj, "max-elements",
			                       json_object_new_string("unbounded"));
	}
	if (node->parent && node->parent->nodetype == LYS_CASE) {
		json_object_object_add(obj, "case",
		                       json_object_new_string(node->parent->name));
		if (node->parent->parent &&
		    node->parent->parent->nodetype == LYS_CHOICE)
			json_object_object_add(obj, "choice",
			                       json_object_new_string(
			                               node->parent->parent->name));
	} else if (node->parent && node->parent->nodetype == LYS_CHOICE) {
		json_object_object_add(obj, "choice",
		                       json_object_new_string(node->parent->name));
	}
	if (node->dsc)
		json_object_object_add(obj, "description",
	                               json_object_new_string(node->dsc));
	if (node->ref)
		json_object_object_add(obj, "reference",
	                               json_object_new_string(node->ref));
	if (node->module) {
		json_object_object_add(obj, "module",
	                               json_object_new_string(node->module->name));
		if (node->module->ns)
			json_object_object_add(obj, "namespace",
			                       json_object_new_string(node->module->ns));
	}
	if (node->nodetype == LYS_LEAF) {
		const struct lysc_node_leaf *leaf =
			(const struct lysc_node_leaf *)node;
		add_leaf_help(obj, leaf->type);
		if (leaf->units)
			json_object_object_add(obj, "units",
			                       json_object_new_string(leaf->units));
	} else if (node->nodetype == LYS_LEAFLIST) {
		const struct lysc_node_leaflist *leaf =
			(const struct lysc_node_leaflist *)node;
		add_leaf_help(obj, leaf->type);
		if (leaf->units)
			json_object_object_add(obj, "units",
			                       json_object_new_string(leaf->units));
	}
	tree_add_defaults(obj, node);
	musts = json_object_new_array();
	must = lysc_node_musts(node);
	LY_ARRAY_FOR(must, i) {
		struct json_object *item = json_object_new_object();
		json_object_object_add(item, "expression",
		                       json_object_new_string(
		                               lyxp_get_expr(must[i].cond)));
		if (must[i].dsc)
			json_object_object_add(item, "description",
			                       json_object_new_string(must[i].dsc));
		json_object_array_add(musts, item);
	}
	json_object_object_add(obj, "must", musts);
	whens = json_object_new_array();
	when = lysc_node_when(node);
	LY_ARRAY_FOR(when, i)
		json_object_array_add(whens,
		                      json_object_new_string(
		                              lyxp_get_expr(when[i]->cond)));
	json_object_object_add(obj, "when", whens);
}

static struct json_object *
schema_node_at_path(const struct lysc_node *node, const char *path,
		    int effective_depth)
{
	struct schema_walk walk = {
		.depth_limit = effective_depth,
		.callback = tree_node_callback,
	};
	walk.callback_data = NULL;
	return schema_walk_node(node, path, 0, &walk);
}

/* ----------------------------------------------------------------- get_schema
 *
 * Walk either a selected schema subtree or every implemented module. The
 * response keeps module metadata separate and uses the recursive node tree
 * as its sole path-bearing representation.
 */
struct json_object *
tool_get_schema(struct tool_ctx *ctx, struct json_object *args,
		struct mcp_err *err)
{
	const char *xpath = arg_string(args, "xpath");
	const struct ly_ctx *ly;
	const struct lys_module *mod;
	const struct lysc_node *selected = NULL;
	struct json_object *res, *modules;
	uint32_t index = 0;
	int max_depth = arg_int(args, "max_depth", 0);
	int effective_depth;
	int count = 0;

	if (xpath && strcmp(xpath, "/") && !xpath_wellformed(xpath)) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
			    "xpath must be \"/\" or an absolute path with a "
			    "module prefix; got \"%s\"", xpath);
		return NULL;
	}
	if (args && json_object_object_get_ex(args, "xpath", &res) &&
	    !json_object_is_type(res, json_type_string)) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "xpath must be a string when provided");
		return NULL;
	}
	if (max_depth < 0) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "max_depth must not be negative");
		return NULL;
	}
	effective_depth = max_depth == 0 ||
		max_depth > (int)mcp_config_get()->max_tree_depth ?
		(int)mcp_config_get()->max_tree_depth : max_depth;

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
			    "no libyang context on the session");
		return NULL;
	}

	if (xpath && strcmp(xpath, "/")) {
		selected = lys_find_path(ly, NULL, xpath, 0);
		if (!selected) {
			mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
			            "no schema node matches \"%s\"", xpath);
			sr_session_release_context(ctx->sess);
			return NULL;
		}
		mod = selected->module;
		if (!mod || !mod->implemented || !mod->compiled) {
			mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
			            "the schema node is not in an implemented module");
			sr_session_release_context(ctx->sess);
			return NULL;
		}
	}

	modules = json_object_new_object();
	while (selected || (mod = ly_ctx_get_module_iter(ly, &index))) {
		const struct lysc_node *node;
		struct json_object *entry, *roots;
		char path[1024];

		if (!selected && (!mod->implemented || !mod->compiled)) {
			continue;
		}

		entry = json_object_new_object();
		roots = json_object_new_object();
		json_object_object_add(entry, "namespace",
		                       json_object_new_string(mod->ns ? mod->ns : ""));
		json_object_object_add(entry, "prefix",
		                       json_object_new_string(mod->prefix ? mod->prefix : ""));
		json_object_object_add(entry, "revision",
	                       json_object_new_string(mod->revision ? mod->revision : ""));

		if (xpath && strcmp(xpath, "/")) {
			json_object_object_add(roots, selected->name,
			                       schema_node_at_path(
			                       selected, xpath,
			                       effective_depth));
		} else {
			for (node = mod->compiled->data; node; node = node->next) {
				snprintf(path, sizeof(path), "/%s:%s", mod->name,
				         node->name);
				json_object_object_add(roots, node->name,
				                       schema_node_at_path(node, path,
				                                           effective_depth));
			}
			for (node = (const struct lysc_node *)mod->compiled->rpcs;
			     node; node = node->next) {
				snprintf(path, sizeof(path), "/%s:%s", mod->name,
				         node->name);
				json_object_object_add(roots, node->name,
				                       schema_node_at_path(node, path,
				                                           effective_depth));
			}
			for (node = (const struct lysc_node *)mod->compiled->notifs;
			     node; node = node->next) {
				snprintf(path, sizeof(path), "/%s:%s", mod->name,
				         node->name);
				json_object_object_add(roots, node->name,
				                       schema_node_at_path(node, path,
				                                           effective_depth));
			}
		}
		json_object_object_add(entry, "nodes", roots);
		json_object_object_add(modules, mod->name, entry);
		count++;
		if (selected) {
			selected = NULL;
			break;
		}
	}
	sr_session_release_context(ctx->sess);

	res = json_object_new_object();
	json_object_object_add(res, "modules", modules);
	json_object_object_add(res, "count", json_object_new_int(count));
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

/* Helpers for compiled leaf type information emitted by get_schema. */

static void
res_add_range(struct json_object *res, const struct lysc_range *range, LY_DATA_TYPE basetype)
{
	struct json_object *values = json_object_new_array();
	LY_ARRAY_COUNT_TYPE u;
	char buf[256];

	if (!range)
		return;

	LY_ARRAY_FOR(range->parts, u) {
		if (range->parts[u].max_64 == range->parts[u].min_64) {
			if (basetype <= LY_TYPE_STRING) { /* unsigned values */
				snprintf(buf, sizeof buf, "%" PRIu64, range->parts[u].max_u64);
			} else { /* signed values */
				snprintf(buf, sizeof buf, "%" PRId64, range->parts[u].max_64);
			}
		} else {
			if (basetype <= LY_TYPE_STRING) { /* unsigned values */
				snprintf(buf, sizeof buf, "%" PRIu64 "..%" PRIu64, range->parts[u].min_u64, range->parts[u].max_u64);
			} else { /* signed values */
				snprintf(buf, sizeof buf, "%" PRId64 "..%" PRId64, range->parts[u].min_64, range->parts[u].max_64);
			}
		}
		json_object_array_add(values,json_object_new_string(buf));
	}	

	json_object_object_add(res, (basetype == LY_TYPE_STRING || basetype == LY_TYPE_BINARY) ? "length" : "range", values);
}

static void
add_leaf_help(struct json_object *res, const struct lysc_type *type)
{
	LY_ARRAY_COUNT_TYPE u;

	json_object_object_add(res, "base_type",
		json_object_new_string(basetype_name(type->basetype)));

	switch (type->basetype) {
	case LY_TYPE_BINARY: {
		struct lysc_type_bin *bin = (struct lysc_type_bin *)type;

		res_add_range(res, bin->length, type->basetype);
		break;
	}
	case LY_TYPE_UINT8:
	case LY_TYPE_UINT16:
	case LY_TYPE_UINT32:
	case LY_TYPE_UINT64:
	case LY_TYPE_INT8:
	case LY_TYPE_INT16:
	case LY_TYPE_INT32:
	case LY_TYPE_INT64: {
		struct lysc_type_num *num = (struct lysc_type_num *)type;

		res_add_range(res, num->range, type->basetype);
		break;
	}
	case LY_TYPE_STRING: {
		struct lysc_type_str *str = (struct lysc_type_str *)type;
		struct json_object *values = json_object_new_array();

		res_add_range(res, str->length, type->basetype);
		
		LY_ARRAY_FOR(str->patterns, u) {
			struct json_object *obj = json_object_new_object();

			json_object_object_add(obj, "pattern",
				json_object_new_string(str->patterns[u]->expr));
			json_object_object_add(obj, "invert-match", 
				json_object_new_boolean(str->patterns[u]->inverted));
			if (str->patterns[u]->dsc)
				json_object_object_add(obj, "description",
		                       json_object_new_string(str->patterns[u]->dsc));
			if (str->patterns[u]->ref)
				json_object_object_add(res, "reference",
						json_object_new_string(str->patterns[u]->ref));

			json_object_array_add(values, obj);
		}
		json_object_object_add(res, "patterns", values);
		break;
	}
	case LY_TYPE_BITS:
	case LY_TYPE_ENUM: {
		/* bits and enums structures are compatible */
		struct lysc_type_bits *bits = (struct lysc_type_bits *)type;
		struct json_object *values = json_object_new_array();

		LY_ARRAY_FOR(bits->bits, u) {
			json_object_array_add(values,
				json_object_new_string(bits->bits[u].name));
		}

		json_object_object_add(res, "values", values);
		break;
	}
	case LY_TYPE_BOOL:
	case LY_TYPE_EMPTY:
		/* nothing to do */
		break;
	case LY_TYPE_DEC64: {
		struct lysc_type_dec *dec = (struct lysc_type_dec *)type;

		json_object_object_add(res, "fraction-digits", 
			json_object_new_int(dec->fraction_digits));
		res_add_range(res, dec->range, dec->basetype);
		break;
	}
	case LY_TYPE_IDENT: {
		struct lysc_type_identityref *ident = (struct lysc_type_identityref *)type;
		struct json_object *values = json_object_new_array();

		LY_ARRAY_FOR(ident->bases, u) {
			json_object_array_add(values,
				json_object_new_string(ident->bases[u]->name));
		}
		json_object_object_add(res, "base", values);
		break;
	}
	case LY_TYPE_INST: {
		struct lysc_type_instanceid *inst = (struct lysc_type_instanceid *)type;

		json_object_object_add(res, "require-instance", 
			json_object_new_boolean(inst->require_instance));

		break;
	}
	case LY_TYPE_LEAFREF: {
		struct lysc_type_leafref *lr = (struct lysc_type_leafref *)type;

		json_object_object_add(res, "path", 
			json_object_new_string(lyxp_get_expr(lr->path)));
		json_object_object_add(res, "require-instance", 
			json_object_new_boolean(lr->require_instance));
		break;
	}
	case LY_TYPE_UNION: {
		struct lysc_type_union *un = (struct lysc_type_union *)type;

		break;
	}
	}
}
