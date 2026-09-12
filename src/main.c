/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp-server.
 * Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <libconfig.h>

#ifdef CONFIG_SYSREPO_MCP_SERVER_SYSLOG
#include <syslog.h>
#endif /* CONFIG_SYSREPO_MCP_SERVER_SYSLOG */

#ifdef CONFIG_SYSREPO_MCP_SERVER_VERBOSE
#define sysrepo_mcp_server_debug(fmt, ...) \
        fprintf(stderr, "sysrepo-mcp-server: " fmt "\n", ##__VA_ARGS__)
#else  /* !CONFIG_SYSREPO_MCP_SERVER_VERBOSE */
#define sysrepo_mcp_server_debug(fmt, ...)
#endif /* defined(CONFIG_SYSREPO_MCP_SERVER_VERBOSE) */

/* =========================================================================
 * Configuration structure (parsed from libconfig file)
 * ========================================================================= */

struct sysrepo_mcp_server_config {
        /* Server */
        char *host;
        int   port;
        char *transport;   /* "tcp" or "unix" */
        char *socket_path; /* Unix socket path (if transport == "unix") */

        /* Sysrepo connection */
        char *sr_socket_path;
        char *sr_username;
        char *sr_password;
        int   sr_connection_timeout;

        /* Logging */
        char *log_level;
        int   use_syslog;
        char *logfile;

        /* Agent */
        char *api_key;
        char *agent_name;
        char **allowed_modules;
        int   allowed_modules_count;
};

/* =========================================================================
 * Default configuration
 * ========================================================================= */

static struct sysrepo_mcp_server_config
sysrepo_mcp_server_config_defaults(void)
{
        struct sysrepo_mcp_server_config config;

        memset(&config, 0, sizeof(config));

        config.host      = strdup("0.0.0.0");
        config.port      = 8000;
        config.transport = strdup("tcp");
        config.socket_path = NULL;
        config.sr_socket_path = strdup("/var/run/sysrepo/sysrepod.sock");
        config.sr_username = strdup("sysrepo-mcp");
        config.sr_password = strdup("");
        config.sr_connection_timeout = 5000;
        config.log_level     = strdup("info");
        config.use_syslog    = 1;
        config.logfile       = strdup("/var/log/sysrepo-mcp-server.log");
        config.api_key       = strdup("");
        config.agent_name    = strdup("openhands-agent");
        config.allowed_modules = NULL;
        config.allowed_modules_count = 0;

        return config;
}

/* =========================================================================
 * Parse configuration file (libconfig)
 * ========================================================================= */

static int
sysrepo_mcp_server_parse_config(const char *config_path,
                                struct sysrepo_mcp_server_config *config)
{
        config_t cfg;
        config_value_t *value;
        int err = 0;

        config_init(&cfg);

        if (!config_read_file(&cfg, config_path)) {
                fprintf(stderr, "sysrepo-mcp-server: config error: %s "
                               "[%d]: %s\n",
                        config_error_file(&cfg),
                        config_error_line(&cfg),
                        config_error_text(&cfg));
                err = 1;
                goto out;
        }

        /* Server section */
        {
                const char *str;

                if (config_lookup_string(&cfg, "server.host", &str) == CONFIG_TRUE) {
                        free(config->host);
                        config->host = strdup(str);
                }
                if (config_lookup_int(&cfg, "server.port", &config->port)
                    != CONFIG_TRUE) {
                        /* use default */
                }
                if (config_lookup_string(&cfg, "server.transport", &str)
                    == CONFIG_TRUE) {
                        free(config->transport);
                        config->transport = strdup(str);

                        /* If unix transport, read socket path */
                        if (strcmp(config->transport, "unix") == 0) {
                                if (config_lookup_string(&cfg,
                                             "server.socket_path", &str)
                                    == CONFIG_TRUE) {
                                        free(config->socket_path);
                                        config->socket_path = strdup(str);
                                }
                        }
                }
        }

        /* Sysrepo section */
        {
                const char *str;

                if (config_lookup_string(&cfg, "sysrepo.socket_path", &str)
                    == CONFIG_TRUE) {
                        free(config->sr_socket_path);
                        config->sr_socket_path = strdup(str);
                }
                if (config_lookup_string(&cfg, "sysrepo.username", &str)
                    == CONFIG_TRUE) {
                        free(config->sr_username);
                        config->sr_username = strdup(str);
                }
                if (config_lookup_string(&cfg, "sysrepo.password", &str)
                    == CONFIG_TRUE) {
                        free(config->sr_password);
                        config->sr_password = strdup(str);
                }
                if (config_lookup_int(&cfg, "sysrepo.connection_timeout",
                    &config->sr_connection_timeout) != CONFIG_TRUE) {
                        /* use default */
                }
        }

        /* Logging section */
        {
                const char *str;

                if (config_lookup_string(&cfg, "logging.level", &str)
                    == CONFIG_TRUE) {
                        free(config->log_level);
                        config->log_level = strdup(str);
                }
                if (config_lookup_bool(&cfg, "logging.use_syslog",
                    &config->use_syslog) != CONFIG_TRUE) {
                        /* use default */
                }
                if (config_lookup_string(&cfg, "logging.logfile", &str)
                    == CONFIG_TRUE) {
                        free(config->logfile);
                        config->logfile = strdup(str);
                }
        }

        /* Agent section */
        {
                const char *str;
                config_list_t *list;
                int count, i;

                if (config_lookup_string(&cfg, "agent.api_key", &str)
                    == CONFIG_TRUE) {
                        free(config->api_key);
                        config->api_key = strdup(str);
                }
                if (config_lookup_string(&cfg, "agent.name", &str)
                    == CONFIG_TRUE) {
                        free(config->agent_name);
                        config->agent_name = strdup(str);
                }

                /* Parse allowed_modules list */
                if (config_lookup_list(&cfg, "agent.allowed_modules", &list)
                    == CONFIG_TRUE) {
                        count = config_list_size(list);
                        if (config->allowed_modules) {
                                for (i = 0; i < config->allowed_modules_count; i++)
                                        free(config->allowed_modules[i]);
                                free(config->allowed_modules);
                        }
                        config->allowed_modules_count = count;
                        if (count > 0) {
                                config->allowed_modules =
                                        calloc(count, sizeof(char *));
                                for (i = 0; i < count; i++) {
                                        value = config_list_get_elem(list, i);
                                        if (value && value->type == CONFIG_TYPE_STRING) {
                                                config->allowed_modules[i] =
                                                        strdup(value->data.string);
                                        }
                                }
                        }
                }
        }

out:
        config_destroy(&cfg);
        return err;
}

/* =========================================================================
 * Print configuration (debug / validation)
 * ========================================================================= */

static void
sysrepo_mcp_server_print_config(
        const struct sysrepo_mcp_server_config *config)
{
        int i;

        printf("sysrepo-mcp-server configuration:\n");
        printf("  server.host       = %s\n", config->host);
        printf("  server.port       = %d\n", config->port);
        printf("  server.transport  = %s\n", config->transport);
        if (config->socket_path)
                printf("  server.socket_path = %s\n", config->socket_path);
        printf("  sysrepo.socket_path  = %s\n", config->sr_socket_path);
        printf("  sysrepo.username   = %s\n", config->sr_username);
        printf("  sysrepo.timeout    = %d\n", config->sr_connection_timeout);
        printf("  logging.level      = %s\n", config->log_level);
        printf("  logging.use_syslog = %s\n", config->use_syslog ? "yes" : "no");
        printf("  logging.logfile    = %s\n", config->logfile);
        printf("  agent.api_key      = %s\n", config->api_key);
        printf("  agent.name         = %s\n", config->agent_name);
        printf("  agent.allowed_modules:");
        for (i = 0; i < config->allowed_modules_count; i++) {
                printf(" %s", config->allowed_modules[i]);
        }
        printf("\n");
}

/* =========================================================================
 * Free configuration
 * ========================================================================= */

static void
sysrepo_mcp_server_config_free(
        struct sysrepo_mcp_server_config *config)
{
        int i;

        free(config->host);
        free(config->transport);
        free(config->socket_path);
        free(config->sr_socket_path);
        free(config->sr_username);
        free(config->sr_password);
        free(config->log_level);
        free(config->logfile);
        free(config->api_key);
        free(config->agent_name);
        if (config->allowed_modules) {
                for (i = 0; i < config->allowed_modules_count; i++)
                        free(config->allowed_modules[i]);
                free(config->allowed_modules);
        }
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
        int ret;
        const char *config_path = NULL;
        struct sysrepo_mcp_server_config config;

        if (argc > 1 && strcmp(argv[1], "--help") == 0) {
                printf("sysrepo-mcp-server: sysrepo to MCP bridge\n");
                printf("Usage: sysrepo-mcp-server [options]\n");
                printf("  --help        Show this help\n");
                printf("  --version     Show version\n");
                printf("  --config PATH Use this config file (libconfig format)\n");
                return 0;
        }

        if (argc > 1 && strcmp(argv[1], "--version") == 0) {
                printf("sysrepo-mcp-server " PACKAGE_VERSION "\n");
                return 0;
        }

        /* Parse config file path from arguments */
        {
                int i;
                for (i = 1; i < argc; i++) {
                        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
                                config_path = argv[i + 1];
                                i++;
                        }
                }
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

        /* Initialize default configuration */
        config = sysrepo_mcp_server_config_defaults();

        /* Parse configuration file (if provided) */
        if (config_path) {
                sysrepo_mcp_server_debug("Loading config file: %s", config_path);
                ret = sysrepo_mcp_server_parse_config(config_path, &config);
                if (ret) {
                        fprintf(stderr, "sysrepo-mcp-server: failed to parse "
                                       "configuration file '%s'\n", config_path);
                        sysrepo_mcp_server_config_free(&config);
                        return 1;
                }
        } else {
                sysrepo_mcp_server_debug("No config file provided, using defaults");
        }

        /* Print configuration (for validation / debug) */
        sysrepo_mcp_server_debug("Configuration loaded:");
        sysrepo_mcp_server_print_config(&config);

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

        /* Free configuration */
        sysrepo_mcp_server_config_free(&config);

        return 0;
}
