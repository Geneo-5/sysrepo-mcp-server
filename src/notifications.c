/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Notification tools: subscribe, unsubscribe, list_subscriptions, poll, send.
 */

#include <sys/time.h>

#include <json-c/json.h>

#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/sessions.h>
#include <sysrepo/mcp/notifications.h>

/* ---------------------------------------------------------------- subscription_find
 *
 * Search a session's subscription list by ID. Returns the subscription, or
 * NULL if not found.
 */

static struct mcp_subscription *
subscription_find(struct mcp_session *sess, int id)
{
	struct mcp_subscription *sub;

	for (sub = sess->subs; sub; sub = sub->next)
		if (sub->id == (uint32_t)id)
			return sub;

	return NULL;
}

/*
 * Require an MCP session, and say how to get one.
 *
 * Many notification tools need an active MCP session. If the tool is called
 * without one (sessionless mode), we return an error telling the agent how
 * to create one via `initialize`.
 */

static int
need_session(struct tool_ctx *ctx, struct mcp_err *err, const char *tool)
{
	if (ctx->mcp)
		return 0;

	mcp_err_set(err, MCP_ERR_NO_SESSION, "No session",
		    "\"%s\" requires an MCP session; "
		    "call initialize first", tool);
	return -1;
}

/* ---------------------------------------------------------------- sr_notif_subscribe
 *
 * Create a notification subscription. Supports replay via `replay_start`
 * (epoch seconds). The subscription is created with SR_SUBSCR_NO_THREAD so
 * that sysrepo does not spawn a thread; instead, `sr_subscription_process_events()`
 * is called once per request (in `serve()`).
 */

struct json_object *
tool_sr_notif_subscribe(struct tool_ctx *ctx, struct json_object *args,
                        struct mcp_err *err)
{
	const char              *module;
	const char              *xpath;
	struct mcp_subscription *sub;
	struct json_object      *res;
	struct timespec          start;
	struct timespec         *start_ptr = NULL;
	int64_t                  replay_start;
	int                      rc;

	module = arg_string(args, "module");
	if (!module) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
			    "module is required");
		return NULL;
	}

	if (need_session(ctx, err, "sr_notif_subscribe"))
		return NULL;
	xpath = arg_string(args, "xpath");

	if (xpath && !xpath_wellformed(xpath)) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
			    "xpath must be absolute and carry a module prefix; "
			    "got \"%s\"", xpath);
		return NULL;
	}

	/* Replaying history needs the module to have replay support enabled
	 * in sysrepo; without it the subscription starts at "now". */
	replay_start = arg_int64(args, "replay_start", 0);
	if (replay_start > 0) {
		start.tv_sec = (time_t)replay_start;
		start.tv_nsec = 0;
		start_ptr = &start;
	}

	if (session_sysrepo(ctx->mcp, err))
		return NULL;

	sub = calloc(1, sizeof(*sub));
	if (!sub) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
			    "out of memory");
		return NULL;
	}

	sub->owner = ctx->mcp;
	sub->id = ++ctx->mcp->next_sub_id;
	sub->module = strdup(module);
	sub->xpath = xpath ? strdup(xpath) : NULL;

	/* SR_SUBSCR_NO_THREAD: sysrepo must not call back on a thread of its
	 * own. Events are drained at the start of each request instead, which
	 * keeps the server single-threaded and the queue free of locking. */
	rc = sr_notif_subscribe_tree(ctx->mcp->sr_sess, module, xpath,
	                             start_ptr, NULL, notif_callback, sub,
	                             SR_SUBSCR_NO_THREAD, &sub->sub);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->mcp->sr_sess, rc,
		                     "sr_notif_subscribe_tree");
		free(sub->module);
		free(sub->xpath);
		free(sub);
		ctx->mcp->next_sub_id--;
		return NULL;
	}

	sub->next = ctx->mcp->subs;
	ctx->mcp->subs = sub;

	res = json_object_new_object();
	json_object_object_add(res, "subscription_id",
	                       json_object_new_int64((int64_t)sub->id));
	json_object_object_add(res, "module", json_object_new_string(module));
	if (xpath)
		json_object_object_add(res, "xpath",
		                       json_object_new_string(xpath));
	json_object_object_add(res, "session_id",
	                       json_object_new_string(ctx->mcp->id));

	return res;
}

/* ----------------------------------------------------- sr_notif_unsubscribe
 *
 * Remove one or all subscriptions. When `subscription_id` is > 0, only that
 * subscription is removed. Otherwise all subscriptions are removed.
 *
 * sr_unsubscribe triggers the callback with SR_EV_NOTIF_TERMINATED, which
 * queues a {"kind": "terminated", ...} event. Drain it: the agent should not
 * see internal lifecycle events from explicit unsubscribes.
 */

struct json_object *
tool_sr_notif_unsubscribe(struct tool_ctx *ctx, struct json_object *args,
                          struct mcp_err *err)
{
	struct mcp_subscription  *sub;
	struct mcp_subscription **link;
	struct json_object       *res;
	int                       removed = 0;
	int                       id = arg_int(args, "subscription_id", 0);

	if (need_session(ctx, err, "sr_notif_unsubscribe"))
		return NULL;

	if (id > 0) {
		sub = subscription_find(ctx->mcp, id);
		if (!sub) {
			mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
				    "no subscription %d on this session", id);
			return NULL;
		}
	}

	link = &ctx->mcp->subs;
	while (*link) {
		sub = *link;

		if (id > 0 && sub->id != (uint32_t)id) {
			link = &sub->next;
			continue;
		}

		*link = sub->next;
		if (sub->sub) {
			sr_unsubscribe(sub->sub);

			/* sr_unsubscribe triggers the callback with
			 * SR_EV_NOTIF_TERMINATED, which queues a
			 * {"kind": "terminated", ...} event.  Drain it: the
			 * agent should not see internal lifecycle events from
			 * explicit unsubscribes. */
			while (ctx->mcp->count > 0) {
				size_t index = ctx->mcp->head %
				               CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE;
				if (strcmp(
				    ctx->mcp->queue[index].kind,
				    "terminated") != 0)
					break;
				notif_clear(
				    &ctx->mcp->queue[index]);
				ctx->mcp->head =
				    (ctx->mcp->head + 1) %
				    CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE;
				ctx->mcp->count--;
			}
		}
		free(sub->module);
		free(sub->xpath);
		free(sub);
		removed++;
	}

	res = json_object_new_object();
	json_object_object_add(res, "removed", json_object_new_int(removed));

	return res;
}

/* ------------------------------------------------ sr_notif_list_subscriptions
 *
 * List all subscriptions on the current session. Returns an array of
 * subscription objects, each with subscription_id, module, xpath (if any),
 * and received count.
 */

struct json_object *
tool_sr_notif_list_subscriptions(struct tool_ctx *ctx,
                                 struct json_object *args,
                                 struct mcp_err *err)
{
	struct mcp_subscription *sub;
	struct json_object      *res;
	struct json_object      *list;

	(void)args;

	if (need_session(ctx, err, "sr_notif_list_subscriptions"))
		return NULL;

	list = json_object_new_array();

	for (sub = ctx->mcp->subs; sub; sub = sub->next) {
		struct json_object *entry = json_object_new_object();

		json_object_object_add(entry, "subscription_id",
		                       json_object_new_int64((int64_t)sub->id));
		json_object_object_add(entry, "module",
		                       json_object_new_string(sub->module));
		if (sub->xpath)
			json_object_object_add(entry, "xpath",
			                       json_object_new_string(sub->xpath));
		json_object_object_add(entry, "received",
		                       json_object_new_int64(
		                               (int64_t)sub->received));
		json_object_array_add(list, entry);
	}

	res = json_object_new_object();
	json_object_object_add(res, "subscriptions", list);
	json_object_object_add(res, "pending",
	                       json_object_new_int((int)ctx->mcp->count));
	json_object_object_add(res, "session_id",
	                       json_object_new_string(ctx->mcp->id));

	return res;
}

/* ------------------------------------------------------------ sr_notif_poll
 *
 * Everything received since the last call. Draining is the default: an agent
 * calling it in a loop must not be handed the same event twice.
 */

struct json_object *
tool_sr_notif_poll(struct tool_ctx *ctx, struct json_object *args,
                   struct mcp_err *err)
{
	struct json_object *res;
	struct json_object *list;
	int                 max = arg_int(args, "max", 0);
	int                 peek = arg_bool(args, "peek", 0);
	size_t              taken = 0;
	size_t              available;

	if (need_session(ctx, err, "sr_notif_poll"))
		return NULL;
	if (max < 0) {
		mcp_err_set(err, MCP_ERR_PARAMS, "Invalid params",
			    "max must not be negative");
		return NULL;
	}

	available = ctx->mcp->count;
	if (max > 0 && (size_t)max < available)
		available = (size_t)max;

	list = json_object_new_array();

	while (taken < available) {
		size_t            index = (ctx->mcp->head + taken) %
		                          CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE;
		struct mcp_notif *notif = &ctx->mcp->queue[index];
		struct json_object *entry = json_object_new_object();

		if (notif->path)
			json_object_object_add(entry, "xpath",
			                       json_object_new_string(notif->path));
		json_object_object_add(entry, "kind",
		                       json_object_new_string(
		                               notif->kind ? notif->kind :
		                               "unknown"));
		json_object_object_add(entry, "timestamp",
		                       json_object_new_int64(notif->timestamp));

		if (notif->json) {
			struct json_object *data =
			        json_tokener_parse(notif->json);

			json_object_object_add(entry, "data",
			                       data ? data :
			                       json_object_new_object());
		}

		json_object_array_add(list, entry);
		taken++;
	}

	if (!peek) {
		size_t i;

		for (i = 0; i < taken; i++)
			notif_clear(&ctx->mcp->queue[(ctx->mcp->head + i) %
			                             CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE]);

		ctx->mcp->head = (ctx->mcp->head + taken) % CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE;
		ctx->mcp->count -= taken;
	}

	res = json_object_new_object();
	json_object_object_add(res, "notifications", list);
	json_object_object_add(res, "returned", json_object_new_int((int)taken));
	json_object_object_add(res, "pending",
	                       json_object_new_int((int)ctx->mcp->count));
	json_object_object_add(res, "total_received",
	                       json_object_new_int64(
	                               (int64_t)ctx->mcp->total_received));
	/* Non-zero means the queue overflowed: the agent is polling too
	 * slowly, or the filter is too wide. */
	json_object_object_add(res, "dropped",
	                       json_object_new_int64(
	                               (int64_t)ctx->mcp->total_dropped));

	return res;
}

/* ------------------------------------------------------------ sr_notif_send
 *
 * Emitting a notification is what makes the subscription tools testable
 * without waiting for a device to produce one on its own.
 *
 * wait = 0 on purpose. Waiting would block on this process's own
 * subscriptions, whose events are only processed between requests:
 * the server would deadlock against itself.
 */

struct json_object *
tool_sr_notif_send(struct tool_ctx *ctx, struct json_object *args,
                   struct mcp_err *err)
{
	const char          *xpath = arg_xpath(args, err);
	struct json_object  *input;
	const struct ly_ctx *ly;
	struct lyd_node     *notif = NULL;
	struct json_object  *res;
	int                  rc;

	if (!xpath)
		return NULL;

	input = arg_object(args, "input");

	ly = sr_session_acquire_context(ctx->sess);
	if (!ly) {
		mcp_err_set(err, MCP_ERR_INTERNAL, "Internal error",
			    "no libyang context on the session");
		return NULL;
	}

	if (lyd_new_path(NULL, ly, xpath, NULL, 0, &notif) != LY_SUCCESS) {
		mcp_err_set(err, MCP_ERR_NOT_FOUND, "Not found",
			    "cannot resolve \"%s\": %s", xpath,
			    ly_err_last(ly));
		sr_session_release_context(ctx->sess);
		return NULL;
	}

	if (input && json_object_is_type(input, json_type_object)) {

		json_object_object_foreach(input, key, val) {
			const char *text = json_object_get_string(val);

			if (lyd_new_path(notif, NULL, key, text, 0, NULL) !=
			    LY_SUCCESS) {
				mcp_err_set(err, MCP_ERR_VALIDATION,
					    "Validation failed",
					    "input \"%s\" rejected: %s", key,
					    ly_err_last(ly));
				lyd_free_all(notif);
				sr_session_release_context(ctx->sess);
				return NULL;
			}
		}
	}

	sr_session_release_context(ctx->sess);

	/* wait = 0 on purpose. Waiting would block on this process's own
	 * subscriptions, whose events are only processed between requests:
	 * the server would deadlock against itself. */
	rc = sr_notif_send_tree(ctx->sess, notif, 0, 0);
	lyd_free_all(notif);

	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, ctx->sess, rc, "sr_notif_send_tree");
		return NULL;
	}

	res = json_object_new_object();
	json_object_object_add(res, "ok", json_object_new_boolean(1));
	json_object_object_add(res, "xpath", json_object_new_string(xpath));

	return res;
}
