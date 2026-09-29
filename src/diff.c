/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * sr_diff_config: read two datastore subtrees (both "source" and
 * "destination" are required and must differ) and return a flat "diff" array
 * of the changes between them.
 *
 * libyang computes the diff (lyd_diff_siblings()). The two subtrees are read
 * from the session, libyang produces a diff tree, and every change in that
 * tree becomes one entry:
 *
 *     { "xpath": "...", "operation": "created" | "replaced" | "deleted",
 *       "value":          <new subtree>,
 *       "previous_value": <old subtree> }
 *
 * libyang records the operation, the original value, the key and the
 * user-ordered position as metadata under the "yang" module. libyang 5.8.6
 * stores that metadata directly on the changed node (node->meta), so it is
 * read here through the public struct lyd_meta — no libyang-internal API,
 * and node->priv (private user data) is not involved.
 *
 * Diff tree layout: "operation" is set only on the topmost node of a created,
 * deleted or replaced subtree; its descendants inherit it and carry no
 * metadata of their own. A node with operation "none" (or none at all) is an
 * anchor: nothing changed on it, a descendant did.
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
#include <sysrepo/mcp/log.h>

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

/* Serialize the OLD value of a term node (leaf / leaf-list). libyang stores
 * it as "orig-value", a canonical string and not JSON: parsing it as JSON
 * would drop or retype string values ("eth0" fails, "123" becomes an int).
 * Instead, duplicate the diff node, put the old value back and serialize it
 * exactly like the new value, so both sides have the same shape. Falls back
 * to a plain JSON string if that fails.
 */
static struct json_object *
previous_term_to_json(const struct lyd_node *node, const char *orig)
{
	struct lyd_node *dup = NULL;
	struct json_object *js = NULL;
	LY_ERR lyrc;

	if (lyd_dup_single(node, NULL, LYD_DUP_NO_META, &dup) == LY_SUCCESS) {
		lyrc = lyd_change_term(dup, orig);
		if (lyrc == LY_SUCCESS || lyrc == LY_ENOT || lyrc == LY_EEXIST)
			js = tree_to_json(dup, 0, NULL);
		lyd_free_tree(dup);
	}
	return js ? js : json_object_new_string(orig);
}

/* Read one "yang" metadata entry (operation, orig-value, orig-default, ...) as
 * a string. libyang stores that metadata on the changed node itself (node->meta)
 * in 5.8.6: walk node->meta and return the entry named `name` that belongs to
 * the "yang" module, freshly strdup'd, or NULL if absent.
 *
 * The module is identified through meta->annotation->module, which needs no
 * ly_ctx_get_module() lookup (the "yang" module is not always resolvable from
 * this context), while still refusing same-named annotations of other modules
 * (e.g. ietf-netconf:operation). */
static char *
get_node_meta(const struct lyd_node *node, const char *name)
{
	struct lyd_meta *meta;
	const char *value;

	for (meta = node->meta; meta; meta = meta->next) {
		if (!meta->annotation || !meta->annotation->module ||
		    strcmp(meta->annotation->module->name, "yang") != 0)
			continue;
		if (strcmp(meta->name, name) == 0) {
			value = lyd_get_meta_value(meta);
			return value ? strdup(value) : NULL;
		}
	}
	return NULL;
}

static void emit_diff(const struct lyd_node *diff, struct json_object *array);

/* Emit the change(s) carried by one diff node.
 *
 * - No "operation" metadata, or "none": the node is an anchor (or a list key).
 *   Nothing changed on it; recurse into its children, if any.
 * - Any other operation sits on the topmost node of a changed subtree: emit
 *   ONE entry for that whole node (a created/deleted container or list
 *   instance is a single change, its descendants carry no metadata).
 *
 * libyang reports a leaf changing across a default value as "replace" in its
 * own `operation`. "created"/"deleted" for those come from two public
 * metadata:
 *   - "orig-default" ("true"/"false"): was the OLD side (source) a default
 *     value?
 *   - the LYD_DEFAULT flag on the diff node itself: was the NEW side (target)
 *     a default value? (the diff node is a copy of the target, flags
 *     included.)
 */
static void
emit_change(const struct lyd_node *node, struct json_object *array)
{
	char *op;
	char *orig_default = NULL;
	char *xpath_str;
	const char *my_op;
	struct json_object *entry;
	struct json_object *prev = NULL;
	int is_term;
	int old_was_default = 0;
	int new_was_default = 0;

	op = get_node_meta(node, "operation");
	if (!op || strcmp(op, "none") == 0) {
		free(op);
		if (lyd_child(node))
			emit_diff(lyd_child(node), array);
		return;
	}

	is_term = node->schema && (node->schema->nodetype & LYD_NODE_TERM);

	if (strcmp(op, "create") == 0) {
		my_op = "created";
	} else if (strcmp(op, "delete") == 0) {
		my_op = "deleted";
	} else {
		if (is_term) {
			orig_default = get_node_meta(node, "orig-default");
			old_was_default = orig_default &&
			                  strcmp(orig_default, "true") == 0;
			new_was_default = (node->flags & LYD_DEFAULT) != 0;
		}
		if (old_was_default && !new_was_default)
			my_op = "created";
		else if (!old_was_default && new_was_default)
			my_op = "deleted";
		else
			my_op = "replaced";
	}

	entry = json_object_new_object();

	/* libyang allocates the xpath (NULL -> malloc), free it. */
	xpath_str = lyd_path(node, LYD_PATH_STD, NULL, 0);
	json_object_object_add(entry, "xpath",
	                       json_object_new_string(xpath_str ? xpath_str : ""));
	free(xpath_str);

	/* value carries the NEW side: the diff node is a copy of the target. */
	if (strcmp(my_op, "deleted") != 0)
		json_object_object_add(entry, "value", node_to_json(node));

	/* previous_value carries the OLD side. For a term node in "replace",
	 * libyang records it as "orig-value". For a structural "delete" (the
	 * target was stripped, no "orig-value"), the diff node itself, a copy of
	 * the source, holds the previous value. */
	if (strcmp(my_op, "created") != 0) {
		char *orig = is_term ? get_node_meta(node, "orig-value") : NULL;

		prev = orig ? previous_term_to_json(node, orig)
		            : node_to_json(node);
		free(orig);
	}

	json_object_object_add(entry, "operation",
	                       json_object_new_string(my_op));
	if (prev)
		json_object_object_add(entry, "previous_value", prev);

	json_object_array_add(array, entry);

	free(op);
	free(orig_default);
}

/* Walk a forest of diff siblings (top level, or the children of an anchor). */
static void
emit_diff(const struct lyd_node *diff, struct json_object *array)
{
	const struct lyd_node *d;

	LY_LIST_FOR(diff, d)
		emit_change(d, array);
}

struct json_object *
tool_sr_diff_config(struct tool_ctx *ctx, struct json_object *args,
                    struct mcp_err *err)
{
	const char *source_name = NULL, *target_name = NULL;
	const char *xpath = NULL;
	/* sr_get_data requires a non-NULL xpath; without a filter, the
	 * top-level wildcard below reads the whole datastore. */
	const char *read_xpath;
	sr_datastore_t source, target, orig_ds;
	int max_depth;
	const struct ly_ctx *ly = NULL;
	const struct ly_err_item *lerr;
	struct json_object *res = NULL, *diff_array = NULL;
	sr_data_t *tree_source = NULL, *tree_target = NULL;
	struct lyd_node *diff = NULL;
	size_t changed;
	int rc;

	/* The xpath filter is optional; when present it must be well-formed
	 * (arg_xpath fills err and returns NULL otherwise). */
	if (arg_string(args, "xpath")) {
		xpath = arg_xpath(args, err);
		if (!xpath)
			return NULL;
	}

	/* source and destination are both required, and must name different
	 * datastores: identical datastores is a usage error, not a diff. */
	if (copy_datastore_arg(args, "source", &source, &source_name, err) ||
	    copy_datastore_arg(args, "destination", &target, &target_name, err))
		return NULL;
	if (source == target) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "source and destination must be different datastores");
		return NULL;
	}

	max_depth = arg_int(args, "max_depth", 0);
	if (max_depth < 0) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
		            "max_depth must not be negative");
		return NULL;
	}

	read_xpath = (xpath && *xpath) ? xpath : "/*";

	/* Read the two subtrees from the same session, switching datastore.
	 * Both trees live in the same libyang context, so libyang can resolve
	 * schema across them. The session's original datastore is restored on
	 * every exit path (see cleanup). */
	orig_ds = sr_session_get_ds(ctx->sess);

	if ((rc = sr_session_switch_ds(ctx->sess, source)) != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_session_switch_ds");
		goto cleanup;
	}
	if ((rc = sr_get_data(ctx->sess, read_xpath, (uint32_t)max_depth,
	                      mcp_config_get()->default_timeout_ms, 0,
	                      &tree_source)) != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		goto cleanup;
	}
	if ((rc = sr_session_switch_ds(ctx->sess, target)) != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_session_switch_ds");
		goto cleanup;
	}
	if ((rc = sr_get_data(ctx->sess, read_xpath, (uint32_t)max_depth,
	                      mcp_config_get()->default_timeout_ms, 0,
	                      &tree_target)) != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_get_data");
		goto cleanup;
	}

	ly = sr_session_acquire_context(ctx->sess);

	/* sr_get_data may return no data at all (empty subtree): a NULL tree
	 * is a valid, empty side for lyd_diff_siblings(). */
	if (lyd_diff_siblings(tree_source ? tree_source->tree : NULL,
	                      tree_target ? tree_target->tree : NULL,
	                      LYD_DIFF_DEFAULTS | LYD_DIFF_META,
	                      &diff) != LY_SUCCESS) {
		lerr = ly ? ly_err_last(ly) : NULL;
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
		            "libyang diff failed: %s",
		            lerr && lerr->msg ? lerr->msg : "unknown error");
		goto cleanup;
	}

	diff_array = json_object_new_array();
	emit_diff(diff, diff_array);
	changed = json_object_array_length(diff_array);

	/* The result (and the diff values) may contain configuration secrets:
	 * never log the payload, only its size. */
	mcp_log_debug("sr_diff_config: %s -> %s (%s%s): %zu change(s)",
	              source_name, target_name, read_xpath,
	              max_depth ? "" : ", unlimited depth", changed);

	res = json_object_new_object();
	json_object_object_add(res, "diff", diff_array);
	diff_array = NULL;	/* now owned by res */
	json_object_object_add(res, "changed", json_object_new_uint64(changed));
	json_object_object_add(res, "source", json_object_new_string(source_name));
	json_object_object_add(res, "target", json_object_new_string(target_name));
	json_object_object_add(res, "xpath",
	                       json_object_new_string(xpath ? xpath : "data"));

cleanup:
	/* Restore the session's datastore (best effort) so a reused session is
	 * not left pointing at the diff target. */
	sr_session_switch_ds(ctx->sess, orig_ds);

	if (diff_array)
		json_object_put(diff_array);
	if (diff)
		lyd_free_all(diff);
	sr_release_data(tree_source);
	sr_release_data(tree_target);
	if (ly)
		sr_session_release_context(ctx->sess);

	return res;
}
