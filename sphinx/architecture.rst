.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Architecture
============

This chapter describes the architecture of sysrepo-mcp, including its
placement in the system, communication protocols, and configuration models.

> **Note**: This project is a integration skeleton. No functionality is
> implemented at this stage. This documentation describes the target
> architecture and technical choices.

Overview
--------

sysrepo-mcp is a bridge between the `Model Context Protocol
<https://modelcontextprotocol.io>`_ (MCP) and `sysrepo
<https://github.com/sysrepo/sysrepo>`_, a NETCONF configuration library. It
exposes sysrepo operations as MCP tools that an AI agent can invoke.

The server **uses FastCGI for transport** (via the fcgi2 library). This is a
mandatory design choice. The server communicates with a reverse proxy
(lighttpd, nginx) that handles HTTP/HTTPS connections and forwards MCP
requests to sysrepo-mcp via FastCGI.

The server links the sysrepo library at build time and accesses the YANG
datastore files directly through the sysrepo C API (``sr_conn_open_session()``,
``sr_get_items()``, etc.). **No separate sysrepo daemon (sysrepod) is used**.

Architecture Diagram
--------------------

::

   +-------------+      +---------------+      +-------------+
   |   AI Agent  |<--->| Reverse Proxy |<--->| sysrepo-mcp |
   | (OpenHands  |      | (lighttpd)    |      | (FastCGI)   |
   |  SDK, etc.) |      |               |      +-------------+
   +-------------+      +---------------+            |
                                                  v
                                                 +-------------+
                                                 |  Sysrepo    |
                                                 |  Library    |
                                                 | (linked)    |
                                                 +------+------+
                                                        |
                                                        v
                                                 +-------------+
                                                 | YANG Models |
                                                 | (Datastore) |
                                                 +-------------+

Components
----------

1. **AI Agent**: The client that communicates with sysrepo-mcp via
   MCP. This could be OpenHands, a custom agent, or any MCP-compatible client.

2. **Reverse Proxy (lighttpd/nginx)**: Handles HTTP/HTTPS connections, TLS
   termination, and forwards MCP requests to sysrepo-mcp via **FastCGI**.

3. **sysrepo-mcp**: The core application that:

   - Accepts FastCGI connections from the reverse proxy
   - Parses MCP JSON-RPC messages
   - Calls the sysrepo library directly (``sr_conn_open_session()``,
     ``sr_get_items()``, ``sr_edit_item()``, etc.)
   - Returns MCP responses to the agent via FastCGI

4. **Sysrepo Library**: A C library linked into sysrepo-mcp that provides
   NETCONF datastore operations. The library accesses YANG model data
   directly from the filesystem (``/etc/sysrepo/data/``) without a
   separate daemon process.

5. **YANG Models**: Schema definitions that define the configuration and
   operational state data accessible through sysrepo.

Communication Protocol
----------------------

The server implements **MCP over FastCGI** transport. FastCGI is a binary
protocol that allows the reverse proxy to forward HTTP requests to the
sysrepo-mcp process.

**Important Limitation**: Server-Sent Events (SSE) **will NOT be implemented**
because it is incompatible with the FastCGI protocol. All communication occurs
through standard HTTP GET and POST requests forwarded via FastCGI.

Request Flow
~~~~~~~~~~~~

1. The AI agent sends an HTTP POST to the MCP endpoint (e.g. ``/mcp``).

2. The request includes:

   - ``Content-Type: application/json``
   - JSON-RPC payload with method and parameters

3. The reverse proxy (lighttpd) forwards the request to sysrepo-mcp via FastCGI.

4. sysrepo-mcp processes the request and returns an HTTP response
   with the MCP result via FastCGI.

5. The reverse proxy forwards the response back to the agent.

.. code-block:: http

   POST /mcp HTTP/1.1
   Host: example.com
   Content-Type: application/json
   Authorization: Bearer <api-key>

   {
       "jsonrpc": "2.0",
       "id": 1,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "path": "/ietf-interfaces:interfaces"
           }
       }
   }

Session Management
~~~~~~~~~~~~~~~~~~

Sessions are managed through HTTP headers and FastCGI environment variables.
The server maintains session state and associates requests with their
respective sessions.

- **Session Creation**: First request creates a new session. The server
  responds with a session identifier.

- **Session Validation**: Subsequent requests must include the session identifier.
  Invalid or expired session IDs are rejected with HTTP 401.

- **Session TTL**: Sessions idle for longer than a configured timeout
  are automatically destroyed.

- **Max Sessions**: The server enforces a maximum number of concurrent
  sessions. New sessions are rejected with HTTP 503 when this limit is reached.

Configuration Models
--------------------

sysrepo-mcp uses a **split configuration model**:

- **Runtime Configuration**: Defined in ``yang/sysrepo-mcp.yang`` (API keys list and server operational state)
- **Build-time Configuration**: Defined in ``config.in`` via Kconfig (transport, logging, sysrepo paths, access control)

Build-time Configuration
~~~~~~~~~~~~~~~~~~~~~~~~

Build-time configuration is defined in ``config.in`` (Kconfig format) and
determines the server's capabilities at compile time.


Sysrepo Connection
""""""""""""""""""""""

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_SYSREPO_DATSTORE_DIR``
     - ``"/etc/sysrepo/data"``
     - Path to sysrepo datastore directory

.. note::

   Since sysrepo is a library (not a daemon), there is no socket path to
   configure. The library accesses datastore files directly from the
   filesystem. The datastore directory path is configured at build time in ``config.in``.

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``SYSREPO_MCP_SERVER_SYSLOG_ENABLED``
     - ``y``
     - Enable syslog logging via elog library
   * - ``SYSREPO_MCP_SERVER_LOG_LEVEL``
     - ``6``
     - Log level (0-7, where 6=info)
   * - ``SYSREPO_MCP_SERVER_LOG_VERBOSE``
     - ``n``
     - Enable verbose debugging output
   * - ``SYSREPO_MCP_SERVER_LOG_CONSOLE``
     - ``y``
     - Enable console logging (stderr)

Authentication Models
---------------------

sysrepo-mcp will support authentication via API keys (to be implemented).

Bearer Token Authentication
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The recommended mode for agent-to-server communication. Clients send an HTTP
``Authorization: Bearer <key>`` header with each request.

**Planned Workflow (not yet implemented):**

1. Agent includes ``Authorization: Bearer <api-key>`` header
2. Server validates key against the API key list in the YANG datastore (from the YANG configuration (yang/sysrepo-mcp.yang))
3. If valid, server creates sysrepo session
4. Request is processed with appropriate permissions

Cookie-Based Authentication
~~~~~~~~~~~~~~~~~~~~~~~~~~~

An alternative mode for browser-based scenarios. Clients send a cookie with
the API key.

**Planned Workflow (not yet implemented):**

1. Client sends HTTP request with cookie ``mcp_session=<api-key>``
2. Server validates cookie name against configuration
3. Server validates key against the API key list in the YANG datastore
4. If valid, server processes request

.. note::

   Bearer token authentication is recommended for production deployments.
   Cookie-based authentication may be implemented for specific use cases.

Sysrepo NACM Integration
-------------------------

When access control is enabled, sysrepo-mcp will integrate with sysrepo's
`Native Access Control Module (NACM) <https://www.rfc-editor.org/rfc/rfc6536>`_
to enforce fine-grained access control.

**Note**: NACM integration is planned but not yet implemented in this skeleton.

Access Control Flow
~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   Client Request
       
   API Key Validation (sysrepo-mcp)
       
   Sysrepo Session Creation
       
   Request Processing (enforcing NACM rules)
       
   Response to Client

FastCGI Transport
----------------

sysrepo-mcp **requires FastCGI** for transport. The server runs as a FastCGI
application behind a reverse proxy (lighttpd, nginx). This design choice
simplifies deployment and leverages existing HTTP infrastructure.

**Production Configuration:**

- lighttpd or nginx accepts HTTP/HTTPS requests
- Requests are forwarded to sysrepo-mcp via FastCGI
- TLS is terminated at the reverse proxy
- No direct TCP or Unix socket support

.. note::

   **SSE Support**: Server-Sent Events **will NOT be implemented** because
   the FastCGI protocol does not support bidirectional streaming connections.
   All MCP communication uses standard HTTP request/response over FastCGI.

Sysrepo Library Connection
--------------------------

sysrepo-mcp links the sysrepo library at build time and uses the sysrepo
C API directly to perform NETCONF operations.

Connection Management
~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Session Management**: Each MCP session creates a sysrepo session via
  ``sr_session_create()``. The session uses the username configured at build time.
- **Datastore Access**: The library reads/writes YANG data directly from
  the filesystem (``/etc/sysrepo/data/``).
- **Error Handling**: Connection failures are handled gracefully with
  retries and appropriate error messages.

Sysrepo Operations
~~~~~~~~~~~~~~~~~~~~~~

The server **will expose** the following sysrepo library operations as MCP tools.
Each tool requires specific parameters from the client.

**Note**: These operations are planned but not yet implemented in this skeleton.

Configuration Operations:

``sr_get_config``
   Read configuration from a YANG module.

   Parameters:

   - ``path`` (string, required): XPath expression to select data nodes (must include module namespace prefix, e.g., ``"/oven:oven"`` for the oven module from ``extern/sysrepo/examples/plugin/oven.yang``)
   - ``datastore`` (string, optional): Which datastore - ``"running"``,
     ``"startup"``, or ``"candidate"`` (default: ``"running"``)
   - ``depth`` (string, optional): How deep to traverse (``"shallow"``,
     ``"deep"``, ``"children"``)

   Returns data in libyang LYD_JSON format (compatible with ``lyd_print_fd`` with ``LYD_JSON``).

``sr_edit_config``
   Apply configuration changes to a YANG module.

   Parameters:

   - ``config`` (object, required): Configuration data in JSON format (compatible with libyang/sysrepo). Must match the YANG schema structure with module namespace prefixes as JSON keys (e.g., ``{"oven:oven": {"temperature": 180, "turned-on": true}}`` for the oven module from ``extern/sysrepo/examples/plugin/oven.yang``)
   - ``target`` (string, required): Target datastore (``"running"``,
     ``"startup"``, ``"candidate"``)
   - ``xpath`` (string, required): XPath expression to target specific nodes (must include module namespace prefix, e.g., ``"/oven:oven"``)

**Operational Data:**

``sr_get_operational``
   Read operational state data from the datastore (e.g., oven-state from extern/sysrepo/examples/plugin/oven.yang).

   Parameters:

   - ``xpath`` (string, required): XPath expression selecting the data to
     retrieve (e.g., ``"/oven:oven-state"``)
   - ``datastore`` (string, optional): Which datastore (default:
     ``"operational"``)
   - ``depth`` (string, optional): Traversal depth

   Returns data in libyang LYD_JSON format (compatible with ``lyd_print_fd`` with ``LYD_JSON``).

**Subscriptions:**

.. note::

   Subscription features (``sr_subscribe_oper_changes``, ``sr_subscribe_notifs``)
   may have limited functionality due to FastCGI's request/response model.
   Real-time notifications would require a different transport (not planned).

**Module Management:**

``sr_module_install``
   Install a YANG module into the datastore.

   Parameters:

   - ``yang_file`` (string, required): Path to the YANG schema file
   - ``features`` (string, optional): Comma-separated features to enable
   - ``imports`` (string, optional): Comma-separated import paths

``sr_module_uninstall``
   Uninstall a YANG module from the datastore.

   Parameters:

   - ``module_name`` (string, required): Name of the YANG module to remove

**RPC / Actions:**

``sr_execute_rpc``
   Execute a raw NETCONF RPC operation.

   Parameters:

   - ``rpc_name`` (string, required): Name of the RPC operation
   - ``input_params`` (object, optional): RPC input parameters as key-value
     pairs
   - ``xpath`` (string, optional): XPath for targeted operations

``sr_action``
   Execute a YANG action (RPC-style tool defined in a YANG model).

   Parameters:

   - ``module`` (string, required): YANG module containing the action
   - ``action_name`` (string, required): Action name (e.g. ``"reset-interface"``)
   - ``input_params`` (object, optional): Action input parameters
   - ``xpath`` (string, optional): XPath to target specific data nodes

System Status
-------------

The server **will expose** a ``get_status`` tool that returns server health
information. For operational state data, use ``sr_get_operational`` with the
``sysrepo-mcp`` YANG module.

**Note**: This feature is planned but not yet implemented.

**get_status**
   Returns server status information only (no YANG data).

   Parameters:

   - ``verbose`` (boolean, optional): Include detailed session information
     (default: ``false``).

   Returns:

   - ``version`` (string): Server version
   - ``uptime_seconds`` (integer): Server uptime in seconds
   - ``active_sessions`` (integer): Number of active sessions
   - ``max_sessions`` (integer): Maximum concurrent sessions configured
   - ``session_ids`` (array of strings, optional): List of active session IDs
     (only present when ``verbose`` is ``true``)

YANG Tree Explorer
------------------

The server **will provide** a ``get_tree`` tool to discover the YANG schema
structure (e.g., from ``yang/sysrepo-mcp.yang`` or ``extern/sysrepo/examples/plugin/oven.yang``), which helps agents construct valid XPath expressions.

**Note**: This feature is planned but not yet implemented.

**get_tree**
   Returns the YANG schema tree for a module, showing all nodes, lists,
   keys, and leaf-refs. See ``yang/sysrepo-mcp.yang`` for the server module or ``oven.yang`` for a concrete example.

   Parameters:

   - ``module`` (string, required): YANG module name (e.g., ``"sysrepo-mcp"`` for
     the server module from ``yang/sysrepo-mcp.yang``, or ``"oven"`` for
     the oven example from ``extern/sysrepo/examples/plugin/oven.yang``)
   - ``revision`` (string, optional): Module revision date (default: latest)
   - ``path`` (string, optional): Sub-path within the module (default: root)
   - ``with-comments`` (boolean, optional): Include description/mandatory
     attributes (default: ``false``)

   Returns:

   - ``tree`` (object): Hierarchical representation of the YANG schema
   - ``nodes`` (array): Flat list of all nodes with their XPath expressions
   - ``references`` (array): List of module dependencies

YANG Help
---------

The server **will provide** a ``get_help`` tool that returns documentation for
a specific YANG node or XPath expression (e.g., from ``yang/sysrepo-mcp.yang`` or ``extern/sysrepo/examples/plugin/oven.yang``).

**Note**: This feature is planned but not yet implemented.

**get_help**
   Returns documentation and usage information for a YANG node or XPath.

   Parameters:

   - ``xpath`` (string, required): XPath expression to query (e.g.,
     ``"/sysrepo-mcp:server-state/version"`` for the server module from
     ``yang/sysrepo-mcp.yang``, or ``"/oven:oven/temperature"`` for the oven module from
     ``extern/sysrepo/examples/plugin/oven.yang``)
   - ``module`` (string, optional): YANG module name (auto-detected from
     xpath if not provided)

   Returns:

   - ``path`` (string): The matched XPath
   - ``node_type`` (string): YANG node type (leaf, container, list, etc.)
   - ``description`` (string): YANG description statement
   - ``mandatory`` (boolean): Whether the node is mandatory
   - ``default_value`` (string, optional): Default value if specified
   - ``possible_values`` (array, optional): Enum or enumeration values
   - ``example`` (string, optional): Example usage

Logging with elog
-----------------

When ``CONFIG_SYSREPO_MCP_SERVER_SYSLOG`` is enabled, sysrepo-mcp **will use**
the `elog <https://github.com/grgbr/elog>`_ library for syslog management.

**Note**: elog integration is planned but not yet implemented in this skeleton.

elog provides a command-line argument parser for log configuration, making it
simple and flexible.

.. note::

   Logging configuration details will be added when elog integration is
   implemented.

Deployment Considerations
-------------------------

Security Best Practices
~~~~~~~~~~~~~~~~~~~~~~~

1. **Enable Access Control**: Always enable access control in production
   (planned feature).

2. **Use Bearer Authentication**: Prefer Bearer token authentication over
   cookie-based authentication (planned feature).

3. **Enable TLS**: Terminate TLS at the reverse proxy (lighttpd) and use
   HTTPS for all client connections.

4. **Restrict API Keys**: Use strong, random API keys and rotate them
   regularly (planned feature).

5. **Configure NACM**: Configure NACM users and permissions appropriately
   for your deployment (planned feature).

6. **Limit Exposed Tools**: Only expose the tools your agents need
   (planned feature).

Performance Considerations
~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Session Management**: Configure session timeout and maximum sessions
   appropriately for your deployment (planned feature).

2. **Concurrency**: FastCGI handles concurrency via the reverse proxy's worker
   threads. Tune worker count based on expected load.

3. **Logging**: Use appropriate log levels to minimize overhead (planned
   feature).

Monitoring and Observability
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Log Monitoring**: Monitor logs for errors and warnings (planned feature).

2. **System Status**: Use the ``get_status`` tool to check server health
   (planned feature).

3. **YANG Explorer**: Use the ``get_tree`` tool to discover the YANG schema
   structure (planned feature).

4. **YANG Help**: Use the ``get_help`` tool to get documentation for specific
   YANG nodes (planned feature).

Future Considerations
---------------------

The following features are **NOT planned** for future versions:

- **SSE Support**: Will NOT be implemented (incompatible with FastCGI)
- **WebSocket Support**: Will NOT be implemented (incompatible with FastCGI)
- **Socket-based transport**: Will NOT be implemented (FastCGI only)

Features that **may be considered** for future versions:

- **Authentication**: Add OAuth2, JWT, and other authentication methods
- **Encryption**: Add end-to-end encryption for MCP messages
- **Horizontal Scaling**: Support for multiple server instances behind a
  load balancer with session affinity

Summary
-------

This chapter described the architecture of sysrepo-mcp, including its
placement in the system, communication protocols, and configuration models.

Key points:

- sysrepo-mcp bridges MCP and sysrepo, exposing NETCONF operations as
  MCP tools
- The server links the sysrepo library at build time and connects directly
  to the datastore files, without a separate daemon
- **FastCGI transport only** via reverse proxy (lighttpd, nginx)
- **SSE will NOT be implemented** (incompatible with FastCGI)
- Build-time configuration via Kconfig/Config.in
- elog will provide enhanced syslog management (planned feature)
- Access control will integrate with sysrepo NACM (planned feature)
