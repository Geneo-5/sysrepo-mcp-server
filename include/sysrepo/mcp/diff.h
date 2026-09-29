/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 */

#ifndef SYSREPO_MCP_DIFF_H
#define SYSREPO_MCP_DIFF_H

#include <json-c/json.h>

struct tool_ctx;
struct mcp_err;

/*
 * sr_diff_config: read two datastore subtrees (default running -> candidate),
 * compute the libyang diff between them, and return a flat list of the
 * leaf-level changes. See sphinx/api.rst.
 */
struct json_object *tool_sr_diff_config(struct tool_ctx *ctx,
                                        struct json_object *args,
                                        struct mcp_err *err);

#endif
