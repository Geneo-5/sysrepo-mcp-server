/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#ifndef _SYSREPO_MCP_SCHEMA_H
#define _SYSREPO_MCP_SCHEMA_H

#include "config.h"
#include <sysrepo.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct json_object *schema_node_to_json(const struct lysc_node *node,
					int with_desc, int depth,
					int effective_depth,
					struct json_object *flat,
					const char *parent_path);
struct json_object *tool_get_tree(struct tool_ctx *ctx,
				  struct json_object *args,
				  struct mcp_err *err);
const char *basetype_name(LY_DATA_TYPE type);
struct json_object *tool_get_help(struct tool_ctx *ctx,
				  struct json_object *args,
				  struct mcp_err *err);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_SCHEMA_H */
