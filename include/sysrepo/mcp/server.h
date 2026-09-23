/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

/**
 * @file
 * @brief Public interface of the sysrepo-mcp server.
 *
 * sysrepo-mcp currently ships as a single executable, whose whole
 * implementation lives in src/main.c. Nothing is exported yet, so this header
 * declares nothing.
 *
 * It is kept because Doxygen extracts include/ into sphinx/_doxygen/xml for
 * Breathe, and because the split announced in the roadmap (splitting the
 * FastCGI transport, the JSON-RPC layer and the sysrepo tools into separate
 * translation units) will need somewhere to declare the interfaces between
 * them.
 *
 * Adding a declaration here is not enough on its own: ebuild.mk builds a
 * single object, src/main.o, and must gain the new sources at the same time.
 */

#ifndef _SYSREPO_MCP_SERVER_H
#define _SYSREPO_MCP_SERVER_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* No public API yet. See the file comment above. */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* !_SYSREPO_MCP_SERVER_H */
