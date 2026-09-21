/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#ifndef _SYSREPO_MCP_SESSIONS_H
#define _SYSREPO_MCP_SESSIONS_H

#include <sysrepo/mcp/config.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ---------------------------------------------------------------- mcp_notif
 */

/** One slot in the per-session notification ring buffer. */
struct mcp_notif {
	char    *path;          /* data path of the notification */
	char    *json;          /* the notification tree, as libyang JSON */
	char    *kind;          /* realtime, replay, replay-complete, ... */
	int64_t  timestamp;     /* seconds since the epoch */
};

/* ---------------------------------------------------------------- mcp_subscription
 */

/** One notification subscription, linked list per session. */
struct mcp_subscription {
	struct mcp_subscription *next;
	struct mcp_session      *owner;
	uint32_t                 id;
	char                    *module;
	char                    *xpath;
	sr_subscription_ctx_t   *sub;
	uint64_t                 received;
};

/* ---------------------------------------------------------------- mcp_session
 */

/** Per-client session: sysrepo session, subscriptions, notification queue. */
struct mcp_session {
	int      in_use;
	char     id[CONFIG_SYSREPO_MCP_SERVER_SESSION_ID_LEN];
	time_t   created;
	time_t   last_activity;

	/* sysrepo session owning every notification subscription below. It
	 * must outlive them, so it is stopped only when the session dies. */
	sr_session_ctx_t        *sr_sess;
	struct mcp_subscription *subs;
	uint32_t                 next_sub_id;

	/* Ring buffer of notifications waiting to be polled. */
	struct mcp_notif queue[CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE];
	size_t           head;
	size_t           count;
	uint64_t         total_received;
	uint64_t         total_dropped;
};

/* ------------------------------------------------------------------- globals */

extern struct mcp_session g_sessions[CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS];
extern unsigned g_session_count;

void session_drain(struct mcp_session *session);
void session_destroy(struct mcp_session *session);
void notif_clear(struct mcp_notif *notif);
void sessions_expire(void);
struct mcp_session *session_create(void);
void session_generate_id(char *out);
struct mcp_session *session_find(const char *id);
void sessions_process_events(void);
void session_push_notif(struct mcp_session *session,
			       const char *path, const char *json,
			       const char *kind, time_t timestamp);
const char *notif_kind_str(uint32_t type);
void notif_callback(sr_session_ctx_t *session, uint32_t sub_id,
		    const sr_ev_notif_type_t type, const struct lyd_node *notif,
		    struct timespec *timestamp, void *private_data);
int session_sysrepo(struct mcp_session *sess, struct mcp_err *err);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_SESSIONS_H */
