/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp-server.
 * Copyright (C) 2025 Grégor Boirie <gregor.boirie@free.fr>
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#ifdef CONFIG_SYSREPO_MCP_SERVER_SYSLOG
#include <syslog.h>
#endif /* CONFIG_SYSREPO_MCP_SERVER_SYSLOG */

#ifdef CONFIG_SYSREPO_MCP_SERVER_VERBOSE
#define sysrepo_mcp_server_debug(fmt, ...) \
        fprintf(stderr, "sysrepo-mcp-server: " fmt "\n", ##__VA_ARGS__)
#else  /* !CONFIG_SYSREPO_MCP_SERVER_VERBOSE */
#define sysrepo_mcp_server_debug(fmt, ...)
#endif /* defined(CONFIG_SYSREPO_MCP_SERVER_VERBOSE) */

static volatile sig_atomic_t server_stopping = 0;

static void
server_signal_handler(int signum)
{
        if (signum == SIGINT || signum == SIGTERM)
                server_stopping = 1;
}

int
main(int argc, char *argv[])
{
        int ret;

        if (argc > 1 && strcmp(argv[1], "--help") == 0) {
                printf("sysrepo-mcp-server: sysrepo to MCP bridge\n");
                printf("Usage: sysrepo-mcp-server [options]\n");
                printf("  --help        Show this help\n");
                printf("  --version     Show version\n");
                return 0;
        }

        if (argc > 1 && strcmp(argv[1], "--version") == 0) {
                printf("sysrepo-mcp-server " PACKAGE_VERSION "\n");
                return 0;
        }

        ret = signal(SIGINT, server_signal_handler);
        if (ret) {
                fprintf(stderr, "sysrepo-mcp-server: failed to install "
                               "SIGINT handler: %s\n", strerror(errno));
                return 1;
        }

        ret = signal(SIGTERM, server_signal_handler);
        if (ret) {
                fprintf(stderr, "sysrepo-mcp-server: failed to install "
                               "SIGTERM handler: %s\n", strerror(errno));
                return 1;
        }

        /* TODO: Initialize sysrepo connection */
        sysrepo_mcp_server_debug("Initializing sysrepo connection");

        /* TODO: Initialize MCP server */
        sysrepo_mcp_server_debug("Initializing MCP server");

        /* TODO: Run event loop */
        while (!server_stopping) {
                /* Event loop placeholder */
                sleep(1);
        }

        /* TODO: Cleanup */
        sysrepo_mcp_server_debug("Shutting down");

        return 0;
}
