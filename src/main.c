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

/* FastCGI - C only headers */
#include <fcgi_stdio.h>

/* JSON-C */
#include <json-c/json.h>

/******************************************************************************
 * Server version
 ******************************************************************************/
#define SERVER_VERSION "0.1.0"

/******************************************************************************
 * MCP Tool: get_status
 * Returns server status information
 ******************************************************************************/
static struct json_object *
handle_get_status(const struct json_object *params)
{
    (void)params; /* Unused parameter */
    struct json_object *result = json_object_new_object();
    
    json_object_object_add(result, "version", 
        json_object_new_string(SERVER_VERSION));
    json_object_object_add(result, "uptime_seconds", 
        json_object_new_int(0));
    json_object_object_add(result, "active_sessions", 
        json_object_new_int(0));
    json_object_object_add(result, "max_sessions", 
        json_object_new_int(64));
    
    return result;
}

/******************************************************************************
 * MCP Tool: sr_get_config
 * Read configuration from a YANG module
 ******************************************************************************/
static struct json_object *
handle_sr_get_config(const struct json_object *params)
{
    const char *xpath = NULL;
    const char *datastore = "running";
    const char *depth = "deep";
    
    /* Extract parameters */
    struct json_object *xpath_obj;
    if (json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        xpath = json_object_get_string(xpath_obj);
    }
    
    struct json_object *datastore_obj;
    if (json_object_object_get_ex(params, "datastore", &datastore_obj)) {
        datastore = json_object_get_string(datastore_obj);
    }
    
    struct json_object *depth_obj;
    if (json_object_object_get_ex(params, "depth", &depth_obj)) {
        depth = json_object_get_string(depth_obj);
    }
    
    if (!xpath) {
        return NULL; /* Error: xpath is required */
    }
    
    /* TODO: Implement actual sysrepo sr_get_config call */
    /* For now, return empty data as skeleton */
    struct json_object *result = json_object_new_object();
    struct json_object *data = json_object_new_object();
    
    json_object_object_add(result, "data", data);
    json_object_object_add(result, "module", 
        json_object_new_string("sysrepo-mcp"));
    json_object_object_add(result, "path", 
        json_object_new_string(xpath));
    
    return result;
}

/******************************************************************************
 * MCP Tool: sr_edit_config
 * Apply configuration changes
 ******************************************************************************/
static struct json_object *
handle_sr_edit_config(const struct json_object *params)
{
    const char *xpath = NULL;
    const char *target = NULL;
    struct json_object *config_obj = NULL;
    
    /* Extract required parameters */
    struct json_object *xpath_obj;
    if (!json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        return NULL; /* Error: xpath is required */
    }
    xpath = json_object_get_string(xpath_obj);
    
    struct json_object *target_obj;
    if (json_object_object_get_ex(params, "target", &target_obj)) {
        target = json_object_get_string(target_obj);
    }
    
    if (!json_object_object_get_ex(params, "config", &config_obj)) {
        return NULL; /* Error: config is required */
    }
    
    if (!xpath || !target) {
        return NULL; /* Error: missing required parameters */
    }
    
    /* TODO: Implement actual sysrepo sr_edit_config call */
    struct json_object *result = json_object_new_object();
    json_object_object_add(result, "ok", json_object_new_boolean(1));
    
    return result;
}

/******************************************************************************
 * MCP Tool: sr_get_operational
 * Read operational state data
 ******************************************************************************/
static struct json_object *
handle_sr_get_operational(const struct json_object *params)
{
    const char *xpath = NULL;
    
    /* Extract parameters */
    struct json_object *xpath_obj;
    if (json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        xpath = json_object_get_string(xpath_obj);
    }
    
    if (!xpath) {
        return NULL; /* Error: xpath is required */
    }
    
    /* TODO: Implement actual sysrepo sr_get_operational call */
    struct json_object *result = json_object_new_object();
    struct json_object *data = json_object_new_object();
    
    json_object_object_add(result, "data", data);
    
    return result;
}

/******************************************************************************
 * MCP Tool: sr_execute_rpc
 * Execute a raw NETCONF RPC operation
 ******************************************************************************/
static struct json_object *
handle_sr_execute_rpc(const struct json_object *params)
{
    const char *xpath = NULL;
    
    /* Extract required parameters */
    struct json_object *xpath_obj;
    if (!json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        return NULL; /* Error: xpath is required */
    }
    xpath = json_object_get_string(xpath_obj);
    
    if (!xpath) {
        return NULL; /* Error: xpath is required */
    }
    
    /* TODO: Implement actual sysrepo RPC execution */
    /* xpath format: /module:rpc-name */
    struct json_object *result = json_object_new_object();
    struct json_object *output = json_object_new_object();
    
    json_object_object_add(result, "output", output);
    
    return result;
}

/******************************************************************************
 * MCP Tool: sr_action
 * Execute a YANG action
 ******************************************************************************/
static struct json_object *
handle_sr_action(const struct json_object *params)
{
    const char *xpath = NULL;
    
    /* Extract required parameters */
    struct json_object *xpath_obj;
    if (!json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        return NULL; /* Error: xpath is required */
    }
    xpath = json_object_get_string(xpath_obj);
    
    if (!xpath) {
        return NULL; /* Error: xpath is required */
    }
    
    /* TODO: Implement actual YANG action execution */
    /* xpath format: /module:action-name */
    struct json_object *result = json_object_new_object();
    struct json_object *output = json_object_new_object();
    
    json_object_object_add(result, "output", output);
    
    return result;
}

/******************************************************************************
 * MCP Tool: get_tree
 * Get YANG schema tree
 ******************************************************************************/
static struct json_object *
handle_get_tree(const struct json_object *params)
{
    const char *module = NULL;
    const char *xpath = "/";
    struct json_object *revision_obj = NULL;
    struct json_object *with_comments_obj = NULL;
    
    /* Extract parameters */
    struct json_object *module_obj;
    if (!json_object_object_get_ex(params, "module", &module_obj)) {
        return NULL; /* Error: module is required */
    }
    module = json_object_get_string(module_obj);
    
    struct json_object *xpath_obj;
    if (json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        xpath = json_object_get_string(xpath_obj);
    }
    
    (void)json_object_object_get_ex(params, "revision", &revision_obj);
    (void)json_object_object_get_ex(params, "with-comments", &with_comments_obj);
    
    if (!module || !xpath) {
        return NULL; /* Error: missing required parameters */
    }
    
    /* TODO: Implement actual YANG tree retrieval via libyang */
    struct json_object *result = json_object_new_object();
    struct json_object *tree = json_object_new_object();
    struct json_object *nodes = json_object_new_array();
    struct json_object *references = json_object_new_array();
    
    json_object_object_add(result, "tree", tree);
    json_object_object_add(result, "nodes", nodes);
    json_object_object_add(result, "references", references);
    
    return result;
}

/******************************************************************************
 * MCP Tool: get_help
 * Get documentation for a YANG node
 ******************************************************************************/
static struct json_object *
handle_get_help(const struct json_object *params)
{
    const char *xpath = NULL;
    
    /* Extract required parameters */
    struct json_object *xpath_obj;
    if (!json_object_object_get_ex(params, "xpath", &xpath_obj)) {
        return NULL; /* Error: xpath is required */
    }
    xpath = json_object_get_string(xpath_obj);
    
    if (!xpath) {
        return NULL; /* Error: xpath is required */
    }
    
    /* TODO: Implement actual YANG help retrieval via libyang */
    struct json_object *result = json_object_new_object();
    
    json_object_object_add(result, "path", json_object_new_string(xpath));
    json_object_object_add(result, "node_type", 
        json_object_new_string("container"));
    json_object_object_add(result, "description", 
        json_object_new_string("YANG node description"));
    json_object_object_add(result, "mandatory", 
        json_object_new_boolean(0));
    
    return result;
}

/******************************************************************************
 * Tool dispatch table
 ******************************************************************************/
struct tool_handler {
    const char *name;
    struct json_object *(*handler)(const struct json_object *params);
};

static const struct tool_handler tool_handlers[] = {
    { "get_status",      handle_get_status      },
    { "sr_get_config",   handle_sr_get_config   },
    { "sr_edit_config",  handle_sr_edit_config  },
    { "sr_get_operational", handle_sr_get_operational },
    { "sr_execute_rpc",  handle_sr_execute_rpc  },
    { "sr_action",       handle_sr_action       },
    { "get_tree",        handle_get_tree        },
    { "get_help",        handle_get_help        },
    { NULL, NULL }
};

/******************************************************************************
 * Find tool handler by name
 ******************************************************************************/
static const struct tool_handler *
find_tool_handler(const char *name)
{
    for (int i = 0; tool_handlers[i].name != NULL; i++) {
        if (strcmp(tool_handlers[i].name, name) == 0) {
            return &tool_handlers[i];
        }
    }
    return NULL;
}

/******************************************************************************
 * Build MCP JSON-RPC response
 ******************************************************************************/
static void
send_mcp_response(int id, struct json_object *result, 
                  int error_code, const char *error_msg)
{
    struct json_object *response = json_object_new_object();
    
    json_object_object_add(response, "jsonrpc", 
        json_object_new_string("2.0"));
    json_object_object_add(response, "id", 
        json_object_new_int(id));
    
    if (error_code == 0) {
        json_object_object_add(response, "result", result);
    } else {
        struct json_object *error = json_object_new_object();
        json_object_object_add(error, "code", 
            json_object_new_int(error_code));
        json_object_object_add(error, "message", 
            json_object_new_string(error_msg ? error_msg : "Unknown error"));
        json_object_object_add(response, "error", error);
    }
    
    const char *response_str = json_object_to_json_string(response);
    
    /* Send HTTP response */
    printf("Content-Type: application/json\r\n\r\n");
    printf("%s", response_str);
    
    json_object_put(response);
    free((void *)response_str);
}

/******************************************************************************
 * Process MCP JSON-RPC request
 ******************************************************************************/
static void
process_mcp_request(const char *request_body)
{
    struct json_object *request_json = json_tokener_parse(request_body);
    
    if (!request_json) {
        send_mcp_response(0, NULL, -32700, "Parse error");
        return;
    }
    
    /* Extract method */
    struct json_object *method_obj;
    if (!json_object_object_get_ex(request_json, "method", &method_obj)) {
        send_mcp_response(0, NULL, -32600, "Invalid request");
        json_object_put(request_json);
        return;
    }
    
    const char *method = json_object_get_string(method_obj);
    
    /* Extract id */
    struct json_object *id_obj;
    int id = 0;
    if (json_object_object_get_ex(request_json, "id", &id_obj)) {
        id = json_object_get_int(id_obj);
    }
    
    /* Extract params */
    struct json_object *params = NULL;
    json_object_object_get_ex(request_json, "params", &params);
    
    /* Handle tools/call method */
    if (strcmp(method, "tools/call") == 0 && params) {
        struct json_object *name_obj;
        struct json_object *arguments;
        
        if (json_object_object_get_ex(params, "name", &name_obj) &&
            json_object_object_get_ex(params, "arguments", &arguments)) {
            
            const char *tool_name = json_object_get_string(name_obj);
            const struct tool_handler *handler = find_tool_handler(tool_name);
            
            if (handler) {
                struct json_object *result = handler->handler(arguments);
                if (result) {
                    send_mcp_response(id, result, 0, NULL);
                    json_object_put(result);
                } else {
                    send_mcp_response(id, NULL, -32602, 
                                     "Invalid parameters");
                }
            } else {
                send_mcp_response(id, NULL, -32601, 
                                 "Tool not found");
            }
        } else {
            send_mcp_response(id, NULL, -32602, 
                             "Invalid parameters");
        }
    } else {
        send_mcp_response(id, NULL, -32601, "Method not found");
    }
    
    json_object_put(request_json);
}

/******************************************************************************
 * Show help
 ******************************************************************************/
static void
show_help(void)
{
    printf("sysrepo-mcp: sysrepo to MCP bridge\n");
    printf("Usage: sysrepo-mcp [options]\n");
    printf("  --help        Show this help\n");
    printf("  --version     Show version\n");
}

/******************************************************************************
 * Signal handler
 ******************************************************************************/
static volatile sig_atomic_t server_stopping = 0;

static void
signal_handler(int signum)
{
    (void)signum; /* Unused parameter */
    server_stopping = 1;
}

/******************************************************************************
 * Check if running as FastCGI
 * Returns 1 if FCGX_IsCGI() would succeed, 0 otherwise
 ******************************************************************************/
static int
is_fastcgi_environment(void)
{
    /* Check if we're running under FastCGI by looking at environment */
    const char *fcgi_env = getenv("FCGI_ROLE");
    if (fcgi_env != NULL) {
        return 1;
    }
    
    /* Also check for common FastCGI environment variables */
    if (getenv("GATEWAY_INTERFACE") != NULL ||
        getenv("REQUEST_METHOD") != NULL ||
        getenv("QUERY_STRING") != NULL) {
        return 1;
    }
    
    return 0;
}

/******************************************************************************
 * Main entry point
 ******************************************************************************/
int
main(int argc, char *argv[])
{
    /* Handle command-line options (CLI mode) */
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0) {
            show_help();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("sysrepo-mcp " SERVER_VERSION "\n");
            return 0;
        }
    }
    
    /* Check if we're running as FastCGI */
    if (!is_fastcgi_environment()) {
        /* Running as standalone CLI - show help and exit */
        if (argc == 1) {
            fprintf(stderr, "sysrepo-mcp: This is a FastCGI application.\n");
            fprintf(stderr, "Run it behind a reverse proxy (lighttpd, nginx) or\n");
            fprintf(stderr, "use --help or --version for command-line options.\n");
            return 1;
        }
        return 0;
    }
    
    /* Install signal handlers */
    if (signal(SIGINT, signal_handler) == SIG_ERR) {
        fprintf(stderr, "Failed to install SIGINT handler: %s\n", strerror(errno));
        return 1;
    }
    if (signal(SIGTERM, signal_handler) == SIG_ERR) {
        fprintf(stderr, "Failed to install SIGTERM handler: %s\n", strerror(errno));
        return 1;
    }
    
    /* Initialize FastCGI */
    FCGX_Request request;
    FCGX_Init();
    FCGX_InitRequest(&request, 0, 0);
    
    /* Log startup */
    fprintf(stderr, "sysrepo-mcp: starting FastCGI server\n");
    
    /* Main FastCGI request loop */
    while (!server_stopping) {
        int rc = FCGX_Accept_r(&request);
        
        if (rc < 0) {
            break; /* Error or end of connection */
        }
        
        /* Read request body from stdin */
        char *content_length_str = FCGX_GetParam("CONTENT_LENGTH", request.envp);
        int content_length = content_length_str ? atoi(content_length_str) : 0;
        
        char *request_body = NULL;
        if (content_length > 0 && content_length < 1024 * 1024) {
            request_body = malloc(content_length + 1);
            if (request_body) {
                int nread = FCGX_GetStr(request_body, content_length, request.in);
                request_body[nread] = '\0';
            }
        }
        
        /* Process MCP request */
        if (request_body) {
            process_mcp_request(request_body);
            free(request_body);
        }
        
        /* Finish request */
        FCGX_Finish_r(&request);
    }
    
    /* Cleanup FastCGI */
    FCGX_Finish();
    
    fprintf(stderr, "sysrepo-mcp: shutdown complete\n");
    
    return 0;
}
