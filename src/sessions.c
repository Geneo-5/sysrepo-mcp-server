/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************
 *
 * Session management: create, find, destroy, and expire sessions.
 * Notification ring buffer, subscription lifecycle, and sysrepo notification
 * callbacks.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <json-c/json.h>

#include <sysrepo.h>

#include <sysrepo/mcp/utilities.h>
#include <sysrepo/mcp/sessions.h>

/* --------------------------------------------------------------------- globals
 *
 * These variables are declared `extern` in main.c as well; sessions.c owns
 * the storage. External modules only read g_sessions via the declarations in
 * main.c.
 */

/* ----------------------------------------------------------------------- globals */

struct mcp_session               g_sessions[CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS];
unsigned                         g_session_count;

/* ------------------------------------------------------------------ notif_clear
 *
 * Free the heap-allocated fields of a ring buffer slot. The slot itself
 * (embedded in `mcp_session.queue`) is reused, not freed.
 */

void
notif_clear(struct mcp_notif *notif)
{
	free(notif->path);
	free(notif->json);
	free(notif->kind);
	notif->path = NULL;
	notif->json = NULL;
	notif->kind = NULL;
	notif->timestamp = 0;
}

/* ------------------------------------------------------------------- session_drain
 *
 * Clear every notification in the ring buffer. Used when a session is
 * destroyed so that stale notifications do not leak.
 */

void
session_drain(struct mcp_session *sess)
{
	size_t i;

	for (i = 0; i < sess->count; i++)
		notif_clear(&sess->queue[(sess->head + i) % CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE]);

	sess->head = 0;
	sess->count = 0;
}

/* ------------------------------------------------------------------ session_destroy
 *
 * Unsubscribe every subscription, stop the sysrepo session, drain the ring
 * buffer, and reset all state. Must be called before process exit for every
 * active session.
 */

void
session_destroy(struct mcp_session *sess)
{
	struct mcp_subscription *sub;
	struct mcp_subscription *next;

	if (!sess->in_use)
		return;

	/* Unsubscribe before stopping the sysrepo session: a subscription
	 * outliving the session it was made on is undefined. */
	for (sub = sess->subs; sub; sub = next) {
		next = sub->next;
		if (sub->sub)
			sr_unsubscribe(sub->sub);
		free(sub->module);
		free(sub->xpath);
		free(sub);
	}
	sess->subs = NULL;

	if (sess->sr_sess) {
		sr_session_stop(sess->sr_sess);
		sess->sr_sess = NULL;
	}

	session_drain(sess);

	sess->in_use = 0;
	sess->id[0] = '\0';
	sess->next_sub_id = 0;
	sess->total_received = 0;
	sess->total_dropped = 0;

	if (g_session_count)
		g_session_count--;
}

/* ------------------------------------------------------------------- sessions_expire
 *
 * Expire sessions that have been idle longer than `CONFIG_SYSREPO_MCP_SERVER_SESSION_TTL`. Called
 * once per request (in `serve()`) so that timeout is enforced regardless of
 * whether the client explicitly DELETEs.
 */

void
sessions_expire(void)
{
	time_t now = time(NULL);
	size_t i;

	for (i = 0; i < CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS; i++) {
		if (!g_sessions[i].in_use)
			continue;
		if (now - g_sessions[i].last_activity < CONFIG_SYSREPO_MCP_SERVER_SESSION_TTL)
			continue;

		fprintf(stderr, PACKAGE_NAME ": session %s expired\n",
		        g_sessions[i].id);
		session_destroy(&g_sessions[i]);
	}
}

/* ---------------------------------------------------------------- session_generate_id
 *
 * 32 hex characters (128 bits of randomness) derived from /dev/urandom. Falls
 * back to an LCG seeded from time() + getpid() + a counter if /dev/urandom
 * is unavailable. Predictability means anyone can guess a session ID and use
 * another agent's session.
 */

void
session_generate_id(char *out)
{
	static uint64_t counter;
	unsigned char   raw[16];
	FILE           *urandom;
	size_t          got = 0;
	size_t          i;

	urandom = fopen("/dev/urandom", "rb");
	if (urandom) {
		got = fread(raw, 1, sizeof(raw), urandom);
		fclose(urandom);
	}

	if (got != sizeof(raw)) {
		/* Never expected. Still better than a predictable identifier:
		 * anyone able to guess one can use another agent's session. */
		uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32)
		                ^ ++counter;

		for (i = 0; i < sizeof(raw); i++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			raw[i] = (unsigned char)(seed >> 33);
		}
	}

	for (i = 0; i < sizeof(raw); i++)
		snprintf(out + 2 * i, 3, "%02x", raw[i]);

	out[2 * sizeof(raw)] = '\0';
}

/* -------------------------------------------------------------------- session_create
 *
 * Find a free slot in `g_sessions`, initialise it, and return it. Returns NULL
 * when all slots are in use (which is not a bug — the client should receive
 * 503).
 */

struct mcp_session *
session_create(void)
{
	size_t i;

	for (i = 0; i < CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS; i++) {
		struct mcp_session *sess = &g_sessions[i];

		if (sess->in_use)
			continue;

		memset(sess, 0, sizeof(*sess));
		session_generate_id(sess->id);
		sess->in_use = 1;
		sess->created = time(NULL);
		sess->last_activity = sess->created;
		g_session_count++;

		return sess;
	}

	return NULL;
}

/* -------------------------------------------------------------------- session_find
 *
 * Linear search through `g_sessions` for a session matching `id`. Returns NULL
 * if not found or `id` is NULL/empty.
 */

struct mcp_session *
session_find(const char *id)
{
	size_t i;

	if (!id || !*id)
		return NULL;

	for (i = 0; i < CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS; i++) {
		if (g_sessions[i].in_use && !strcmp(g_sessions[i].id, id))
			return &g_sessions[i];
	}

	return NULL;
}

/* ---------------------------------------------------------------- sessions_process_events
 *
 * Run every pending notification callback, on this thread.
 *
 * Subscriptions are created with SR_SUBSCR_NO_THREAD, so sysrepo never calls
 * back on its own; without this, events would simply pile up in the pipe.
 * Calling it once per request means a poll always sees everything that had
 * arrived when the request came in.
 */

void
sessions_process_events(void)
{
	struct mcp_subscription *sub;
	size_t                   i;

	for (i = 0; i < CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS; i++) {
		if (!g_sessions[i].in_use)
			continue;

		for (sub = g_sessions[i].subs; sub; sub = sub->next) {
			if (sub->sub)
				sr_subscription_process_events(sub->sub, NULL,
				                               NULL);
		}
	}
}

/* ---------------------------------------------------------------- session_push_notif
 *
 * Append a notification to the ring buffer. If the buffer is full, drop the
 * oldest entry (so that the most recent events are preserved) and update
 * `total_dropped`. Returns nothing; the caller does not check the return value.
 */

void
session_push_notif(struct mcp_session *sess, const char *path, const char *json,
                   const char *kind, time_t timestamp)
{
	struct mcp_notif *slot;

	if (sess->count == CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE) {
		/* A full queue means the agent stopped polling. Dropping the
		 * oldest keeps the most recent events, which are the ones it
		 * is behind on; the count is reported so it knows. */
		notif_clear(&sess->queue[sess->head]);
		sess->head = (sess->head + 1) % CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE;
		sess->count--;
		sess->total_dropped++;
	}

	slot = &sess->queue[(sess->head + sess->count) % CONFIG_SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE];
	slot->path = path ? strdup(path) : NULL;
	slot->json = json ? strdup(json) : NULL;
	slot->kind = kind ? strdup(kind) : NULL;
	slot->timestamp = timestamp;

	sess->count++;
	sess->total_received++;
}

/* ------------------------------------------------------------------ notif_kind_str
 *
 * Map a sysrepo notification type enum to the string name that the agent
 * expects. Every value is lowercased so that matching is case-insensitive.
 */

const char *
notif_kind_str(sr_ev_notif_type_t type)
{
	switch (type) {
	case SR_EV_NOTIF_REALTIME:        return "realtime";
	case SR_EV_NOTIF_REPLAY:          return "replay";
	case SR_EV_NOTIF_REPLAY_COMPLETE: return "replay-complete";
	case SR_EV_NOTIF_TERMINATED:      return "terminated";
	case SR_EV_NOTIF_MODIFIED:        return "modified";
	case SR_EV_NOTIF_SUSPENDED:       return "suspended";
	case SR_EV_NOTIF_RESUMED:         return "resumed";
	default:                          return "unknown";
	}
}

/* ------------------------------------------------------------------- notif_callback
 *
 * sysrepo calls this when a notification arrives. The notification tree is
 * serialised to JSON (LYD_JSON + LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK) and
 * queued in the session's ring buffer. The `path` is extracted with
 * lyd_path().
 *
 * For lifecycle events (replay-complete, terminated, suspended, resumed) the
 * `notif` pointer is NULL; they are queued without a json blob so that an
 * agent replaying history knows where the replay ends.
 */

void
notif_callback(sr_session_ctx_t *session, uint32_t sub_id,
               const sr_ev_notif_type_t type, const struct lyd_node *notif,
               struct timespec *timestamp, void *private_data)
{
	struct mcp_subscription *sub = private_data;
	char                    *json = NULL;
	char                    *path = NULL;

	(void)session;
	(void)sub_id;

	if (!sub || !sub->owner || !sub->owner->in_use)
		return;

	/* notif is NULL for the lifecycle events: replay-complete,
	 * terminated, suspended, resumed. They are queued too, because an
	 * agent replaying history needs to know where the replay ends. */
	if (notif) {
		if (lyd_print_mem(&json, notif, LYD_JSON,
		                  LYD_PRINT_SIBLINGS | LYD_PRINT_SHRINK) !=
		    LY_SUCCESS)
			json = NULL;

		path = lyd_path(notif, LYD_PATH_STD, NULL, 0);
	}

	sub->received++;
	session_push_notif(sub->owner, path, json, notif_kind_str(type),
	                   timestamp ? (int64_t)timestamp->tv_sec :
	                   (int64_t)time(NULL));
}

/* ---------------------------------------------------------------- session_sysrepo
 *
 * Create (lazily) the sysrepo session that a session's subscriptions are made
 * on. Returns 0 on success, -1 on failure. The session is created once, on
 * the first subscription, and reused for all subsequent subscriptions on that
 * MCP session.
 */

int
session_sysrepo(struct mcp_session *sess, struct mcp_err *err)
{
	int rc;

	if (sess->sr_sess)
		return 0;

	rc = sr_session_start(g_conn, SR_DS_RUNNING, &sess->sr_sess);
	if (rc != SR_ERR_OK) {
		mcp_err_from_session(err, NULL, rc, "sr_session_start");
		sess->sr_sess = NULL;
		return -1;
	}

	return 0;
}
