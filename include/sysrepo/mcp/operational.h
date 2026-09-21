/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#ifndef _SYSREPO_MCP_OPERATIONAL_H
#define _SYSREPO_MCP_OPERATIONAL_H

#include <sysrepo/mcp/config.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct json_object *tool_sr_get_operational(struct tool_ctx *ctx,
					  struct json_object *args,
					  struct mcp_err *err);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_OPERATIONAL_H */
