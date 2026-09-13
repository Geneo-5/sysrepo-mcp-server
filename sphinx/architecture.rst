.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Architecture
============

This chapter describes the architecture of sysrepo-mcp, including its
placement in the system, communication protocols, and configuration models.

Overview
--------

sysrepo-mcp is a bridge between the `Model Context Protocol
<https://modelcontextprotocol.io>`_ (MCP) and `sysrepo
<https://github.com/sysrepo/sysrepo>`_, a NETCONF configuration library. It
exposes sysrepo operations as MCP tools that an AI agent can invoke.

The server communicates directly with the sysrepo library (linked at build
time) to access the YANG datastore. No separate sysrepo daemon is required.
The server connects to the datastore files through the sysrepo C API
(``sr_conn_open_session()``, ``sr_get_items()``, etc.).

For transport, sysrepo-mcp uses a **stream-based** connection (Unix socket or
TCP). A front-end reverse proxy (e.g. **lighttpd**) can forward HTTP
connections to sysrepo-mcp via a TCP stream. This design allows the server to
run as a standalone process managing its own I/O without FastCGI overhead.

.. note::

   The current implementation supports **stream-based transport** over Unix
   sockets or TCP. Future versions may add **Server-Sent Events (SSE)**
   support for streaming MCP responses. All MCP communication currently uses
   HTTP GET and POST.

Architecture Diagram
--------------------

::

   +-------------+      +----------------------+      +-----------------+
   |   AI Agent  |<---->|  Reverse Proxy       |<---->|  sysrepo-mcp    |
   | (OpenHands  |      |  (lighttpd)          |<---->|  (Stream)       |
   |  SDK, etc.) |      |                      |      +-----------------+
   +-------------+      |  - HTTP/HTTPS        |                  |
                        |  - TLS termination   |                  |
                        |  - Load balancing    |                  |
                        |  - Rate limiting     |                  |
                        +----------------------+                  |
                                                                \(\bigtriangledown\)
                                                      +-----------------+
                                                      |  Sysrepo Lib. |
                                                      |  (linked)     |
                                                      +--------+--------+
                                                               |
                                                               \(\bigtriangledown\)
                                                      +-----------------+
                                                      |  YANG Models    |
                                                      |  (Datastore)    |
                                                      +-----------------+

Components
----------

1. **AI Agent**: The client that communicates with sysrepo-mcp via
   MCP. This could be OpenHands, a custom agent, or any MCP-compatible client.

2. **Reverse Proxy (lighttpd)**: Handles HTTP/HTTPS connections, TLS
   termination, and forwards MCP requests to sysrepo-mcp via a stream
   (Unix socket or TCP).

3. **sysrepo-mcp**: The core application that:

   - Accepts stream connections from the proxy (Unix socket or TCP)
   - Parses MCP JSON-RPC messages
   - Calls the sysrepo library directly (``sr_conn_open_session()``,
     ``sr_get_items()``, ``sr_edit_item()``, etc.)
   - Returns MCP responses to the agent

4. **Sysrepo Library**: A C library linked into sysrepo-mcp that provides
   NETCONF datastore operations. The library accesses YANG model data
   directly from the filesystem (``/etc/sysrepo/data/``) without a
   separate daemon process.

5. **YANG Models**: Schema definitions that define the configuration and
   operational state data accessible through sysrepo.

Communication Protocol
----------------------

The server implements the **MCP Streamable HTTP** transport over a stream
(Unix socket or TCP). No SSE support yet; all communication occurs through
standard HTTP GET and POST requests.

Request Flow
~~~~~~~~~~~~

1. The AI agent sends an HTTP POST to the MCP endpoint (e.g. ``/mcp``).

2. The request includes:

   - ``Content-Type: application/json``
   - ``MCP-Session-Id`` header (for session management)
   - JSON-RPC payload with method and parameters

3. lighttpd forwards the request to sysrepo-mcp via a TCP stream (or
   directly to a Unix socket in standalone mode).

4. sysrepo-mcp processes the request and returns an HTTP response
   with the MCP result.

5. The agent receives the response and processes the result.

.. code-block:: http

   POST /mcp HTTP/1.1
   Host: example.com
   Content-Type: application/json
   MCP-Session-Id: abc123
   Authorization: Bearer <api-key>

   {
       "jsonrpc": "2.0",
       "id": 1,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "module": "ietf-interfaces",
               "path": "/interfaces"
           }
       }
   }

Session Management
~~~~~~~~~~~~~~~~~~

Sessions are managed through the ``MCP-Session-Id`` HTTP header. The server
maintains session state and associates requests with their respective sessions.

- **Session Creation**: First request without a session ID creates a new
  session. The server responds with a ``MCP-Session-Id`` header.

- **Session Validation**: Subsequent requests must include the session ID.
  Invalid or expired session IDs are rejected with HTTP 401.

- **Session TTL**: Sessions idle for longer than ``MCP_SESSION_TTL`` seconds
  are automatically destroyed (default: 1800 seconds).

- **Max Sessions**: The server enforces a maximum number of concurrent
  sessions (default: 64). New sessions are rejected with HTTP 503 when
  this limit is reached.

Configuration Models
--------------------

sysrepo-mcp supports two independent configuration models:

1. **Build-time Configuration** (Config.in)
2. **Runtime Configuration** (via sysrepo YANG models)

Build-time Configuration
~~~~~~~~~~~~~~~~~~~~~~~~

Build-time configuration is defined in ``config.in`` (Kconfig format) and
determines the server's capabilities at compile time.

Configuration Options
^^^^^^^^^^^^^^^^^^^^^

Server Configuration
""""""""""""""""""""

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_LISTEN_ADDRESS``
     - ``127.0.0.1``
     - Address to bind for MCP connections (Unix/TCP)
   * - ``CONFIG_SYSREPO_MCP_SERVER_LISTEN_PORT``
     - ``8080``
     - TCP port for MCP connections (TCP mode only)
   * - ``CONFIG_SYSREPO_MCP_SERVER_MCP_SOCK_PATH``
     - ``/var/run/sysrepo-mcp.sock``
     - Unix socket path for standalone mode

Access Control
""""""""""""""

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_ACL_ENABLED``
     - ``y``
     - Enable access control (authentication + NACM)
   * - ``CONFIG_SYSREPO_MCP_SERVER_AUTH_BEARER``
     - ``y``
     - Enable Bearer token authentication
   * - ``CONFIG_SYSREPO_MCP_SERVER_AUTH_COOKIE``
     - ``n``
     - Enable cookie-based authentication

Sysrepo Connection
""""""""""""""""""

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_USERNAME``
     - ``mcp``
     - Username used to connect to the sysrepo datastore (library API)
   * - ``CONFIG_SYSREPO_MCP_SERVER_TIMEOUT``
     - ``5000``
     - Sysrepo operation timeout (ms)

.. note::

   Since sysrepo is a library (not a daemon), there is no socket path to
   configure. The library accesses datastore files directly from the
   filesystem. The ``username`` is used when creating sysrepo sessions
   (``sr_session_create()``) and is subject to NACM rules.

Logging Configuration
"""""""""""""""""""""

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_SYSLOG``
     - ``y``
     - Enable syslog logging
   * - ``CONFIG_SYSREPO_MCP_SERVER_VERBOSE``
     - ``n``
     - Enable verbose debugging

.. note::

   When ``CONFIG_SYSREPO_MCP_SERVER_SYSLOG`` is enabled, the server uses the
   `elog <https://github.com/grgbr/elog>`_ library for enhanced syslog
   management. elog provides structured logging with severity levels,
   facility codes, and application-specific tags.

Runtime Configuration (YANG)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Runtime configuration is managed through sysrepo itself. A dedicated YANG
module (``sysrepo-mcp``) defines the configuration schema for:

- API keys and token management
- NACM user provisioning
- Logging settings
- Server behavior parameters

YANG Module Structure
"""""""""""""""""""""

The ``sysrepo-mcp`` YANG module defines the following
configuration sections:

Server Configuration
^^^^^^^^^^^^^^^^^^^^

.. code-block:: yang

   module sysrepo-mcp {
       namespace "urn:sysrepo-mcp:yang:sysrepo-mcp";
       prefix "sysrepo-mcp";

       container mcp-server {
           // Server settings
           container server {
               leaf session-timeout-seconds {
                   type uint32;
                   default 1800;
               }
               leaf max-sessions {
                   type uint32;
                   default 64;
               }
           }

           // API keys with user mapping (key + NACM user pairs)
           list api-key {
               key "id";
               description
                   "Registered API keys with associated NACM user. "
                   "Each entry is a pair of (key-value, username) that "
                   "maps an authentication token to a NACM user identity. "
                   "The username references a user defined in ietf-netconf-acm.";
               leaf id {
                   type string;
               }
               leaf key-value {
                   type string;
                   description
                       "The actual API key value (stored securely).";
               }
               leaf username {
                   type string;
                   description
                       "NACM user associated with this API key. "
                       "This user identity is used for NACM access control. "
                       "Users are defined in ietf-netconf-acm.";
               }
           }
       }
   }

Configuration Flow
~~~~~~~~~~~~~~~~~~

.. code-block:: text

   1. Agent sends configuration via MCP tools
      
   2. sysrepo-mcp receives YANG data
      
   3. Data validated against sysrepo-mcp schema
      
   4. Configuration applied to running server
      
   5. Changes persisted in sysrepo datastore
      
   6. Server re-reads configuration (or on SIGHUP)

Authentication Models
---------------------

sysrepo-mcp supports two authentication modes, selected at build time:

Bearer Token Authentication
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The recommended mode for agent-to-server communication. Clients send an HTTP
``Authorization: Bearer <key>`` header with each request.

**Workflow:**

1. Agent includes ``Authorization: Bearer <api-key>`` header
2. Server validates key against registered keys (from YANG config)
3. If valid, server maps key to NACM user
4. Server creates sysrepo session with mapped user credentials
5. Request is processed with user's NACM permissions

**Security Considerations:**

- API keys are compared using constant-time comparison to prevent timing attacks
- Keys are stored securely in sysrepo datastore (encrypted at rest)
- Keys are loaded from file on SIGHUP for rotation without restart
- Invalid keys receive HTTP 401 with generic error message

Cookie-Based Authentication
~~~~~~~~~~~~~~~~~~~~~~~~~~~

An alternative mode for browser-based or reverse-proxy-authenticated
scenarios. Clients send a cookie with the API key.

**Workflow:**

1. Client sends HTTP request with cookie ``mcp_session=<api-key>``
2. Server validates cookie name against ``ACL_COOKIE_NAME`` configuration
3. Server validates key against registered keys
4. If valid, server processes request with mapped NACM permissions

**Use Cases:**

- Integration with existing cookie-based session management
- Reverse proxy that handles authentication and passes session info
- Testing and development scenarios

.. note::

   Bearer token authentication is recommended for production deployments.

Sysrepo NACM Integration
-------------------------

When access control is enabled, sysrepo-mcp integrates with sysrepo's
`Native Access Control Module (NACM) <https://www.rfc-editor.org/rfc/rfc6536>`_
to enforce fine-grained access control.

Access Control Flow
~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   Client Request
       
   API Key Validation (sysrepo-mcp)
       
   NACM User Mapping (from YANG config)
       
   Sysrepo Session Creation (with NACM user)
       
   Request Processing (enforcing NACM rules)
       
   Response to Client

NACM User Mapping
~~~~~~~~~~~~~~~~~

Each API key can be mapped to a specific NACM user. The mapping is configured
in the YANG module (see :ref:`YANG Module Structure`).

- **Explicit Mapping**: API keys can be explicitly mapped to NACM users
- **Default User**: When an API key has no explicit mapping, a default NACM
  user is used (configured via ``ACL_NACM_USER``)
- **Group Membership**: NACM users can belong to multiple groups, enabling
  granular permission management

Access Policy
~~~~~~~~~~~~~

sysrepo-mcp enforces access policies based on:

- **API Key**: Valid/invalid credentials
- **NACM User**: Mapped user's permissions
- **Module Access**: Per-module allow/deny lists
- **Operation Type**: Read-only vs read-write operations

.. list-table::
   :header-rows: 1
   :widths: 20 20 20 20 20

   * - API Key
     - NACM User
     - Module
     - Operation
     - Result
   * - Valid
     - operators
     - ietf-interfaces
     - get-config
     -  Allowed
   * - Valid
     - operators
     - ietf-interfaces
     - edit-config
     -  Allowed (RW)
   * - Valid
     - operators
     - ietf-system
     - get-config
     -  Allowed (RO)
   * - Valid
     - operators
     - ietf-system
     - edit-config
     - \texttt{\textbackslash{}texttt\{\textbackslash{}backslash\}} Denied (RO module)
   * - Invalid
     - (none)
     - (any)
     - (any)
     - \texttt{\textbackslash{}texttt\{\textbackslash{}backslash\}} 401 Unauthorized

Stream Integration
------------------

sysrepo-mcp uses a stream-based connection (Unix socket or TCP) to
communicate with a front-end reverse proxy (lighttpd). The server can
run in two modes:

**Stand-alone mode (development):**
The server listens on a Unix socket or TCP port and accepts connections
directly. Useful for testing without lighttpd.

**Proxy mode (production):**
lighttpd accepts HTTP/HTTPS requests and forwards them to sysrepo-mcp
over a TCP stream. This is the recommended production configuration.

.. note::

   Future versions may add **Server-Sent Events (SSE)** support for
   streaming MCP responses. The current implementation uses HTTP GET/POST
   over a stream for all communication.

Build Configuration
~~~~~~~~~~~~~~~~~~~

The transport mode is selected at build time via Config.in:

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_TRANSPORT``
     - ``tcp``
     - Transport type (``unix`` or ``tcp``)
   * - ``CONFIG_SYSREPO_MCP_SERVER_SOCKET_PATH``
     - (varies)
     - Unix socket path (when transport = ``unix``)

.. note::

   The lighttpd configuration is managed separately. Refer to the lighttpd
   documentation for stream/FastCGI module setup. sysrepo-mcp only needs to
   accept stream connections on the configured socket/address.

Sysrepo Library Connection
--------------------------

sysrepo-mcp links the sysrepo library at build time and uses the sysrepo
C API directly to perform NETCONF operations. This section describes the
connection details.

Connection Management
~~~~~~~~~~~~~~~~~~~~~

- **Session Management**: Each MCP session creates a sysrepo session via
  ``sr_session_create()``. The session uses the NACM user mapped from
  the API key.
- **Datastore Access**: The library reads/writes YANG data directly from
  the filesystem (``/etc/sysrepo/data/``).
- **Error Handling**: Connection failures are handled gracefully with
  retries and appropriate error messages.

Sysrepo Operations
~~~~~~~~~~~~~~~~~~

The server exposes the following sysrepo library operations as MCP tools.
Each tool requires specific parameters from the client.

**Configuration Operations:**

``sr_get_config``
   Read configuration from a YANG module.

   Parameters:

   - ``module`` (string, required): YANG module name (e.g. ``"ietf-interfaces"``)
   - ``xpath`` (string, optional): XPath filter to select specific nodes
   - ``datastore`` (string, optional): Which datastore - ``"running"``, ``"startup"``, or ``"candidate"`` (default: ``"running"``)
   - ``depth`` (string, optional): How deep to traverse (``"shallow"``, ``"deep"``, ``"children"``)

``sr_edit_config``
   Apply configuration changes to a YANG module.

   Parameters:

   - ``module`` (string, required): YANG module name
   - ``config`` (string, required): XML configuration fragment to apply
   - ``target`` (string, optional): Target datastore (``"running"``, ``"startup"``, ``"candidate"``)
   - ``xpath`` (string, optional): XPath to target specific nodes within the module

**Operational Data:**

``sr_get_operational``
   Read operational state data from the datastore.

   Parameters:

   - ``xpath`` (string, required): XPath expression selecting the data to retrieve
   - ``datastore`` (string, optional): Which datastore (default: ``"operational"``)
   - ``depth`` (string, optional): Traversal depth

**Subscriptions:**

``sr_subscribe_oper_changes``
   Subscribe to operational data changes (push notifications).

   Parameters:

   - ``xpath`` (string, required): XPath expression to monitor for changes
   - ``cb_type`` (string, optional): Callback type (``"sr_info_cb"`` or ``"sr_ev_change_cb"``)
   - ``depth`` (string, optional): Subscription depth

``sr_subscribe_notifs``
   Subscribe to sysrepo notifications.

   Parameters:

   - ``xpath`` (string, required): XPath expression for the notification
   - ``event_type`` (string, optional): Event type filter

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
   - ``input_params`` (object, optional): RPC input parameters as key-value pairs
   - ``xpath`` (string, optional): XPath for targeted operations

``sr_action``
   Execute a YANG action (RPC-style tool defined in a YANG model).

   Parameters:

   - ``module`` (string, required): YANG module containing the action
   - ``action_name`` (string, required): Action name (e.g. ``"reset-interface"``)
   - ``input_params`` (object, optional): Action input parameters
   - ``xpath`` (string, optional): XPath to target specific data nodes

.. note::

   Not all operations are exposed by default. Use ``EXPOSE_*`` configuration
   options to control which tools are available to agents.

.. note::

   For operations that use ``xpath``, use the ``get_tree`` tool (see below)
   to discover the YANG tree structure and construct valid XPath expressions.

System Status
-------------

The server exposes a ``get_status`` tool that returns server health
information.

**get_status**
   Returns server status information.

   Parameters:

   - ``verbose`` (boolean, optional): Include detailed session information
     (default: ``false``). When ``true``, active session IDs are included.
     **Security note**: In high-security deployments, restrict this tool
     to a dedicated admin role. Exposing session IDs reveals internal
     identifiers that could be used for session hijacking attempts.

   Returns:

   - ``version`` (string): Server version
   - ``uptime_seconds`` (integer): Server uptime in seconds
   - ``active_sessions`` (integer): Number of active sessions
   - ``max_sessions`` (integer): Maximum concurrent sessions configured
   - ``session_ids`` (array of strings, optional): List of active session IDs
     (only present when ``verbose`` is ``true``)
   - ``configured`` (object): Current server configuration summary

YANG Tree Explorer
------------------

The server provides a ``get_tree`` tool to discover the YANG schema
structure, which helps agents construct valid XPath expressions.

**get_tree**
   Returns the YANG schema tree for a module, showing all nodes, lists,
   keys, and leaf-refs.

   Parameters:

   - ``module`` (string, required): YANG module name (e.g. ``"ietf-interfaces"``)
   - ``revision`` (string, optional): Module revision date (default: latest)
   - ``path`` (string, optional): Sub-path within the module (default: root)
   - ``with-comments`` (boolean, optional): Include description/mandatory
     attributes (default: ``false``)

   Returns:

   - ``tree`` (object): Hierarchical representation of the YANG schema
   - ``nodes`` (array): Flat list of all nodes with their XPath expressions
   - ``references`` (array): List of module dependencies

Example:

.. code-block:: json

   {
       "module": "ietf-interfaces",
       "path": "/interfaces"
   }

   Response:
   {
       "tree": { ... },
       "nodes": [
           { "path": "/interfaces/interface", "type": "container" },
           { "path": "/interfaces/interface[name='eth0']", "type": "list entry" },
           { "path": "/interfaces/interface[name='eth0']/config/enabled", "type": "leaf" }
       ],
       "references": ["ietf-yang-types"]
   }

YANG Help
---------

The server provides a ``get_help`` tool that returns documentation for
a specific YANG node or XPath expression.

**get_help**
   Returns documentation and usage information for a YANG node or XPath.

   Parameters:

   - ``xpath`` (string, required): XPath expression to query (e.g.
     ``"/interfaces/interface[name='eth0']/config/enabled"``)
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

Example:

.. code-block:: json

   {
       "xpath": "/interfaces/interface[name='eth0']/config/enabled"
   }

   Response:
   {
       "path": "/interfaces/interface[name='eth0']/config/enabled",
       "node_type": "leaf",
       "description": "Allows the user to configure the enabled state of the interface.",
       "mandatory": false,
       "default_value": "true",
       "possible_values": ["true", "false"],
       "example": "Set to 'false' to administratively disable the interface."
   }

Logging with elog
-----------------

When ``CONFIG_SYSREPO_MCP_SERVER_SYSLOG`` is enabled, sysrepo-mcp uses
the `elog <https://github.com/grgbr/elog>`_ library for syslog management.
elog provides a command-line argument parser for log configuration, making it
simple and flexible.

elog Command-Line Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~

elog is invoked with the following command-line options (parsed by elog's
built-in parser):

.. code-block:: bash

   sysrepo-mcp --name sysrepo-mcp \
                       --log 0 \
                       --console 0 \
                       [--socket /var/run/elog.sock]

Common options:

* ``--name <name>``: Application name (used in log messages)
* ``--log <level>``: Log level (0-8, see below)
* ``--console <0|1>``: Enable/disable console logging
* ``--socket <path>``: Use elog Unix socket (instead of syslog)
* ``--no-daily-rotate``: Disable daily log rotation
* ``--no-syslog``: Disable syslog, use file logging only

Log Levels
~~~~~~~~~~

elog supports the following log levels (from highest to lowest severity):

#. **0 (LOG_EMERG)**: System is unusable
#. **1 (LOG_ALERT)**: Action must be taken immediately
#. **2 (LOG_CRIT)**: Critical conditions
#. **3 (LOG_ERR)**: Error conditions
#. **4 (LOG_WARNING)**: Warning conditions
#. **5 (LOG_NOTICE)**: Normal but significant conditions
#. **6 (LOG_INFO)**: Informational messages
#. **7 (LOG_DEBUG)**: Debug-level messages

.. note::

   elog's log level is **inverted** compared to syslog: 0 is the most
   severe, 7 is the least.

Configuration by Kconfig Defaults
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Kconfig (``config.in``) sets default values for elog arguments. These defaults
are baked into the binary at build time and can be overridden at runtime via
command-line arguments.

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_SYSLOG``
     - ``y``
     - Enable syslog integration (passed to elog)
   * - ``CONFIG_SYSREPO_MCP_SERVER_VERBOSE``
     - ``n``
     - Enable verbose debugging (sets elog log level to debug)
   * - ``CONFIG_SYSREPO_MCP_SERVER_LOG_LEVEL``
     - ``6 (info)``
     - Default log level (0-7)
   * - ``CONFIG_SYSREPO_MCP_SERVER_CONSOLE_LOG``
     - ``n``
     - Enable console logging (useful for development)

Command-Line Override
~~~~~~~~~~~~~~~~~~~~~

Command-line arguments always override Kconfig defaults. Example:

.. code-block:: bash

   # Use debug logging despite info default in Config.in
   sysrepo-mcp --log 7

   # Disable syslog, log to file only
   sysrepo-mcp --no-syslog

   # Use elog Unix socket instead of syslog
   sysrepo-mcp --socket /var/run/elog.sock

.. note::

   elog's command-line parser is built into the server binary. The parsed
   arguments determine elog's behavior at runtime. No YAML or configuration
   file is needed for logging.

Deployment Considerations
-------------------------

Security Best Practices
~~~~~~~~~~~~~~~~~~~~~~~

1. **Enable Access Control**: Always enable access control in production
   (``ACL_ENABLED = y``)

2. **Use Bearer Authentication**: Prefer Bearer token authentication over
   cookie-based authentication

3. **Enable TLS**: Terminate TLS at the reverse proxy (lighttpd) and use
   HTTPS for all client connections

4. **Restrict API Keys**: Use strong, random API keys and rotate them
   regularly

5. **Configure NACM**: Configure NACM users and permissions appropriately
   for your deployment

6. **Limit Exposed Tools**: Only expose the tools your agents need
   (disable ``EXPOSE_*`` options you don't need)

Performance Considerations
~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Session Management**: Configure ``MCP_SESSION_TTL`` and
   ``MCP_MAX_SESSIONS`` appropriately for your deployment.
   Each session creates a sysrepo session (``sr_session_create()``).
2. **Concurrency**: The number of concurrent sessions is bounded by
   ``MCP_MAX_SESSIONS`` (default: 64). Adjust based on expected load.
3. **Logging**: Use appropriate log levels to minimize overhead.
4. **I/O**: In proxy mode, lighttpd worker threads handle I/O. Tune
   worker count based on expected concurrency.

Monitoring and Observability
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Log Monitoring**: Monitor elog logs for errors and warnings.
2. **System Status**: Use the ``get_status`` tool to check server health,
   session count, uptime, and active session IDs.
3. **YANG Explorer**: Use the ``get_tree`` tool to discover the YANG schema
   structure and construct valid XPath expressions.
4. **YANG Help**: Use the ``get_help`` tool to get documentation for specific
   YANG nodes and XPath expressions.
5. **Health Checks**: Implement health check endpoints (e.g.,
   ``/health``).
6. **Alerting**: Configure alerts for critical events (e.g., connection
   failures, authentication failures).

.. security_note::

   The ``get_status`` tool with ``verbose=true`` exposes active session
   IDs, which reveals internal session identifiers to authenticated users.
   In high-security deployments, restrict this tool to a dedicated admin
   role only. Exposing session IDs could be used for session hijacking
   attempts if session tokens are compromised.

Future Considerations
---------------------

The following features are planned for future versions:

- **SSE Support**: Add Server-Sent Events for streaming MCP responses
  over the existing stream transport.
- **WebSocket Support**: Add WebSocket support for bidirectional
  communication.
- **gRPC Support**: Add gRPC support for high-performance scenarios.
- **Authentication**: Add OAuth2, JWT, and other authentication methods.
- **Encryption**: Add end-to-end encryption for MCP messages.
- **Horizontal Scaling**: Support for multiple server instances behind a
  load balancer with session affinity.

Summary
-------

This chapter described the architecture of sysrepo-mcp, including its
placement in the system, communication protocols, and configuration models.

Key points:

- sysrepo-mcp bridges MCP and sysrepo, exposing NETCONF operations as
  MCP tools.
- The server links the sysrepo library at build time and connects directly
  to the datastore files, without a separate daemon.
- Stream-based transport over Unix socket or TCP. SSE support is planned
  for future versions.
- Two authentication modes are supported: Bearer token and cookie-based.
- Runtime configuration is managed through sysrepo YANG models (``api-key``
  list; NACM users are managed by ``ietf-netconf-acm``).
- elog provides enhanced syslog management for production deployments.
- Access control integrates with sysrepo NACM for fine-grained permissions.