/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#ifndef _SYSREPO_MCP_UTILITIES_H
#define _SYSREPO_MCP_UTILITIES_H

#include <sysrepo/mcp/config.h>
#include <json-c/json.h>
#include <sysrepo.h>

/* JSON-RPC 2.0 error codes (specification) plus sysrepo-specific custom codes. */

/** Parse error: invalid JSON was received by the server. */
#ifndef MCP_ERR_PARSE
#define MCP_ERR_PARSE             (-32700)
#endif

/** Invalid request: the JSON sent is not a valid request object. */
#ifndef MCP_ERR_INVALID_REQUEST
#define MCP_ERR_INVALID_REQUEST   (-32600)
#endif

/** Method not found: the method named does not exist. */
#ifndef MCP_ERR_METHOD
#define MCP_ERR_METHOD            (-32601)
#endif

/** Invalid params: one or more argument errors. */
#ifndef MCP_ERR_PARAMS
#define MCP_ERR_PARAMS            (-32602)
#endif

/** Internal error: an unexpected server-side failure. */
#ifndef MCP_ERR_INTERNAL
#define MCP_ERR_INTERNAL          (-32603)
#endif

/* sysrepo-specific error extensions. */

/** Server error: NACM denied, module not installed, or operation unsupported. */
#ifndef MCP_ERR_SERVER
#define MCP_ERR_SERVER            (-32000)
#endif

/** Not found: the requested resource does not exist. */
#ifndef MCP_ERR_NOT_FOUND
#define MCP_ERR_NOT_FOUND         (-32001)
#endif

/** Validation error: data does not conform to the schema. */
#ifndef MCP_ERR_VALIDATION
#define MCP_ERR_VALIDATION        (-32002)
#endif

/** Denied: NACM access control denied the operation. */
#ifndef MCP_ERR_DENIED
#define MCP_ERR_DENIED            (-32003)
#endif

/** Locked: the resource is currently locked. */
#ifndef MCP_ERR_LOCKED
#define MCP_ERR_LOCKED            (-32004)
#endif

/** Timeout: the operation did not complete within the allotted time. */
#ifndef MCP_ERR_TIMEOUT
#define MCP_ERR_TIMEOUT           (-32005)
#endif

/** Unsupported: the requested operation is not supported. */
#ifndef MCP_ERR_UNSUPPORTED
#define MCP_ERR_UNSUPPORTED       (-32006)
#endif

/** No session: the session identifier is unknown or has expired. */
#ifndef MCP_ERR_NO_SESSION
#define MCP_ERR_NO_SESSION        (-32008)
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* -------------------------------------------------------------------- mcp_err
 */

/** JSON-RPC / MCP error accumulator. */
struct mcp_err {
	int  code;
	char message[128];
	char detail[512];
	int  sr_code;		/* SR_ERR_* when the failure came from sysrepo */
};

/** Map a sysrepo return code onto a JSON-RPC code. */
int mcp_code_from_sr(int rc);

/** Fill in a mcp_err from a sysrepo return code. */
void mcp_err_set(struct mcp_err *err, int code, const char *message,
		 const char *detail, ...);

/** Fill in a mcp_err from a sysrepo session and return code. */
void mcp_err_from_session(struct mcp_err *err, sr_session_ctx_t *sess, int rc,
			  const char *op);

/** Serialise a libyang tree to a json-c object. NULL tree → empty object. */
struct json_object *tree_to_json(const struct lyd_node *tree,
				 struct mcp_err *err);

/** Return the lowercased node type string ("RPC" → "rpc"). */
struct json_object *nodetype_to_json(uint16_t nodetype);

/* Argument extraction helpers — all return NULL / fallback on failure. */

const char *arg_string(struct json_object *args, const char *name);
struct json_object *arg_object(struct json_object *args, const char *name);
int arg_int(struct json_object *args, const char *name, int fallback);
int64_t arg_int64(struct json_object *args, const char *name, int64_t fallback);
int arg_bool(struct json_object *args, const char *name, int fallback);

/** Validate that an XPath is absolute and has a valid module prefix. */
int xpath_wellformed(const char *xpath);

/** Fetch and validate an xpath argument (required pattern). Returns NULL on
 * failure, or the xpath string on success. */
const char *arg_xpath(struct json_object *args, struct mcp_err *err);

/** Resolve a datastore name string to an enum (defaults to SR_DS_RUNNING).
 * Returns 0 on success, -1 on failure. */
int arg_datastore(struct json_object *args, sr_datastore_t *out,
		  struct mcp_err *err);

/* ------------------------------------------------------------------ tool_ctx
 */

/* Per-request sysrepo session and MCP session identifier.
 * Declared inline here since every module uses it. */
struct tool_ctx {
	sr_session_ctx_t   *sess;	/* per-request sysrepo session, NULL when
				 * sessionless (status, modules, schema) */
	struct mcp_session *mcp;	/* MCP session, NULL when sessionless */
};

/** Handler type for all tool implementations. */
typedef struct json_object *(*tool_fn)(struct tool_ctx *ctx,
				     struct json_object *args,
				     struct mcp_err *err);

/* ---------------------------------------------------------------- tool_desc
 */

/* Tool catalogue descriptor — defined in main.c alongside the catalogue. */
struct tool_desc {
	const char *name;
	const char *description;
	const char *schema;	/* JSON Schema, as a literal */
	tool_fn     handler;
	int         needs_session;	/* needs a per-request sysrepo session */
};

/* Look up a tool by name (linear search). */
const struct tool_desc *tool_find(const char *name);

/* The global tool catalogue — defined in main.c. */
extern const struct tool_desc tools[];
extern const size_t TOOL_COUNT;

/* ------------------------------------------------------------------ sysrepo
 */

/* Global sysrepo connection and server start time, defined in main.c. */
extern sr_conn_ctx_t         *g_conn;
extern time_t                 g_start_time;

/* ---------------------------------------------------------------- tool_content
 */

/** Wrap a tool payload in the MCP content envelope. */
struct json_object *tool_content(struct json_object *payload,
				 int is_error);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_UTILITIES_H */
