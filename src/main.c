/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <json-c/json.h>

#include "config.h"

/* =========================================================================
 * Compile-time defaults from config.in (Kconfig)
 * ========================================================================= */

/* Transport mode (UNIX_SOCKET or TCP) */
#ifndef UNIX_SOCKET
#  define UNIX_SOCKET_DEFAULT 0
#else  /* UNIX_SOCKET */
#  define UNIX_SOCKET_DEFAULT 1
#endif /* UNIX_SOCKET */

#ifndef TCP
#  define TCP_DEFAULT 0
#else  /* TCP */
#  define TCP_DEFAULT 1
#endif /* TCP */

/* Default values from Kconfig */
#define MCP_ENDPOINT_DEFAULT        "/mcp"
#define MCP_SESSION_TTL_DEFAULT     1800
#define MCP_MAX_SESSIONS_DEFAULT    64
#define FCGI_SOCK_PATH_DEFAULT      "/var/run/sysrepo-mcp.sock"
#define FCGI_HOST_DEFAULT           "127.0.0.1"
#define FCGI_PORT_DEFAULT           8080
#define ACL_ENABLED_DEFAULT         1
#define ACL_API_KEY_BEARER_DEFAULT  1
#define ACL_COOKIE_DEFAULT          0
#define ACL_COOKIE_NAME_DEFAULT     "mcp_session"
#define ACL_KEYS_FILE_DEFAULT       "/etc/sysrepo-mcp/keys"
#define ACL_NACM_USER_DEFAULT       "mcp"
#define ACL_NACM_USERS_DEFAULT      "mcp:operators"
#define ACL_DENY_UNKNOWN_DEFAULT    1
#define SR_USERNAME_DEFAULT         "mcp"
#define SR_TIMEOUT_DEFAULT          5000
#define LOG_SYSLOG_DEFAULT          1
#define LOG_LEVEL_DEFAULT           6
#define LOG_CONSOLE_DEFAULT         0
#define LOG_FILE_DEFAULT            "/var/log/sysrepo-mcp.log"
#define LOG_DAILY_ROTATE_DEFAULT    1

#ifdef CONFIG_SYSREPO_MCP_SERVER_SYSLOG
#include <syslog.h>
#endif /* CONFIG_SYSREPO_MCP_SERVER_SYSLOG */

#ifdef CONFIG_SYSREPO_MCP_SERVER_VERBOSE
#define sysrepo_mcp_server_debug(fmt, ...) \
        fprintf(stderr, "sysrepo-mcp: " fmt "\n", ##__VA_ARGS__)
#else  /* !CONFIG_SYSREPO_MCP_SERVER_VERBOSE */
#define sysrepo_mcp_server_debug(fmt, ...)
#endif /* defined(CONFIG_SYSREPO_MCP_SERVER_VERBOSE) */

/* =========================================================================
 * Configuration structure (from config.in defaults + sysrepo runtime)
 * ========================================================================= */

struct sysrepo_mcp_server_config {
        /* Server */
        char *endpoint;
        int   session_ttl;
        int   max_sessions;

        /* FastCGI transport */
#if UNIX_SOCKET_DEFAULT
        char *sock_path;
#else  /* TCP */
        char *host;
        int   port;
#endif /* UNIX_SOCKET */

        /* Access control */
#if ACL_ENABLED_DEFAULT
        int   acl_enabled;
        int   api_key_bearer;   /* 1 = bearer, 0 = cookie */
        char *cookie_name;
        char *keys_file;
        char *nacm_user;
        char *nacm_users;
        int   deny_unknown;
#endif /* ACL_ENABLED */

        /* Sysrepo library connection */
        char *sr_username;
        int   sr_timeout;

        /* Logging (elog defaults) */
#if LOG_SYSLOG_DEFAULT
        int   log_to_syslog;
#endif /* LOG_SYSLOG */
        int   log_level;
#if LOG_CONSOLE_DEFAULT
        int   log_to_console;
#endif /* LOG_CONSOLE */
        char *log_file;
#if LOG_DAILY_ROTATE_DEFAULT
        int   daily_rotate;
#endif /* LOG_DAILY_ROTATE */
};

/* =========================================================================
 * Initialize configuration with compile-time defaults
 * ========================================================================= */

static struct sysrepo_mcp_server_config
sysrepo_mcp_server_config_defaults(void)
{
        struct sysrepo_mcp_server_config config;

        memset(&config, 0, sizeof(config));

        /* Server */
        config.endpoint       = strdup(MCP_ENDPOINT_DEFAULT);
        config.session_ttl    = MCP_SESSION_TTL_DEFAULT;
        config.max_sessions   = MCP_MAX_SESSIONS_DEFAULT;

        /* FastCGI transport */
#if UNIX_SOCKET_DEFAULT
        config.sock_path = strdup(FCGI_SOCK_PATH_DEFAULT);
#else  /* TCP */
        config.host = strdup(FCGI_HOST_DEFAULT);
        config.port = FCGI_PORT_DEFAULT;
#endif /* UNIX_SOCKET */

        /* Access control */
#if ACL_ENABLED_DEFAULT
        config.acl_enabled      = 1;
        config.api_key_bearer   = ACL_API_KEY_BEARER_DEFAULT;
        config.cookie_name      = strdup(ACL_COOKIE_NAME_DEFAULT);
        config.keys_file        = strdup(ACL_KEYS_FILE_DEFAULT);
        config.nacm_user        = strdup(ACL_NACM_USER_DEFAULT);
        config.nacm_users       = strdup(ACL_NACM_USERS_DEFAULT);
        config.deny_unknown     = ACL_DENY_UNKNOWN_DEFAULT;
#endif /* ACL_ENABLED */

        /* Sysrepo library connection */
        config.sr_username     = strdup(SR_USERNAME_DEFAULT);
        config.sr_timeout      = SR_TIMEOUT_DEFAULT;

        /* Logging (elog) */
#if LOG_SYSLOG_DEFAULT
        config.log_to_syslog = 1;
#endif /* LOG_SYSLOG */
        config.log_level = LOG_LEVEL_DEFAULT;
#if LOG_CONSOLE_DEFAULT
        config.log_to_console = 0;
#endif /* LOG_CONSOLE */
        config.log_file = strdup(LOG_FILE_DEFAULT);
#if LOG_DAILY_ROTATE_DEFAULT
        config.daily_rotate = 1;
#endif /* LOG_DAILY_ROTATE */

        return config;
}

/* =========================================================================
 * Print configuration (debug / validation)
 * ========================================================================= */

static void
sysrepo_mcp_server_print_config(
        const struct sysrepo_mcp_server_config *config)
{
        printf("sysrepo-mcp configuration (compile-time defaults):\n");
        printf("  endpoint          = %s\n", config->endpoint);
        printf("  session_ttl       = %d\n", config->session_ttl);
        printf("  max_sessions      = %d\n", config->max_sessions);

#if UNIX_SOCKET_DEFAULT
        printf("  transport         = unix\n");
        printf("  sock_path         = %s\n", config->sock_path);
#else  /* TCP */
        printf("  transport         = tcp\n");
        printf("  host              = %s\n", config->host);
        printf("  port              = %d\n", config->port);
#endif /* UNIX_SOCKET */

#if ACL_ENABLED_DEFAULT
        printf("  acl_enabled       = %d\n", config->acl_enabled);
        printf("  api_key_bearer    = %d\n", config->api_key_bearer);
        printf("  cookie_name       = %s\n", config->cookie_name);
        printf("  keys_file         = %s\n", config->keys_file);
        printf("  nacm_user         = %s\n", config->nacm_user);
        printf("  nacm_users        = %s\n", config->nacm_users);
        printf("  deny_unknown      = %d\n", config->deny_unknown);
#endif /* ACL_ENABLED */

        printf("  sr_username       = %s\n", config->sr_username);
        printf("  sr_timeout        = %d\n", config->sr_timeout);
        printf("  log_level         = %d\n", config->log_level);
        printf("  log_file          = %s\n", config->log_file);
}

/* =========================================================================
 * Free configuration
 * ========================================================================= */

static void
sysrepo_mcp_server_config_free(
        struct sysrepo_mcp_server_config *config)
{
        free(config->endpoint);
#if UNIX_SOCKET_DEFAULT
        free(config->sock_path);
#else  /* TCP */
        free(config->host);
#endif /* UNIX_SOCKET */

#if ACL_ENABLED_DEFAULT
        free(config->cookie_name);
        free(config->keys_file);
        free(config->nacm_user);
        free(config->nacm_users);
#endif /* ACL_ENABLED */

        free(config->sr_username);
        free(config->log_file);
}

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
        struct sysrepo_mcp_server_config config;

        if (argc > 1 && strcmp(argv[1], "--help") == 0) {
                printf("sysrepo-mcp: sysrepo to MCP bridge\n");
                printf("Usage: sysrepo-mcp [options]\n");
                printf("  --help        Show this help\n");
                printf("  --version     Show version\n");
                return 0;
        }

        if (argc > 1 && strcmp(argv[1], "--version") == 0) {
                printf("sysrepo-mcp " PACKAGE_VERSION "\n");
                return 0;
        }

        /* Install signal handlers */
        if (signal(SIGINT, server_signal_handler) == SIG_ERR) {
                fprintf(stderr, "sysrepo-mcp: failed to install "
                               "SIGINT handler: %s\n", strerror(errno));
                return 1;
        }

        if (signal(SIGTERM, server_signal_handler) == SIG_ERR) {
                fprintf(stderr, "sysrepo-mcp: failed to install "
                               "SIGTERM handler: %s\n", strerror(errno));
                return 1;
        }

        /* Initialize configuration with compile-time defaults */
        config = sysrepo_mcp_server_config_defaults();

        /* Print configuration (for validation / debug) */
        sysrepo_mcp_server_debug("Configuration loaded:");
        sysrepo_mcp_server_print_config(&config);

        /* TODO: Initialize sysrepo connection */
        sysrepo_mcp_server_debug("Initializing sysrepo connection");

        /* TODO: Initialize MCP server (stream listener) */
        sysrepo_mcp_server_debug("Initializing MCP server");

        /* TODO: Register MCP tools */
        /* Available MCP tools (to be implemented):
         *
         * Configuration:
         *   sr_get_config  - Read config from YANG module (xpath, datastore, depth)
         *   sr_edit_config - Apply config changes to YANG module (config, target, xpath)
         *   sr_copy_config - Copy config between datastores (source, target, xpath)
         *
         * Operational:
         *   sr_get_operational  - Read operational data (xpath, datastore, depth)
         *
         * Subscriptions:
         *   sr_subscribe_oper_changes - Subscribe to operational changes (xpath, cb_type)
         *   sr_subscribe_notifs       - Subscribe to notifications (xpath, event_type)
         *
         * Module Management:
         *   sr_module_install    - Install YANG module (yang_file, features, imports)
         *   sr_module_uninstall  - Uninstall YANG module (module_name)
         *
         * RPC / Actions:
         *   sr_execute_rpc - Execute raw NETCONF RPC (rpc_name, input_params, xpath)
         *   sr_action      - Execute YANG action (module, action_name, input_params, xpath)
         *
         * System Status:
         *   get_status     - Server health (version, uptime, session_count, verbose)
         *
         * YANG Explorer:
         *   get_tree       - YANG schema tree (module, revision, path, with-comments)
         *   get_help       - Node documentation (xpath, module)
         */

        /* TODO: Run event loop */
        while (!server_stopping) {
                /* Event loop placeholder */
                sleep(1);
        }

        /* TODO: Cleanup */
        sysrepo_mcp_server_debug("Shutting down");

        /* Free configuration */
        sysrepo_mcp_server_config_free(&config);

        return 0;
}
