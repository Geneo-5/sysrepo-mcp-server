/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 *****************************************************************************/

#ifndef _SYSREPO_MCP_LOG_H
#define _SYSREPO_MCP_LOG_H

#include <elog/elog.h>

struct mcp_config;

int mcp_log_init(const struct mcp_config *config);
void mcp_log_close(void);
void mcp_log(enum elog_severity severity, const char *format, ...);

#define mcp_log_emerg(...)  mcp_log(ELOG_EMERG_SEVERITY, __VA_ARGS__)
#define mcp_log_alert(...)  mcp_log(ELOG_ALERT_SEVERITY, __VA_ARGS__)
#define mcp_log_crit(...)   mcp_log(ELOG_CRIT_SEVERITY, __VA_ARGS__)
#define mcp_log_err(...)    mcp_log(ELOG_ERR_SEVERITY, __VA_ARGS__)
#define mcp_log_warn(...)   mcp_log(ELOG_WARNING_SEVERITY, __VA_ARGS__)
#define mcp_log_notice(...) mcp_log(ELOG_NOTICE_SEVERITY, __VA_ARGS__)
#define mcp_log_info(...)   mcp_log(ELOG_INFO_SEVERITY, __VA_ARGS__)
#define mcp_log_debug(...)  mcp_log(ELOG_DEBUG_SEVERITY, __VA_ARGS__)

#endif /* !_SYSREPO_MCP_LOG_H */
