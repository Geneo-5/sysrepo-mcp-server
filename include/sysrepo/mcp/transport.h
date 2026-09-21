/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#ifndef _SYSREPO_MCP_TRANSPORT_H
#define _SYSREPO_MCP_TRANSPORT_H

#include <sysrepo/mcp/config.h>
#include <sysrepo/mcp/utilities.h>
#include <fcgiapp.h>

/* MCP Streamable HTTP transport constants. */

/** MCP protocol version string (Streamable HTTP binding). */
#ifndef MCP_PROTOCOL_VERSION
#define MCP_PROTOCOL_VERSION "2025-01-13"
#endif

/** Maximum allowed request body size (1 MiB). */
#ifndef MCP_MAX_BODY
#define MCP_MAX_BODY (1024 * 1024)
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ------------------------------------------------------------------- http_send
 *
 * Write an HTTP response with a JSON content type and the given body.
 */

void http_send(FCGX_Request *req, int status, const char *extra_headers,
	       const char *body);

/* ---------------------------------------------------------------------- serve
 *
 * Read request body, dispatch to the appropriate MCP method handler.
 */

void serve(FCGX_Request *req);

/* ------------------------------------------------------------------- dispatch
 *
 * Parse a JSON-RPC request and dispatch to the appropriate handler.
 */

void dispatch(FCGX_Request *req, const char *body, size_t len,
	      struct mcp_session *mcp);

/* ------------------------------------------------------------------- stopping
 *
 * Volatile flag set by on_signal() to break the FCGX_Accept_r loop.
 */

extern volatile sig_atomic_t stopping;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_TRANSPORT_H */
