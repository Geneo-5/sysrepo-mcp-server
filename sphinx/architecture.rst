.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Architecture
============

This chapter describes where sysrepo-mcp sits in a system, how it talks to its
peers, and how it is configured.

.. warning::

   Implementation status. The FastCGI transport, the MCP lifecycle, sessions,
   the sysrepo tools, the notification tools, authentication (API keys, NACM,
   module filter, write protection) are implemented.  Only elog remains to be
   integrated.  Do not expose this build to an untrusted agent until the
   deployment config enables authentication and ACL.

Overview
--------

sysrepo-mcp bridges the `Model Context Protocol
<https://modelcontextprotocol.io>`_ (MCP) and `sysrepo
<https://github.com/sysrepo/sysrepo>`_, the YANG datastore behind Netopeer2. It
exposes datastore operations as MCP tools that an AI agent can invoke.

Two design decisions shape everything else:

**FastCGI transport.**
   The server is a FastCGI application. A reverse proxy (lighttpd, nginx)
   terminates HTTP and TLS and forwards requests over FastCGI. The server never
   speaks HTTP itself and never listens on a public port.

**sysrepo as a library.**
   The sysrepo library is linked into the binary and called directly
   (``sr_connect()``, ``sr_session_start()``, ``sr_get_data()``, ...). There is
   no separate daemon: sysrepo has had none since version 2, the
   ``sysrepo-plugind`` service being an unrelated, optional component.

Architecture diagram
--------------------

::

   +-------------+       +---------------+       +---------------+
   |   AI agent  |<----->| Reverse proxy |<----->|  sysrepo-mcp  |
   | (MCP client)|  HTTP | (lighttpd,    | FastCGI  (this project)|
   +-------------+  TLS  |  nginx)       |       +-------+-------+
                         +---------------+               |
                                                         | linked
                                                         v
                                                 +---------------+
                                                 | libsysrepo    |
                                                 | + libyang     |
                                                 +-------+-------+
                                                         |
                                     +-------------------+-------------------+
                                     v                                       v
                            +-----------------+                    +------------------+
                            | Repository      |                    | Shared memory    |
                            | /etc/sysrepo    |                    | /dev/shm         |
                            | (YANG modules,  |                    | (running DS,     |
                            |  startup DS)    |                    |  locks, events)  |
                            +-----------------+                    +------------------+

Components
----------

1. **AI agent**: any MCP-compatible client.

2. **Reverse proxy**: terminates HTTP and TLS, applies rate limiting, and
   forwards the MCP endpoint to the FastCGI socket. It may also spawn the
   server process itself (lighttpd ``bin-path``).

3. **sysrepo-mcp**: accepts FastCGI requests, parses JSON-RPC 2.0, dispatches
   MCP methods to tool handlers, calls the sysrepo API, and serialises the
   result back as JSON-RPC.

4. **libsysrepo and libyang**: the datastore API and the YANG engine. libyang
   parses the schemas, validates the data and serialises trees; sysrepo owns
   the datastores, the locking and the change notifications.

5. **The repository**: sysrepo persists YANG modules and the startup datastore
   under its repository directory (``/etc/sysrepo`` by default) and keeps the
   running datastore, locks and event pipes in POSIX shared memory.

.. note::

   Because the running datastore lives in shared memory owned by sysrepo, every
   process linked against libsysrepo participates in the same locking and
   notification scheme. The server is *not* reading configuration files
   directly, and must not be given a repository directory that another sysrepo
   build does not agree on.

Communication protocol
----------------------

MCP messages are JSON-RPC 2.0. The MCP Streamable HTTP binding sends them to a
single endpoint (``/mcp`` here) with HTTP POST.

Request flow
~~~~~~~~~~~~

1. The agent POSTs a JSON-RPC message to the MCP endpoint.
2. The proxy forwards method, headers and body over FastCGI.
3. sysrepo-mcp authenticates the request, dispatches it, and writes a JSON-RPC
   response on the FastCGI output stream.
4. The proxy relays the HTTP response to the agent.

.. code-block:: http

   POST /mcp HTTP/1.1
   Host: example.com
   Content-Type: application/json
   Accept: application/json, text/event-stream
   Authorization: Bearer <api-key>

   {
       "jsonrpc": "2.0",
       "id": 1,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "xpath": "/oven:oven"
           }
       }
   }

Streaming and SSE
~~~~~~~~~~~~~~~~~

sysrepo-mcp answers with ``Content-Type: application/json`` only. It never
opens a ``text/event-stream`` response and does not implement the
server-to-client streaming half of the Streamable HTTP binding.

.. note::

   This is a deliberate scope restriction, not a protocol impossibility. The
   MCP Streamable HTTP binding explicitly allows a server to answer a POST with
   a single JSON object instead of an SSE stream, so an unmodified client
   works. FastCGI can in principle carry a long-lived streamed response, but
   proxy response buffering, the ``max-procs`` process model and the absence of
   a per-connection event loop make it a poor fit here.

   The practical consequence is that **sysrepo notifications cannot be pushed
   to the agent**. They are not lost, though: the server subscribes on the
   agent's behalf, queues what arrives, and hands it over when the agent calls
   ``sr_notif_poll``. See `Notifications`_ below. Anything requiring genuinely
   server-initiated messages, including MCP sampling and elicitation, is out
   of scope for this transport.

Sessions
~~~~~~~~

*Implemented.* A session is what makes state survive between two requests. It
holds the notification subscriptions an agent has opened and the queue of
events waiting to be collected.

- ``initialize`` creates one and returns it in the ``Mcp-Session-Id``
  response header.
- Later requests echo that header. An identifier that does not resolve is
  answered with HTTP 404, which tells the client to re-initialize.
- ``DELETE`` on the endpoint terminates a session and releases its
  subscriptions.
- A session idle longer than ``SYSREPO_MCP_SERVER_SESSION_TTL`` is dropped,
  and past ``SYSREPO_MCP_SERVER_MAX_SESSIONS`` concurrent sessions
  ``initialize`` is refused with HTTP 503.

The header is optional. A request without one is served normally, so an agent
reading a value once does not have to handshake first; only the notification
tools need a session, and they say so with the ``-32008`` error.

.. warning::

   **max-procs must be 1.** A session, its subscriptions and its queue live in
   the FastCGI process that created them. With several processes, consecutive
   requests from one agent land in different ones and the session is not
   found.

   This is the main open limitation. The fix is to move the session store out
   of the process, most likely under
   ``/sysrepo-mcp:server-state/session`` in the operational datastore, which
   would give sharing and expiry at once. The subscriptions themselves are
   harder: a sysrepo subscription belongs to the process that created it, so
   sharing sessions across workers means one worker receiving events on behalf
   of the others. See :doc:`todo`, milestone 4.

Notifications
~~~~~~~~~~~~~

*Implemented.* Since the transport cannot push, the server pulls on the
agent's behalf:

1. ``sr_notif_subscribe`` calls ``sr_notif_subscribe_tree()`` on the session's
   sysrepo session, with ``SR_SUBSCR_NO_THREAD``.
2. At the start of every request, before anything else, the server calls
   ``sr_subscription_process_events()`` on every live subscription. The
   sysrepo callbacks therefore run on the request thread.
3. Each callback serialises the notification and appends it to the session's
   ring buffer.
4. ``sr_notif_poll`` drains that buffer.

``SR_SUBSCR_NO_THREAD`` is the load-bearing choice. Letting sysrepo call back
on a thread of its own would mean locking the queue and reasoning about a
buffer that changes while a response is being built; draining events between
requests keeps the whole server single-threaded.

The cost is latency and bounded memory: an event is only noticed when the next
request arrives, and a queue that overflows drops its oldest entries. The
dropped count is reported by every poll rather than hidden.

Configuration model
-------------------

Configuration is split in two:

**Build time**, in ``config.in`` (Kconfig)
   Transport, credential type, access control switches, repository path,
   logging, and the session limits: maximum concurrent sessions, idle TTL and
   notification queue size. Fixed when the binary is compiled. See
   :doc:`install`.

**Runtime**, in ``yang/sysrepo-mcp.yang``
   The API key list, and the server operational state. Read from and written to
   the sysrepo datastore like any other YANG data, which means it is itself
   subject to NACM.

.. note::

   The session limits are build-time today because the session store is
   build-time: it is a fixed array in the process. Moving sessions to the
   datastore, which milestone 4 calls for, is what would let an operator
   change them without recompiling.

Authentication
--------------

.. warning::

   Not implemented. The server currently accepts every request.

Bearer token
~~~~~~~~~~~~

The recommended mode. The agent sends ``Authorization: Bearer <api-key>``, and
the proxy forwards it as the ``HTTP_AUTHORIZATION`` FastCGI parameter.

1. The server extracts the key from the header.
2. It looks the key up in ``/sysrepo-mcp:api-key``.
3. It resolves the associated NACM user name.
4. It calls ``sr_nacm_set_user()`` on the sysrepo session, so every subsequent
   operation is evaluated against that user's NACM rules.
5. A missing or unknown key yields HTTP 401.

.. warning::

   Storing API keys as cleartext leaves in a datastore means any principal with
   read access to ``/sysrepo-mcp:api-key`` can impersonate every agent. The
   YANG module therefore marks the list ``nacm:default-deny-all``, and keys
   should be stored hashed rather than in the clear. Neither the hashing nor
   the comparison is implemented yet.

Cookie
~~~~~~

An alternative for browser-based clients: the key travels in a cookie named
after ``SYSREPO_MCP_SERVER_COOKIE_NAME``, and is validated identically. Cookie
credentials are exposed to CSRF; the bearer header is preferred everywhere
else.

NACM
----

sysrepo implements the **NETCONF Access Control Model** (NACM), specified in
:rfc:`8341`, through the ``ietf-netconf-acm`` YANG module.

.. note::

   NACM stands for NETCONF Access Control Model, and its current specification
   is RFC 8341. RFC 6536 defined the earlier revision and has been obsoleted.

Enforcement is delegated to sysrepo rather than reimplemented:

- ``sr_nacm_init()`` once, at startup, on a dedicated session.
- ``sr_nacm_set_user()`` per request, which switches the session into NACM
  enforcement for that user.
- ``sr_nacm_check_operation()`` before running an RPC or action.
- ``sr_nacm_destroy()`` at shutdown.

Access control flow::

   FastCGI request
        |
        v  extract credential (Bearer or cookie)
   API key lookup in libconfig ``api_keys[]``
        |
        v  map key -> NACM user name
   sr_nacm_set_user(session, user)
        |
        v  sysrepo evaluates every read/write/exec against ietf-netconf-acm
   JSON-RPC response, or NACM access-denied error

The module allow-list and the write protection declared in ``config.in`` are a
coarse second layer applied before sysrepo is called at all; they are not a
replacement for NACM rules.

.. warning::

When deployed with API keys configured (``auth.api_keys[]`` in the libconfig
file), the server enforces authentication at the ``initialize`` step and
evaluates ACL filters before executing any operation.  Without API keys,
every request is served with the rights of the system user running the
FastCGI process.  Do not expose this build to an untrusted agent without
enabling authentication.

FastCGI transport
-----------------

The server runs as a FastCGI responder. Two deployment shapes are supported:

**Proxy-spawned** (lighttpd ``bin-path``)
   The proxy starts and supervises the process and hands the listening socket
   over on descriptor 0. The socket options in ``config.in`` are unused.
   ``max-procs`` must be 1.

**Externally started**
   The server creates its own listening socket, Unix or TCP, from the
   ``config.in`` options, and the proxy connects to it. This is the shape to
   use under an init system or in a container.

lighttpd
~~~~~~~~

.. code-block:: none

   server.modules += ( "mod_fastcgi" )

   fastcgi.server = (
       "/mcp" => (
           "sysrepo-mcp" => (
               "socket"      => "/var/run/sysrepo-mcp.sock",
               "bin-path"    => "/usr/local/bin/sysrepo-mcp",
               "check-local" => "disable",
               "max-procs"   => 1
           )
       )
   )

nginx
~~~~~

nginx never spawns the application, so the server must already be listening:

.. code-block:: none

   location /mcp {
       include            fastcgi_params;
       fastcgi_pass       unix:/var/run/sysrepo-mcp.sock;
       fastcgi_param      SCRIPT_NAME /mcp;
       fastcgi_buffering  off;
   }

.. note::

   The FastCGI socket must be reachable and writable by the proxy, and by
   nobody else. It is the only authentication boundary below the API key.

sysrepo usage
-------------

Connection and session
~~~~~~~~~~~~~~~~~~~~~~

- One ``sr_conn_ctx_t`` per process, opened with ``sr_connect()`` at startup
  and closed with ``sr_disconnect()``.
- One ``sr_session_ctx_t`` per request, opened with ``sr_session_start()`` and
  moved between datastores with ``sr_session_switch_ds()``.
- The datastore of a session is *not* implicit: a session started on
  ``SR_DS_RUNNING`` keeps answering from the running datastore until it is
  switched, which is the usual cause of an empty operational read.

Data representation
~~~~~~~~~~~~~~~~~~~

Data crosses the API as libyang trees (``struct lyd_node``), obtained with
``sr_get_data()`` and serialised with ``lyd_print_mem(..., LYD_JSON, ...)``.

.. note::

   The older ``sr_val_t`` array API (``sr_get_items()``, ``sr_set_item()``) is
   documented upstream as deprecated in favour of ``lyd_node``. It also cannot
   represent a tree, so rebuilding a JSON document from an ``sr_val_t`` array
   means re-deriving the hierarchy by hand. New code should use
   ``sr_get_data()`` and ``sr_edit_batch()``.

Edits
~~~~~

An edit is parsed into a libyang tree, staged, then committed:

1. ``lyd_parse_data_mem()`` with ``LYD_JSON`` and ``LYD_PARSE_ONLY`` on the
   ``config`` argument, against the context returned by
   ``sr_session_acquire_context()``.
2. ``sr_edit_batch()`` to stage the tree with a default operation
   (``merge``, ``replace`` or ``none``).
3. ``sr_apply_changes()`` to validate and commit; ``sr_discard_changes()`` on
   failure.

.. note::

   ``sr_set_item_str()`` sets a **single node** from its string value. It
   cannot be given a serialised JSON document, which is what a ``config``
   argument holds.

Tools
-----

The tool surface is specified in :doc:`api`. Summarised by area:

.. list-table::
   :header-rows: 1
   :widths: 26 20 54

   * - Tool
     - Status
     - sysrepo entry point
   * - ``sr_get_config``
     - implemented
     - ``sr_get_data()`` on running/startup/candidate
   * - ``sr_edit_config``
     - implemented
     - ``sr_edit_batch()`` + ``sr_apply_changes()``
   * - ``sr_delete_config``
     - implemented
     - ``sr_delete_item()`` + ``sr_apply_changes()``
   * - ``sr_get_operational``
     - implemented
     - ``sr_session_switch_ds()`` + ``sr_get_data()``
   * - ``sr_execute_rpc``, ``sr_action``
     - implemented
     - ``sr_rpc_send_tree()``
   * - ``sr_notif_subscribe``
     - implemented
     - ``sr_notif_subscribe_tree()``, ``SR_SUBSCR_NO_THREAD``
   * - ``sr_notif_unsubscribe``
     - implemented
     - ``sr_unsubscribe()``
   * - ``sr_notif_poll``, ``sr_notif_list_subscriptions``
     - implemented
     - none, server-local queue
   * - ``sr_notif_send``
     - implemented
     - ``sr_notif_send_tree()``
   * - ``sr_list_modules``
     - implemented
     - ``ly_ctx_get_module_iter()``
   * - ``sr_module_install``
     - implemented
     - ``sr_install_module()``
   * - ``sr_module_uninstall``
     - implemented
     - ``sr_remove_module()``
   * - ``get_status``
     - implemented
     - none, server-local counters
   * - ``get_tree``
     - implemented
     - ``lys_find_path()`` on the session context
   * - ``get_help``
     - partial
     - ``lysc_node`` introspection; no ranges or defaults yet

"Partial" means the handler works but does not produce every member the API
reference documents; what is missing is stated there, tool by tool.

Logging
-------

.. warning::

   Not implemented. The current code writes to ``stderr`` with ``fprintf()``.

`elog <https://github.com/grgbr/elog>`_ is to provide syslog, file and console
back ends together with a command-line parser for the log configuration. Under
FastCGI, syslog is the only reliable sink: ``stdout`` belongs to the response
body, and ``stderr`` is captured by the proxy.

Two log streams have to stay separate: the server's own log, and the sysrepo
library log, which is redirected with ``sr_log_set_cb()``.

Deployment
----------

Security
~~~~~~~~

1. Terminate TLS at the proxy and publish the endpoint over HTTPS only.
2. Keep the FastCGI socket private to the proxy user.
3. Run the server as a dedicated unprivileged user, member of the sysrepo
   group, never as root, which is also the NACM recovery user.
4. Turn access control on, and give each agent its own key and NACM user.
5. Restrict the module allow-list to what the agent genuinely needs.
6. Rotate keys, and log every configuration change with the key identity that
   caused it.

Performance
~~~~~~~~~~~

1. **Concurrency is capped at one process.** ``max-procs`` must stay at 1
   while sessions are process-local, so requests are serialised. This is the
   binding constraint on throughput today, and the reason milestone 4 matters
   beyond correctness.
2. A sysrepo connection is expensive to open and cheap to reuse; one is kept
   per process for its lifetime.
3. Reading a whole module is expensive. Prefer a precise XPath over fetching a
   subtree and filtering afterwards.
4. Operational reads may invoke subscriber callbacks in other processes and
   are bounded by their timeouts.
5. Notification callbacks run on the request thread, so a wide subscription
   makes every request do the work of draining it. Filter at subscription
   time rather than after polling.

Observability
~~~~~~~~~~~~~

1. ``get_status`` for a liveness probe.
2. ``sr_get_operational`` on ``/sysrepo-mcp:server-state`` for detail.
3. The proxy access log for latency and status codes.
4. syslog for the server and sysrepo library messages.

Out of scope
------------

Deliberately not planned:

- **SSE and server-initiated messages**: see `Streaming and SSE`_ above. This
  rules out pushed notifications, MCP sampling and elicitation.
- **WebSocket transport**: not an MCP binding, and redundant with FastCGI.
- **A direct HTTP listener**: HTTP, TLS and rate limiting stay in the proxy.

Possible later:

- OAuth2 or JWT credentials, which the MCP authorization specification builds
  on.
- Horizontal scaling behind a load balancer, once sessions are shared.
- Metrics export.

Summary
-------

- sysrepo-mcp exposes sysrepo datastore operations as MCP tools.
- The transport is FastCGI behind a reverse proxy; responses are plain JSON,
  with no SSE and therefore no pushed notification. Notifications are queued
  per session and collected by polling instead.
- Sessions are identified by ``Mcp-Session-Id`` and live in the FastCGI
  process, which forces ``max-procs = 1``.
- sysrepo is linked as a library; there is no daemon, and the running datastore
  lives in shared memory.
- Build-time configuration is Kconfig; runtime configuration is YANG.
- Access control is delegated to sysrepo NACM, keyed by the configured API
  keys, and is fully enforced; only elog remains to be integrated.
