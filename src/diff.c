/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * sr_diff_config: read two datastore subtrees (default running -> candidate)
 * and return a flat "diff" array of the leaf-level changes.
 *
 * libyang computes the diff (lyd_diff_siblings()). The two subtrees are read
 * from the session, libyang produces a diff tree, and every leaf-level change
 * in that tree becomes one entry:
 *
 *     { "xpath": "...", "operation": "created" | "replaced" | "deleted",
 *       "value":      <new subtree>,
 *       "previous_value": <old subtree> }
 *
 * libyang records the operation, the original value, the key and the
 * user-ordered position as metadata under the "yang" module (diff.c). libyang
 * 5.8.6 stores that metadata directly on the changed node (lyd_insert_meta(),
 * tree_data.c), so it is readable with the public lyd_find_meta() — no
 * libyang-internal API, and node->priv (private user data) is not involved.
 */

#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/libconfig.h>
#include <sysrepo/mcp/config_tools.h>
#include <sysrepo/mcp/diff.h>

/* Serialize a data subtree (a diff node is a real data node) to a JSON
 * object, reusing tree_to_json (utilities.c). A NULL tree yields an empty
 * object, not a failure.
 */
static struct json_object *
node_to_json(const struct lyd_node *node)
{
	struct json_object *js = tree_to_json(node, 0, NULL);

	if (!js)
		js = json_object_new_object();
	return js;
}

/* Read one "yang" metadata entry (operation, value, position, ...) as a
 * string. libyang stores that metadata on the changed node itself (node->meta)
 * in 5.8.6: walk node->meta and return the entry named `name`, freshly
 * strdup'd, or NULL if absent. */
static char *
get_node_meta(const struct lyd_node *node, const char *name)
{
	struct lyd_meta *meta;

	meta = lyd_find_meta(node->meta, NULL, name);
	if (!meta)
		return NULL;

	return strdup(lyd_get_meta_value(meta));
}

/* Every leaf-level change in a diff subtree, collected into a JSON array. A
 * diff node that carries children is an anchor (nothing changed here, a
 * descendant changed): skip it and recurse. A diff node with no children is a
 * leaf change: read its operation, value and previous value.
 *
 * libyang reports every leaf change (default-value crossings included) as
 * "replace" in its own `operation`; it does not expose "created"/"deleted".
 * Those two states come from two public metadata:
 *   - "orig-default" ("true"/"false"): was the OLD side (source) a default
 *     value? (libyang, diff.c:959)
 *   - the LYD_DEFAULT flag on the diff node itself: was the NEW side (target)
 *     a default value? the diff node is a copy of the target, with its flags
 *     copied through (libyang, diff.c:360, LYD_DUP_WITH_FLAGS), so libyang
 *     sets that flag on it (also read there, diff.c:1858).
 */
static void
emit_leaf_changes(const struct lyd_node *node, struct json_object *array,
                  const struct ly_ctx *ly)
{
	char *op;
	char *orig_default;
	char *my_op;
	char *xpath_str;
	struct json_object *entry;
	struct json_object *prev = NULL;
	int old_was_default;
	int new_was_default;

	if (lyd_child(node)) {
		emit_leaf_changes(lyd_child(node), array, ly);
		return;
	}

	/* Absent or "none": no value change, emit nothing. */
	op = get_node_meta(node, "operation");
	if (op && strcmp(op, "none") == 0) {
		free(op);
		return;
	}

	/* libyang exposes no create/delete of its own; interpret "orig-default"
	 * (old side, source) and the diff node's LYD_DEFAULT flag (new side,
	 * target) to pick between them. */
	orig_default = get_node_meta(node, "orig-default");
	old_was_default = (orig_default && !strcmp(orig_default, "true"));
	new_was_default = (node->flags & LYD_DEFAULT) != 0;
	if (strcmp(op, "create") == 0) {
		my_op = "created";
	} else if (strcmp(op, "delete") == 0) {
		my_op = "deleted";
	} else if (old_was_default && !new_was_default) {
		my_op = "created";
	} else if (!old_was_default && new_was_default) {
		my_op = "deleted";
	} else {
		my_op = "replaced";
	}

	entry = json_object_new_object();

	/* libyang allocates the xpath (NULL -> malloc), free it. */
	xpath_str = lyd_path(node, LYD_PATH_STD, NULL, 0);
	json_object_object_add(entry, "xpath",
	                       json_object_new_string(xpath_str ?
		xpath_str : ""));
	free(xpath_str);

	/* value carries the NEW side: the diff node is a copy of the target. */
	if (strcmp(my_op, "deleted") != 0) {
		json_object_object_add(entry, "value", node_to_json(node));
	}

	/* previous_value carries the OLD side. libyang records it as "orig-value"
	 * for a leaf in "replace" (diff.c:967); for a structural "delete" (the
	 * target was stripped, no "orig-value"), the diff node itself, a copy of
	 * the source, holds the previous value. */
	if (strcmp(my_op, "created") != 0) {
		char *orig = get_node_meta(node, "orig-value");
		if (orig) {
			struct json_object *js = json_tokener_parse(orig);

			if (!json_object_is_type(js, json_type_null))
				prev = js;
			free(orig);
		} else {
			prev = node_to_json(node);
		}
	}

	json_object_object_add(entry, "operation",
	                       json_object_new_string(my_op));
	if (prev)
		json_object_object_add(entry, "previous_value", prev);

	json_object_array_add(array, entry);

	free(op);
	free(orig_default);
}

/* Walk the top-level change forest produced by libyang. */
static void
emit_diff(const struct lyd_node *diff, struct json_object *array,
          const struct ly_ctx *ly)
{
	for (struct lyd_node *d = (struct lyd_node *)diff; d; d = d->next)
		emit_leaf_changes(d, array, ly);
}

struct json_object *
tool_sr_diff_config(struct tool_ctx *ctx, struct json_object *args,
                    struct mcp_err *err)
{
	const char *source_name = NULL, *target_name = NULL;
	const char *xpath = arg_xpath(args, err);
	/* sr_get_data exige un xpath non nul ; sans filtre, lire le datastore
	 * entier (« /* ») revient à diffuser toutes les racines. */
	const char *read_xpath;
	sr_datastore_t source, target;
	int max_depth = arg_int(args, "max_depth", 0);
	const struct ly_ctx *ly;
	struct json_object *res, *diff_array, *xpath_val;
	sr_data_t *tree_source = NULL, *tree_target = NULL;
	struct lyd_node *diff = NULL;
	size_t changed;
	int rc;

	/* source and target are both required, and must name different
	 * datastores: identical datastores is a usage error, not a diff. */
	if (copy_datastore_arg(args, "source", &source, &source_name, err) ||
	    copy_datastore_arg(args, "destination", &target, &target_name, err))
		return NULL;
	if (source == target) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "source and target must be different datastores");
		return NULL;
	}
	if (max_depth < 0 || (uint32_t)max_depth > UINT32_MAX) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "max_depth must not exceed the range of an integer");
		return NULL;
	}

	/* Read the two subtrees from the same session, switching datastore.
	 * Both trees live in the same libyang context, so libyang can resolve
	 * schema across them. No xpath reads the whole datastore (see
	 * read_xpath below). */

	read_xpath = (xpath && *xpath) ? xpath : "/*";
	if ((rc = sr_session_switch_ds(ctx->sess, source)) != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_session_switch_ds");
		return NULL;
	}
	if (sr_get_data(ctx->sess, read_xpath, (uint32_t)max_depth,
		                mcp_config_get()->default_timeout_ms, 0, &tree_source) !=
	    SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		return NULL;
	}
	if ((rc = sr_session_switch_ds(ctx->sess, target)) != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_session_switch_ds");
		sr_release_data(tree_source);
		return NULL;
	}
	if (sr_get_data(ctx->sess, read_xpath, (uint32_t)max_depth,
		                mcp_config_get()->default_timeout_ms, 0, &tree_target) !=
	    SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		sr_release_data(tree_source);
		return NULL;
	}

	ly = sr_session_acquire_context(ctx->sess);

	diff_array = json_object_new_array();
	if (lyd_diff_siblings(tree_source->tree, tree_target->tree,
		                    LYD_DIFF_DEFAULTS | LYD_DIFF_META, &diff) != LY_SUCCESS) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "libyang diff failed: %s", ly_err_last(ly));
		if (diff)
			lyd_free_all(diff);
		json_object_put(diff_array);
		sr_release_data(tree_source);
		sr_release_data(tree_target);
		return NULL;
	}

	emit_diff(diff, diff_array, ly);
	changed = json_object_array_length(diff_array);

	xpath_val = json_object_new_string(xpath ? xpath : "data");

	res = json_object_new_object();
	json_object_object_add(res, "diff", diff_array);
	json_object_object_add(res, "changed", json_object_new_uint64(changed));
	json_object_object_add(res, "source",
	               json_object_new_string(source_name));
	json_object_object_add(res, "target",
	               json_object_new_string(target_name));
	json_object_object_add(res, "xpath", xpath_val);

	if (diff)
		lyd_free_all(diff);
	sr_release_data(tree_source);
	sr_release_data(tree_target);
	sr_session_release_context(ctx->sess);

	return res;
}
