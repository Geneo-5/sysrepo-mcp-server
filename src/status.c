/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Server status tool: version, uptime, session counters.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <json-c/json.h>

#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/sessions.h>
#include <sysrepo/mcp/status.h>

/* External state from main.c. */
extern time_t g_start_time;
extern unsigned g_session_count;

/* ------------------------------------------------------------------ get_status
 *
 * Returns server health: version, uptime, session counters, and optionally a
 * verbose list of active sessions with their subscription counts.
 */
struct json_object *
tool_get_status(struct tool_ctx *ctx, struct json_object *args,
                struct mcp_err *err)
{
	struct json_object *res;

	(void)err;

	res = json_object_new_object();
	json_object_object_add(res, "version",
	                       json_object_new_string(PACKAGE_VERSION));
	json_object_object_add(res, "uptime_seconds",
	                       json_object_new_int64(
		                               (int64_t)(time(NULL) - g_start_time)));
	json_object_object_add(res, "active_sessions",
	                       json_object_new_int((int)g_session_count));
	json_object_object_add(res, "max_sessions",
	                       json_object_new_int(CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS));
	json_object_object_add(res, "session_ttl_seconds",
	                       json_object_new_int(CONFIG_SYSREPO_MCP_SERVER_SESSION_TTL));

	if (arg_bool(args, "verbose", 0)) {
		struct json_object *list = json_object_new_array();
		time_t              now = time(NULL);
		size_t              i;

		for (i = 0; i < CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS; i++) {
			struct mcp_session *sess = &g_sessions[i];
			struct json_object *entry;
			struct mcp_subscription *sub;
			unsigned            subs = 0;

			if (!sess->in_use)
				continue;

			for (sub = sess->subs; sub; sub = sub->next)
				subs++;

			entry = json_object_new_object();
			json_object_object_add(entry, "session_id",
			                       json_object_new_string(sess->id));
			json_object_object_add(entry, "created",
			                       json_object_new_int64(
		                               (int64_t)sess->created));
			json_object_object_add(entry, "idle_seconds",
			                       json_object_new_int64(
		                               (int64_t)(now - sess->last_activity)));
			json_object_object_add(entry, "subscriptions",
			                       json_object_new_int((int)subs));
			json_object_object_add(entry, "pending_notifications",
			                       json_object_new_int((int)sess->count));
			json_object_object_add(entry, "current",
			                       json_object_new_boolean(
		                               ctx->mcp == sess));
			json_object_array_add(list, entry);
		}

		json_object_object_add(res, "sessions", list);
	}

	return res;
}
