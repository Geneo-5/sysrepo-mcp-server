################################################################################
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of sysrepo-mcp-server.
# Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>
################################################################################

Architecture
============

This chapter describes the architecture of sysrepo-mcp-server, including its
placement in the system, communication protocols, and configuration models.

Overview
--------

sysrepo-mcp-server is a bridge between the `Model Context Protocol
<https://modelcontextprotocol.io>`_ (MCP) and `sysrepo
<https://github.com/sysrepo/sysrepo>`_, a NETCONF configuration store. It
exposes sysrepo operations as MCP tools that an AI agent can invoke.

The server uses **FastCGI** as its transport layer. A front-end reverse proxy
(e.g. **lighttpd**) handles HTTP connections and forwards requests to the
server via FastCGI. This design separates network handling from application
logic and allows the server to run as a FastCGI application without managing
HTTP parsing itself.

.. note::

   The current implementation does **not** support Server-Sent Events (SSE).
   All MCP communication uses HTTP GET and POST via FastCGI. Future versions
   may add streaming support through additional FastCGI extensions.

Architecture Diagram
--------------------

::

   +-------------+      +----------------------+      +-----------------+
   |   AI Agent  |<---->|  Reverse Proxy       |<---->|  sysrepo-mcp    |
   | (OpenHands  |      |  (lighttpd)          |      |  Server         |
   |  SDK, etc.) |      |                      |      |  (FastCGI)      |
   +-------------+      |  - HTTP/HTTPS        |      +--------+--------+
                        |  - TLS termination   |               |
                        |  - Load balancing    |               |
                        |  - Rate limiting     |               |
                        +----------------------+               |
                                                               \(\bigtriangledown\)
                                                      +-----------------+
                                                      |   Sysrepo       |
                                                      |   Daemon        |
                                                      |   (sysrepod)    |
                                                      +--------+--------+
                                                               |
                                                               \(\bigtriangledown\)
                                                      +-----------------+
                                                      |  YANG Models    |
                                                      |  (Datastore)    |
                                                      +-----------------+

Components
----------

1. **AI Agent**: The client that communicates with sysrepo-mcp-server via
   MCP. This could be OpenHands, a custom agent, or any MCP-compatible client.

2. **Reverse Proxy (lighttpd)**: Handles HTTP/HTTPS connections, TLS
   termination, and forwards MCP requests to sysrepo-mcp-server via FastCGI.

3. **sysrepo-mcp-server**: The core application that:

   - Receives FastCGI requests from the proxy (or standalone Unix/TCP socket)
   - Parses MCP JSON-RPC messages
   - Executes sysrepo operations (read, write, subscribe)
   - Returns MCP responses to the agent

4. **Sysrepo Daemon (sysrepod)**: The NETCONF configuration store that
   manages YANG models and their data.

5. **YANG Models**: Schema definitions that define the configuration and
   operational state data accessible through sysrepo.

Communication Protocol
----------------------

The server implements the **MCP Streamable HTTP** transport without SSE
support. All communication occurs through standard HTTP GET and POST requests
forwarded via FastCGI.

Request Flow
~~~~~~~~~~~~

1. The AI agent sends an HTTP POST to the MCP endpoint (e.g. ``/mcp``).

2. The request includes:

   - ``Content-Type: application/json``
   - ``MCP-Session-Id`` header (for session management)
   - JSON-RPC payload with method and parameters

3. lighttpd forwards the request to sysrepo-mcp-server via FastCGI.

4. sysrepo-mcp-server processes the request and returns an HTTP response
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

sysrepo-mcp-server supports two independent configuration models:

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
   * - ``CONFIG_SYSREPO_MCP_SERVER_AUTH_KEYS_FILE``
     - ``/etc/sysrepo-mcp/keys``
     - Path to file containing API keys

Sysrepo Connection
""""""""""""""""""

.. list-table::
   :header-rows: 1
   :widths: 30 10 60

   * - Option
     - Default
     - Description
   * - ``CONFIG_SYSREPO_MCP_SERVER_SYSREPO_SOCK_PATH``
     - ``/var/run/sysrepod.sock``
     - Sysrepo daemon socket path
   * - ``CONFIG_SYSREPO_MCP_SERVER_SYSREPO_USER``
     - ``mcp``
     - Sysrepo username for NACM
   * - ``CONFIG_SYSREPO_MCP_SERVER_SYSREPO_TIMEOUT``
     - ``5000``
     - Sysrepo operation timeout (ms)

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
module (``sysrepo-mcp-server``) defines the configuration schema for:

- API keys and token management
- NACM user provisioning
- Logging settings
- Server behavior parameters

YANG Module Structure
"""""""""""""""""""""

The ``sysrepo-mcp-server`` YANG module defines the following
configuration sections:

MCP Server Configuration
^^^^^^^^^^^^^^^^^^^^^^^^

.. code-block:: yang

   module sysrepo-mcp-server {
       namespace "urn:sysrepo-mcp-server:yang:sysrepo-mcp-server";
       prefix "sysrepo-mcp";

       import ietf-acm {
           prefix acm;
       }

       container mcp-server {
           // Server settings
           container server {
               leaf host {
                   type inet:ip-address;
                   default "127.0.0.1";
               }
               leaf port {
                   type uint16;
                   default 8080;
               }
               leaf endpoint {
                   type string;
                   default "/mcp";
               }
               leaf session-timeout-seconds {
                   type uint32;
                   default 1800;
               }
               leaf max-sessions {
                   type uint32;
                   default 64;
               }
           }

           // API keys and authentication
           list api-key {
               key "id";
               description
                   "Registered API keys for agent authentication.";
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
                       "NACM user associated with this API key.";
               }
           }

           // NACM user configuration
           list nacm-user {
               key "name";
               description
                   "NACM users provisioned for this server.";
               leaf name {
                   type string;
               }
               leaf-list group {
                   type string;
                   description
                       "Groups this user belongs to.";
               }
           }

           // Logging configuration (runtime defaults only)
           // Note: Actual logging behavior is controlled by elog's
           // command-line parser. Kconfig (config.in) sets defaults.
           // This container allows runtime override of logging defaults.
           container logging {
               leaf level {
                   type uint32 {
                       range "0..7";
                   }
                   default 6;
                   description
                       "Default log level (overridden by command-line).";
               }
               leaf console {
                   type boolean;
                   default false;
                   description
                       "Enable console logging (overridden by --console).";
               }
               leaf socket-path {
                   type string;
                   default "";
                   description
                       "Elog Unix socket path (overridden by --socket).";
               }
           }

           // Sysrepo connection
           container sysrepo {
               leaf socket-path {
                   type string;
                   default "/var/run/sysrepo/sysrepod.sock";
               }
               leaf username {
                   type string;
                   default "mcp";
               }
               leaf password {
                   type string;
               }
               leaf connection-timeout-ms {
                   type uint32;
                   default 5000;
               }
           }
       }
   }

Configuration Flow
~~~~~~~~~~~~~~~~~~

.. code-block:: text

   1. Agent sends configuration via MCP tools
      
   2. sysrepo-mcp-server receives YANG data
      
   3. Data validated against sysrepo-mcp-server schema
      
   4. Configuration applied to running server
      
   5. Changes persisted in sysrepo datastore
      
   6. Server re-reads configuration (or on SIGHUP)

Authentication Models
---------------------

sysrepo-mcp-server supports two authentication modes, selected at build time:

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

When access control is enabled, sysrepo-mcp-server integrates with sysrepo's
`Native Access Control Module (NACM) <https://www.rfc-editor.org/rfc/rfc6536>`_
to enforce fine-grained access control.

Access Control Flow
~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   Client Request
       
   API Key Validation (sysrepo-mcp-server)
       
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

sysrepo-mcp-server enforces access policies based on:

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

FastCGI Integration
-------------------

sysrepo-mcp-server implements the FastCGI protocol to communicate with a
front-end reverse proxy (lighttpd). The server can run in two modes:

**Stand-alone mode (development):**
The server listens on a Unix socket or TCP port and accepts FastCGI
connections directly. Useful for testing without lighttpd.

**Proxy mode (production):**
lighttpd accepts HTTP/HTTPS requests and forwards them to sysrepo-mcp-server
via FastCGI. This is the recommended production configuration.

Build Configuration
~~~~~~~~~~~~~~~~~~~

The FastCGI transport mode is selected at build time via Config.in:

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
   documentation for FastCGI module setup. sysrepo-mcp-server only needs to
   accept FastCGI connections on the configured socket/address.

Sysrepo Connection
------------------

sysrepo-mcp-server connects to the sysrepo daemon (sysrepod) to perform
NETCONF operations. This section describes the sysrepo connection details.

Connection Management
~~~~~~~~~~~~~~~~~~~~~

- **Connection Pooling**: The server maintains a pool of connections to sysrepod
- **Session Management**: Each MCP session creates a sysrepo session
- **Error Handling**: Connection failures are handled gracefully with retries

Sysrepo Operations
~~~~~~~~~~~~~~~~~~

The server exposes the following sysrepo operations as MCP tools:

**Configuration Operations:**

- ``sr_get_config``: Read configuration from a YANG module
- ``sr_edit_config``: Apply configuration changes to a YANG module
- ``sr_copy_config``: Copy configuration between datastores

**Operational Data:**

- ``sr_get_operational``: Read operational state data
- ``sr_subscribe_oper_changes``: Subscribe to operational data changes

**Notifications:**

- ``sr_subscribe_notifs``: Subscribe to sysrepo notifications

**Module Management:**

- ``sr_module_install``: Install a YANG module
- ``sr_module_uninstall``: Uninstall a YANG module

**RPC/Actions:**

- ``sr_execute_rpc``: Execute raw NETCONF RPC
- ``sr_action``: Execute YANG action (RPC-style tool)

.. note::

   Not all operations are exposed by default. Use ``EXPOSE_*`` configuration
   options to control which tools are available to agents.

Logging with elog
-----------------

When ``CONFIG_SYSREPO_MCP_SERVER_SYSLOG`` is enabled, sysrepo-mcp-server uses
the `elog <https://github.com/grgbr/elog>`_ library for syslog management.
elog provides a command-line argument parser for log configuration, making it
simple and flexible.

elog Command-Line Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~

elog is invoked with the following command-line options (parsed by elog's
built-in parser):

.. code-block:: bash

   sysrepo-mcp-server --name sysrepo-mcp-server \
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
   sysrepo-mcp-server --log 7

   # Disable syslog, log to file only
   sysrepo-mcp-server --no-syslog

   # Use elog Unix socket instead of syslog
   sysrepo-mcp-server --socket /var/run/elog.sock

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

1. **Connection Pooling**: Enable connection pooling to sysrepod for
   better performance

2. **Session Management**: Configure ``MCP_SESSION_TTL`` and
   ``MCP_MAX_SESSIONS`` appropriately for your deployment

3. **Thread Pooling**: Configure the number of FastCGI workers based on
   expected load

4. **Logging**: Use appropriate log levels to minimize overhead

Monitoring and Observability
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **Log Monitoring**: Monitor elog logs for errors and warnings
2. **Metrics**: Expose metrics via sysrepo operational data (e.g., active
   sessions, request counts)
3. **Health Checks**: Implement health check endpoints (e.g.,
   ``/health``)
4. **Alerting**: Configure alerts for critical events (e.g., connection
   failures, authentication failures)

Future Considerations
---------------------

The following features are planned for future versions:

- **SSE Support**: Add Server-Sent Events support for streaming MCP
  responses
- **WebSocket Support**: Add WebSocket support for bidirectional
  communication
- **gRPC Support**: Add gRPC support for high-performance scenarios
- **Authentication**: Add OAuth2, JWT, and other authentication methods
- **Encryption**: Add end-to-end encryption for MCP messages
- **Clustering**: Support for multiple server instances behind a load
  balancer

Summary
-------

This chapter described the architecture of sysrepo-mcp-server, including its
placement in the system, communication protocols, and configuration models.

Key points:

- sysrepo-mcp-server bridges MCP and sysrepo, exposing NETCONF operations as
  MCP tools
- The server uses FastCGI for communication with lighttpd reverse proxy
- Two authentication modes are supported: Bearer token and cookie-based
- Runtime configuration is managed through sysrepo YANG models
- elog provides enhanced syslog management for production deployments
- Access control integrates with sysrepo NACM for fine-grained permissions