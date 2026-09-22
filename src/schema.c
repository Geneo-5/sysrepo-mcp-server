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
	/* When xpath is NULL or "/", return the full module tree. */

	const struct ly_ctx     *ly;
	const struct lys_module *mod;
	const struct lysc_node  *node;
	struct json_object      *res;
	struct json_object      *tree;
	struct json_object      *nodes;
	struct json_object      *roots;
	int                      with_desc = arg_bool(args,
	                                              "with_descriptions", 0);

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
 */

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

struct json_object *
tool_get_help(struct tool_ctx *ctx, struct json_object *args,
              struct mcp_err *err)
{
	const char             *xpath = arg_xpath(args, err);
	const struct ly_ctx    *ly;
	const struct lysc_node *node;
	struct json_object     *res;
	LY_ARRAY_COUNT_TYPE    i;

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
		const struct lysc_node_leaf *leaf = (const struct lysc_node_leaf *)node;
		struct json_object *musts = json_object_new_array();
		struct json_object *whens = json_object_new_array();
		
		add_leaf_help(res, leaf->type);
		if (leaf->units)
			json_object_object_add(res, "units", json_object_new_string(leaf->units));

		LY_ARRAY_FOR(leaf->musts, i) {
			struct json_object *obj =
				json_object_new_object();
			const char *expr =
				lyxp_get_expr(leaf->musts[i].cond);

			json_object_object_add(obj, "expression",
				json_object_new_string(expr));
			if (leaf->musts[i].dsc)
				json_object_object_add(
					obj, "description",
					json_object_new_string(
						leaf->musts[i].dsc));
			json_object_array_add(musts, obj);
		}
		json_object_object_add(res, "must", musts);

		LY_ARRAY_FOR(leaf->when, i) {
			const char *expr = lyxp_get_expr(leaf->when[i]->cond);

			json_object_array_add(whens, json_object_new_string(expr));
		}
		json_object_object_add(res, "when", whens);
	}

	/* Must and when assertions on the node itself.
	 * musts is a sized array of lysc_must (by value, not
	 * pointer); when is a sized array of lysc_when *. */

	if (node->nodetype == LYS_LEAF) {
		const struct lysc_node_leaf *leaf = (const struct lysc_node_leaf *)node;
	
		/* Default value. If the node carries a default (either explicit or
		 * from its type), validate it and return the canonical string. */

		if (node->flags & LYS_SET_DFLT) {
			const char *raw = leaf->dflt.str;
			const char *canonical = NULL;

			if (raw && lyd_value_validate_dflt(
				node, raw, leaf->dflt.prefixes,
				NULL, NULL, &canonical) == LY_SUCCESS) {
				json_object_object_add(res, "default",
					json_object_new_string(canonical));
				lydict_remove(ly, canonical);
			}
		}
	}

	if (node->nodetype == LYS_LEAFLIST) {
		const struct lysc_node_leaflist *leaf = (const struct lysc_node_leaflist *)node;
	
		/* Default value. If the node carries a default (either explicit or
		 * from its type), validate it and return the canonical string. */

		if (node->flags & LYS_SET_DFLT) {
			struct json_object *defaults = json_object_new_array();
			
			LY_ARRAY_FOR(leaf->dflts, i) {
				const char *raw = leaf->dflts[i].str;
				const char *canonical = NULL;

				if (raw && lyd_value_validate_dflt(
					node, raw, leaf->dflts[i].prefixes,
					NULL, NULL, &canonical) == LY_SUCCESS) {
					json_object_array_add(defaults,
						json_object_new_string(canonical));
					lydict_remove(ly, canonical);
				}
			}
			json_object_object_add(res, "default", defaults);

		}
		json_object_object_add(res, "min-elements", json_object_new_int(leaf->min));
		if (leaf->max)
			json_object_object_add(res, "max-elements", json_object_new_int(leaf->max));
		else
			json_object_object_add(res, "max-elements", json_object_new_string("unbounded"));

		json_object_object_add(res, "ordered-by",
			json_object_new_string((leaf->flags & LYS_ORDBY_USER) ? "user" : "system"));
		
	}

	sr_session_release_context(ctx->sess);

	return res;
}
