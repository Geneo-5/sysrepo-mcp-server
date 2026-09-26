.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

API Reference
=============

This chapter specifies the MCP interface of sysrepo-mcp: the protocol methods
it answers, the tools it exposes, and the errors it returns.

.. note::

   Each tool below carries a status marker:

   **implemented**
      Present, covered by the test suite.

   **partial**
      Present, but some documented members are not produced yet. What is
      missing is stated in place.

   **planned**
      Specified here, not written yet.

   The server optionally supports authentication: when API keys are
   configured, every request is bound to the user from the matching key,
   and ACL filters may restrict operations for that user.  Without API
   keys, every request is served with the rights of the system user
   running the server.  That is a property of the whole API, not of one
   tool.

Protocol
--------

All traffic is JSON-RPC 2.0 over HTTP POST, forwarded to the server over
FastCGI. Requests carry ``Content-Type: application/json``; responses are
``application/json``. Encoding is UTF-8.

Every exchange uses the endpoint configured in the reverse proxy, ``/mcp`` by
convention.

Lifecycle
~~~~~~~~~

*Status: implemented for two protocol eras.* Modern clients use the stateless
``2026-07-28`` request format and can start with ``server/discover``. Legacy
clients use the ``2025-11-25`` handshake described below.

Modern request metadata
^^^^^^^^^^^^^^^^^^^^^^^

Every modern POST carries the protocol version in both the HTTP header and
the request's ``params._meta`` object, along with client identity and
capabilities. The server requires ``Mcp-Method`` to match the JSON-RPC method.
For ``tools/call``, ``Mcp-Name`` must match ``params.name``. For example::

   POST /mcp
   Content-Type: application/json
   MCP-Protocol-Version: 2026-07-28
   Mcp-Method: server/discover

   {"jsonrpc":"2.0","id":1,"method":"server/discover","params":{
     "_meta":{
       "io.modelcontextprotocol/protocolVersion":"2026-07-28",
       "io.modelcontextprotocol/clientInfo":{"name":"client","version":"1"},
       "io.modelcontextprotocol/clientCapabilities":{}
     }
   }}

Modern requests are independent; the server does not return
``Mcp-Session-Id``. The session-bound notification subscription tools are
available only through the legacy handshake. Requests with an ``Origin``
header receive HTTP 403 by default. Modern ``server/discover`` and
``tools/list`` results include ``ttlMs: 0`` and ``cacheScope: "private"``:
clients must treat each response as immediately stale and must not reuse it
across authorization contexts.

``initialize``
^^^^^^^^^^^^^^

Negotiates the protocol revision, announces capabilities, and **opens a
session**.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 1,
       "method": "initialize",
       "params": {
           "protocolVersion": "2025-11-25",
           "capabilities": {},
           "clientInfo": {
               "name": "example-agent",
               "version": "1.0.0"
           }
       }
   }

The response announces the tool capability, and only that one: sysrepo-mcp
exposes no resources, no prompts and no server-initiated messages.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 1,
       "result": {
           "protocolVersion": "2025-11-25",
           "capabilities": {
               "tools": {
                   "listChanged": false
               }
           },
           "serverInfo": {
               "name": "sysrepo-mcp",
               "version": "0.1.0"
           }
       }
   }

The session identifier is returned in the ``Mcp-Session-Id`` **HTTP header**,
not in the JSON body::

   HTTP/1.1 200 OK
   Content-Type: application/json
   Mcp-Session-Id: 3f2a1c9e8b7d6540a1b2c3d4e5f60718

The server also supports stateless MCP revision ``2026-07-28``. Modern
clients send ``MCP-Protocol-Version`` and ``Mcp-Method`` headers and include
``_meta.io.modelcontextprotocol/protocolVersion``, client identity and
capabilities on every request. They can call ``server/discover`` to inspect
the supported versions and capabilities. Modern requests are independent and
do not receive an ``Mcp-Session-Id``. Legacy clients continue to use the
``initialize`` handshake described above. For security, requests that include
an ``Origin`` header are rejected with HTTP 403; browser-origin clients are
not supported by the default configuration.

.. note::

   ``initialize`` consumes a session slot whether or not the client goes on to
   use it. A client that only wants to read something once can skip the
   handshake entirely; see `Sessions`_ below.

``notifications/initialized``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

*Status: implemented.* Sent by the client once the handshake is complete. It
is a JSON-RPC notification: it has no ``id`` and **must not** be answered with
a JSON-RPC response. The server acknowledges it with HTTP 202 and an empty
body.

``ping``
^^^^^^^^

*Status: implemented.* Returns an empty result. A liveness check that touches
neither sysrepo nor any session.

Sessions
~~~~~~~~

*Status: implemented.* A session is what makes state survive between two
requests. It holds the notification subscriptions an agent has opened and the
queue of events waiting to be collected; nothing else in this API is
stateful.

Using one
^^^^^^^^^

1. ``initialize`` returns ``Mcp-Session-Id``.
2. Every later request repeats that header.
3. ``DELETE`` on the endpoint, with the header, terminates the session and
   releases its subscriptions. The response is HTTP 204 with no body.

.. code-block:: http

   POST /mcp HTTP/1.1
   Content-Type: application/json
   Mcp-Session-Id: 3f2a1c9e8b7d6540a1b2c3d4e5f60718

   {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
    "params": {"name": "sr_notif_poll", "arguments": {}}}

When a session is required
^^^^^^^^^^^^^^^^^^^^^^^^^^

The header is **optional**. A request without one is served normally, which
lets an agent read a value without handshaking first. Only the four tools that
keep state between requests need a session:
``sr_notif_subscribe``, ``sr_notif_unsubscribe``,
``sr_notif_list_subscriptions`` and ``sr_notif_poll``. Called without one they
return ``-32008``, whose message says to call ``initialize``.

Losing one
^^^^^^^^^^

An identifier that does not resolve is answered with **HTTP 404**, which is
what tells a client to re-initialize rather than retry. A session is lost
when:

- the client deleted it;
- it was idle longer than ``SYSREPO_MCP_SERVER_SESSION_TTL``;
- the server restarted.

Past ``SYSREPO_MCP_SERVER_MAX_SESSIONS`` concurrent sessions, ``initialize``
is refused with HTTP 503 and a ``-32000`` error.

.. warning::

   Sessions live in the FastCGI process that created them. The deployment
   **must** use ``max-procs = 1``: with more, consecutive requests from one
   agent land in different processes and the session is not found. This is the
   main open limitation of the current implementation; see :doc:`todo`,
   milestone 4.

``tools/list``
^^^^^^^^^^^^^^

Returns the tool catalogue. Each entry carries a name, a description and a
JSON Schema for its arguments, which is how an agent learns to call the tools
documented below.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 2,
       "result": {
           "tools": [
               {
                   "name": "sr_get_config",
                   "description": "Read configuration data from a sysrepo datastore.",
                   "inputSchema": {
                       "type": "object",
                       "properties": {
                           "xpath": {
                               "type": "string",
                               "description": "XPath selecting the data, with module prefixes."
                           },
                           "datastore": {
                               "type": "string",
                               "enum": ["running", "startup", "candidate"],
                               "default": "running"
                           },
                           "max_depth": {
                               "type": "integer",
                               "minimum": 0,
                               "default": 0
                           }
                       },
                       "required": ["xpath"]
                   }
               }
           ]
       }
   }

``tools/call``
^^^^^^^^^^^^^^

*Status: implemented.* Invokes one tool.

The result is wrapped in the MCP envelope: ``result.content`` is an array of
content blocks, ``result.structuredContent`` carries the same payload as JSON,
and ``result.isError`` reports a tool-level failure, as opposed to a
protocol-level JSON-RPC ``error``. The examples below show the payload, which
is what ``structuredContent`` holds.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 3,
       "result": {
           "content": [
               {"type": "text", "text": "{ \"version\": \"0.1.0\" }"}
           ],
           "structuredContent": {"version": "0.1.0"},
           "isError": false
       }
   }

Conventions
~~~~~~~~~~~

XPath arguments
   Every path is an absolute XPath with module prefixes, as sysrepo and
   libyang expect: ``/oven:oven/temperature``, not ``/oven/temperature``. The
   prefix is the *module name*, not the module's YANG ``prefix`` statement.

Data encoding
   Configuration and state data are libyang JSON trees (``LYD_JSON``): object
   keys of top-level nodes are prefixed with their module name, lists are JSON
   arrays, and a value keeps its YANG type.

Examples
   The examples below mostly use ``oven``, the example module shipped with
   sysrepo in ``extern/sysrepo/examples/plugin/oven.yang``. This project no
   longer installs a YANG module of its own — API keys and other runtime
   settings live in a libconfig file instead (see :doc:`todo`, P1).

Tool catalogue
--------------

.. list-table::
   :header-rows: 1
   :widths: 30 14 56

   * - Tool
     - Status
     - Purpose
   * - ``sr_get_config``
     - implemented
     - Read configuration data
   * - ``sr_edit_config``
     - implemented
     - Create or modify configuration data
   * - ``sr_delete_config``
     - implemented
     - Delete configuration data
   * - ``sr_copy_config``
     - implemented
     - Replace one datastore with another
   * - ``sr_diff_config``
     - planned
     - Compare two datastores over a subtree
   * - ``sr_get_operational``
     - implemented
     - Read operational state
   * - ``sr_execute_rpc``
     - implemented
     - Invoke a YANG RPC
   * - ``sr_action``
     - implemented
     - Invoke a YANG action
   * - ``sr_notif_subscribe``
     - implemented
     - Watch a module's notifications (needs a session)
   * - ``sr_notif_unsubscribe``
     - implemented
     - Stop watching (needs a session)
   * - ``sr_notif_list_subscriptions``
     - implemented
     - List what the session watches (needs a session)
   * - ``sr_notif_poll``
     - implemented
     - Collect what arrived since the last poll (needs a session)
   * - ``sr_notif_send``
     - implemented
     - Emit a YANG notification
   * - ``sr_list_modules``
     - implemented
     - List installed modules
   * - ``sr_module_install``
     - implemented
     - Install a YANG module
   * - ``sr_module_uninstall``
     - implemented
     - Remove a YANG module
   * - ``get_status``
     - implemented
     - Server health and session counters
   * - ``get_schema``
     - implemented
     - Explore compiled YANG schema, including constraints and defaults

Configuration tools
-------------------

sr_get_config
~~~~~~~~~~~~~

*Status: implemented.* Reads configuration data from a conventional datastore.

**Arguments**

``xpath`` (string, required)
   XPath selecting the data to read.

``datastore`` (string, optional)
   ``running``, ``startup`` or ``candidate``. Default ``running``.

``max_depth`` (integer, optional)
   Maximum subtree depth, 0 meaning unlimited. Default 0.

``options`` (array of strings, optional)
   Select how default values are printed. ``["trim-defaults"]`` trims
   default-valued nodes; ``["all-defaults"]`` prints all default values
   represented in the returned tree. Omitting the parameter preserves
   libyang's existing output. Pass at most one option; an empty array is
   equivalent to omitting it.

.. note::

   ``max_depth`` replaces the ``depth`` argument of earlier drafts, whose
   ``shallow`` / ``deep`` / ``children`` values had no counterpart in the
   sysrepo API. ``sr_get_data()`` takes an integer depth.

**Result**

``data`` (object)
   The matched data, as a libyang JSON tree. An empty object when the XPath
   matches nothing, which is not an error.

``xpath`` (string)
   The XPath that was evaluated.

``datastore`` (string)
   The datastore that was read.

.. note::

   API keys are **not** datastore data. They live in the server's own
   libconfig file (``auth.api_keys[]``), read once at startup — the
   ``yang/sysrepo-mcp.yang`` module that used to expose
   ``/sysrepo-mcp:api-key`` was removed (see :doc:`todo`, P1). There is
   nothing to read or write about API keys through ``sr_get_config`` or
   ``sr_edit_config``.

The oven configuration container:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 12,
       "method": "tools/call",
       "params": {
           "name": "sr_get_config",
           "arguments": {
               "xpath": "/oven:oven"
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 12,
       "result": {
           "data": {
               "oven:oven": {
                   "turned-on": false,
                   "temperature": 0
               }
           },
           "xpath": "/oven:oven",
           "datastore": "running"
       }
   }

sr_edit_config
~~~~~~~~~~~~~~

*Status: implemented.* Stages an edit and commits it.

**Arguments**

``config`` (object, required)
   The data to apply, as a libyang JSON tree, rooted at a module-prefixed
   node.

``datastore`` (string, optional)
   ``running``, ``startup`` or ``candidate``. Default ``running``.

``operation`` (string, optional)
   Default operation applied to the tree: ``merge``, ``replace`` or ``none``.
   Default ``merge``.

.. note::

   Earlier drafts required both a ``config`` tree and an ``xpath``, and named
   the datastore ``target``. The XPath was redundant: a libyang JSON tree is
   already absolutely rooted, so the edit target is implied by ``config``
   itself. ``target`` is renamed ``datastore`` for consistency with the read
   tools.

**Result**

``ok`` (boolean)
   True when the change was validated and committed.

``operation`` (string)
   Operation that was applied: ``merge``, ``replace`` or ``none``.

``edit_nodes`` (integer)
   Number of explicit schema nodes in the submitted config tree. The count
   includes containers and list nodes as well as their leaves. It confirms
   the size of the edit accepted by sysrepo; it is not a datastore diff, so
   a merge that writes existing values still counts them, and a replace may
   remove nodes that are absent from the submitted tree without counting
   those removals.

Turning the oven on at 200 degrees:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 20,
       "method": "tools/call",
       "params": {
           "name": "sr_edit_config",
           "arguments": {
               "datastore": "running",
               "operation": "merge",
               "config": {
                   "oven:oven": {
                       "temperature": 200,
                       "turned-on": true
                   }
               }
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 20,
       "result": {
           "ok": true,
           "operation": "merge",
           "edit_nodes": 3
       }
   }

Changing only the temperature, leaving ``turned-on`` untouched:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 21,
       "method": "tools/call",
       "params": {
           "name": "sr_edit_config",
           "arguments": {
               "config": {
                   "oven:oven": {
                       "temperature": 150
                   }
               }
           }
       }
   }

sr_delete_config
~~~~~~~~~~~~~~~~

*Status: implemented.* Removes the data selected by an XPath.

This is the only tool that can delete. ``sr_edit_config`` can create and
modify, but a ``merge`` cannot express a deletion and a ``replace`` cannot
express "nothing", so without this tool an agent can fill a datastore and
never empty it.

**Arguments**

``xpath`` (string, required)
   XPath selecting what to remove. A list predicate removes one entry; the
   list path removes them all.

``datastore`` (string, optional)
   ``running``, ``startup`` or ``candidate``. Default ``running``.

``strict`` (boolean, optional)
   Fail when the node does not exist. Default false, so a cleanup does not
   have to check first.

**Result**

``ok`` (boolean), ``xpath`` (string).

Removing one oven configuration leaf:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 25,
       "method": "tools/call",
       "params": {
           "name": "sr_delete_config",
           "arguments": {
               "xpath": "/oven:oven/temperature"
           }
       }
   }

.. warning::

   The XPath is evaluated, not matched literally: ``/oven:oven`` removes the
   whole container, and a path naming a list with no predicate removes every
   entry. There is no confirmation step and no undo.

sr_copy_config
~~~~~~~~~~~~~~

*Status: implemented.* Replaces an entire conventional datastore with the
contents of another using sysrepo's native ``sr_copy_config()`` operation.

**Arguments**

``source`` (string, required)
   Source datastore: ``running``, ``startup`` or ``candidate``.

``destination`` (string, required)
   Destination datastore: ``running``, ``startup`` or ``candidate``. Must be
   different from ``source``.

**Result**

``ok`` (boolean), ``source`` (string), ``destination`` (string).

The operation requires write access to the destination, and sysrepo applies
its normal validation and change callbacks. Copying between ``candidate``
and ``running`` resets candidate to its normal mirroring behavior. The tool
copies the complete datastore; it has no module or XPath filter.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 26,
       "method": "tools/call",
       "params": {
           "name": "sr_copy_config",
           "arguments": {"source": "startup", "destination": "running"}
       }
   }

sr_diff_config
~~~~~~~~~~~~~~

*Status: planned.* Compares two datastores over a subtree and reports what
differs, by reading both sides with ``sr_get_data()`` and handing the two
trees to libyang's ``lyd_diff_siblings()``.

This is not a sysrepo concept: sysrepo has no dry-run operation. The closest
thing today is writing to ``candidate``, validating it, then comparing it
against ``running`` by hand — exactly what this tool is meant to automate.
See :doc:`todo`, P5, for the planning note.

**Arguments**

``source`` (string, required)
   ``running``, ``startup`` or ``candidate``.

``target`` (string, required)
   ``running``, ``startup`` or ``candidate``, different from ``source``.

``xpath`` (string, optional)
   Subtree to compare. Default: the datastore root, i.e. every module.

``max_depth`` (integer, optional)
   Maximum subtree depth read from each side before diffing, 0 meaning
   unlimited. Default 0.

**Result**

``diff`` (array of objects)
   One entry per changed node, each with:

   ``xpath`` (string)
      Full path of the node, exactly as ``get_schema``
      reports it.

   ``operation`` (string)
      ``created``, ``deleted``, ``replaced`` or ``moved`` — libyang's
      ``LYD_DIFF_OP_*`` by name.

   ``value``
      The node's new value or subtree for ``created``/``replaced``, as a
      libyang JSON value or object. Absent for ``deleted``.

   ``previous_value``
      The value being replaced or deleted, for a leaf or leaf-list entry.
      Absent for ``created``.

   ``previous_position`` (string, optional)
      For a ``moved`` entry in a user-ordered list or leaf-list: the
      preceding sibling's key predicate or value, taken from libyang's
      ``yang:key``/``yang:value`` diff metadata. Absent for a system-ordered
      list, since sysrepo reports no position for those.

``source`` (string), ``target`` (string), ``xpath`` (string)
   Echo of the request.

``changed`` (integer)
   Number of entries in ``diff``.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 95,
       "method": "tools/call",
       "params": {
           "name": "sr_diff_config",
           "arguments": {
               "source": "running",
               "target": "candidate",
               "xpath": "/oven:oven"
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 95,
       "result": {
           "diff": [
               {
                   "xpath": "/oven:oven/temperature",
                   "operation": "replaced",
                   "value": 220,
                   "previous_value": 200
               }
           ],
           "source": "running",
           "target": "candidate",
           "xpath": "/oven:oven",
           "changed": 1
       }
   }

.. note::

   Each side is read independently with ``sr_get_data()`` before diffing:
   nothing here is transactional, so a concurrent write to either datastore
   between the two reads can produce a diff that never existed as one
   consistent snapshot. Acceptable for an agent comparing ``running``
   against a ``candidate`` it just finished editing itself; not a substitute
   for a real transaction (see :doc:`todo`, P4).

Operational data
----------------

sr_get_operational
~~~~~~~~~~~~~~~~~~

*Status: implemented.* Reads the operational datastore, which merges
configuration with state data supplied by subscribers.

**Arguments**

``xpath`` (string, required)
   XPath selecting the data to read.

``max_depth`` (integer, optional)
   Maximum subtree depth, 0 meaning unlimited. Default 0.

``timeout_ms`` (integer, optional)
   How long to wait for operational subscribers. Default 5000.

.. note::

   There is no ``datastore`` argument: this tool always reads
   ``SR_DS_OPERATIONAL``. Use ``sr_get_config`` for the others.

**Result**

``data`` (object)
   The matched data, as a libyang JSON tree.

The oven state:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 30,
       "method": "tools/call",
       "params": {
           "name": "sr_get_operational",
           "arguments": {
               "xpath": "/oven:oven-state"
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 30,
       "result": {
           "data": {
               "oven:oven-state": {
                   "temperature": 180,
                   "food-inside": true
               }
           }
       }
   }

The server's own state:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 31,
       "method": "tools/call",
       "params": {
           "name": "sr_get_operational",
           "arguments": {
               "xpath": "/sysrepo-mcp:server-state"
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 31,
       "result": {
           "data": {
               "sysrepo-mcp:server-state": {
                   "version": "0.1.0",
                   "uptime-seconds": 3600,
                   "active-sessions": 0,
                   "max-sessions": 64
               }
           }
       }
   }

.. note::

   Reading operational data returns a snapshot. sysrepo can push change
   notifications, but this transport cannot forward them, so an agent that
   needs to follow a value has to poll. See the streaming section of
   :doc:`architecture`.

RPCs and actions
----------------

sr_execute_rpc
~~~~~~~~~~~~~~

*Status: implemented.* Invokes a YANG RPC, that is, a top-level operation.

**Arguments**

``xpath`` (string, required)
   Path of the RPC, ``/<module>:<rpc-name>``.

``input`` (object, optional)
   Input parameters, as a libyang JSON tree relative to the RPC node.

``timeout_ms`` (integer, optional)
   Default 5000.

**Result**

``output`` (object)
   The RPC output tree, empty when the RPC defines no output.

Putting food in the oven, using the ``insert-food`` RPC of ``oven.yang``:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 40,
       "method": "tools/call",
       "params": {
           "name": "sr_execute_rpc",
           "arguments": {
               "xpath": "/oven:insert-food",
               "input": {
                   "time": "on-oven-ready"
               }
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 40,
       "result": {
           "output": {}
       }
   }

.. note::

   Only RPCs defined by *installed YANG modules* can be invoked. The standard
   NETCONF operations of ``ietf-netconf``, such as ``get-config``, are handled
   inside a NETCONF server like Netopeer2 and are not routed through sysrepo's
   RPC machinery; use ``sr_get_config`` and ``sr_edit_config`` instead.

sr_action
~~~~~~~~~

*Status: implemented.* Invokes a YANG 1.1 action, that is, an operation
defined inside a data node and therefore bound to one instance of it.

**Arguments**

``xpath`` (string, required)
   Full data path of the action, including the list keys of its parent, for
   example ``/example:interfaces/interface[name='eth0']/reset``.

``input`` (object, optional)
   Input parameters, as a libyang JSON tree relative to the action node.

``timeout_ms`` (integer, optional)
   Default 5000.

**Result**

``output`` (object)
   The action output tree.

.. note::

   An action differs from an RPC only in being anchored to a data node, which
   is why ``xpath`` must identify a concrete instance. Both are sent with
   ``sr_rpc_send_tree()``, which is why the two handlers share their
   implementation.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 41,
       "method": "tools/call",
       "params": {
           "name": "sr_action",
           "arguments": {
               "xpath": "/example:interfaces/interface[name='eth0']/reset",
               "input": {
                   "reset-type": "hard"
               }
           }
       }
   }

Notifications
-------------

YANG notifications are the one part of this API that is not request and
response. The transport cannot push anything to the agent, so the server
subscribes on its behalf, queues what arrives, and hands it over when asked.

.. note::

   This is why sessions exist. A subscription and its queue belong to one
   agent and must outlive a single request, and the session identifier is what
   ties the two together. All four tools below except ``sr_notif_send``
   require one.

The flow an agent follows:

1. ``initialize``, and keep the ``Mcp-Session-Id``.
2. ``sr_notif_subscribe`` on the modules it cares about.
3. Do its work, calling ``sr_notif_poll`` whenever it wants to know what
   happened.
4. ``sr_notif_unsubscribe``, or ``DELETE`` the session, which does the same.

.. warning::

   Events are queued, not pushed, and the queue is bounded by
   ``SYSREPO_MCP_SERVER_NOTIF_QUEUE_SIZE``. When it overflows the oldest
   entries are dropped and ``sr_notif_poll`` reports how many. An agent that
   must not miss an event has to poll often enough, or narrow its filter.

sr_notif_subscribe
~~~~~~~~~~~~~~~~~~

*Status: implemented.* Starts watching a module's notifications on the current
session. Needs a session.

**Arguments**

``module`` (string, required)
   Module whose notifications to watch.

``xpath`` (string, optional)
   Filter, as an absolute path with a module prefix. Without it, every
   notification of the module is queued.

``replay_start`` (integer, optional)
   Unix timestamp to replay history from. Requires replay support to be
   enabled for the module in sysrepo; without it the subscription starts at
   the present.

**Result**

``subscription_id`` (integer), ``module`` (string), ``xpath`` (string, when
given), ``session_id`` (string).

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 90,
       "method": "tools/call",
       "params": {
           "name": "sr_notif_subscribe",
           "arguments": {
               "module": "oven",
               "xpath": "/oven:oven-ready"
           }
       }
   }

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 90,
       "result": {
           "subscription_id": 1,
           "module": "oven",
           "xpath": "/oven:oven-ready",
           "session_id": "3f2a1c9e8b7d6540a1b2c3d4e5f60718"
       }
   }

sr_notif_poll
~~~~~~~~~~~~~

*Status: implemented.* Returns everything received since the last call, and
removes it from the queue. Needs a session.

**Arguments**

``max`` (integer, optional)
   Largest batch to return. 0, the default, means everything pending.

``peek`` (boolean, optional)
   Return the batch without removing it. Default false.

**Result**

``notifications`` (array of objects)
   Each with ``xpath``, ``kind``, ``timestamp`` and ``data``, the last being
   the notification as a libyang JSON tree.

``returned`` (integer)
   Size of this batch.

``pending`` (integer)
   Still queued afterwards. Non-zero after a capped poll.

``total_received`` (integer)
   Everything this session has ever queued.

``dropped`` (integer)
   Events lost to queue overflow. Non-zero means the agent is polling too
   slowly or watching too much.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 91,
       "result": {
           "notifications": [
               {
                   "xpath": "/oven:oven-ready",
                   "kind": "realtime",
                   "timestamp": 1789000000,
                   "data": {"oven:oven-ready": {}}
               }
           ],
           "returned": 1,
           "pending": 0,
           "total_received": 1,
           "dropped": 0
       }
   }

The ``kind`` member distinguishes a live event from the replay machinery:

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Kind
     - Meaning
   * - ``realtime``
     - A notification that has just been sent.
   * - ``replay``
     - A historical notification, replayed because ``replay_start`` was
       given.
   * - ``replay-complete``
     - The replay is over; everything after it is live. Carries no data.
   * - ``terminated``
     - The subscription ended on the sysrepo side.
   * - ``modified``, ``suspended``, ``resumed``
     - Subscription lifecycle changes reported by sysrepo.

sr_notif_list_subscriptions
~~~~~~~~~~~~~~~~~~~~~~~~~~~

*Status: implemented.* Lists what the current session is watching. Needs a
session.

**Result**

``subscriptions`` (array of objects)
   Each with ``subscription_id``, ``module``, ``xpath`` when one was given,
   and ``received``, the number of notifications that subscription has
   queued.

``pending`` (integer)
   Notifications waiting to be polled.

``session_id`` (string)

sr_notif_unsubscribe
~~~~~~~~~~~~~~~~~~~~

*Status: implemented.* Stops watching. Needs a session.

**Arguments**

``subscription_id`` (integer, optional)
   Which subscription to drop. Omitted, every subscription of the session is
   dropped.

**Result**

``removed`` (integer)
   How many subscriptions were dropped.

Already-queued notifications are **not** discarded: a poll after
unsubscribing still returns what arrived before.

sr_notif_send
~~~~~~~~~~~~~

*Status: implemented.* Emits a YANG notification. No session needed.

**Arguments**

``xpath`` (string, required)
   Path of the notification, ``/<module>:<notification-name>``.

``input`` (object, optional)
   Notification content, as a libyang JSON tree relative to the notification
   node.

**Result**

``ok`` (boolean), ``xpath`` (string).

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 92,
       "method": "tools/call",
       "params": {
           "name": "sr_notif_send",
           "arguments": {
               "xpath": "/oven:oven-ready"
           }
       }
   }

.. note::

   The call returns as soon as sysrepo has published the event; it does not
   wait for subscribers to process it. A subscription held by this same server
   is only serviced between requests, so waiting would mean waiting on
   itself. In practice an agent that sends and then polls will occasionally
   need a second poll.

Module management
-----------------

.. warning::

   The three tools below install, remove and enumerate YANG modules. The first
   two change the schema of the whole datastore, for every process linked
   against sysrepo, and removing a module destroys its data. They should be
   denied to agents unless there is a specific reason not to.

sr_module_install
~~~~~~~~~~~~~~~~~

*Status: implemented.* Wraps ``sr_install_module()``.

**Arguments**

``yang_file`` (string, required)
   Path to the YANG schema file, readable by the server process.

``features`` (array of strings, optional)
   Features to enable. ``["*"]`` enables all of them.

``search_dirs`` (string, optional)
   Colon-separated directories searched for imported modules.

**Result**

``ok`` (boolean), ``yang_file`` (string).

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 50,
       "method": "tools/call",
       "params": {
           "name": "sr_module_install",
           "arguments": {
               "yang_file": "/usr/share/yang/modules/ietf-interfaces@2018-02-20.yang",
               "features": ["if-mib"],
               "search_dirs": "/usr/share/yang/modules"
           }
       }
   }

.. note::

   ``features`` is an array, where earlier drafts used a comma-separated
   string. ``search_dirs`` stays a single string because that is what sysrepo
   takes, colon-separated as a shell path list.

sr_module_uninstall
~~~~~~~~~~~~~~~~~~~

*Status: implemented.* Wraps ``sr_remove_module()``.

**Arguments**

``module`` (string, required)
   Name of the module to remove.

``force`` (boolean, optional)
   Remove it even if other modules import it. Default false.

**Result**

``removed`` (boolean), ``module`` (string).

sr_list_modules
~~~~~~~~~~~~~~~

*Status: implemented.* Enumerates the modules in the libyang context. This is
the first tool an agent should call to explore what modules and XPath
resources are available before invoking any other tool: it returns every
implemented module with all of its root data nodes, RPC, actions and
notifications, so the agent knows exactly which ``/module:name`` paths it can
address.

**Arguments**

``implemented_only`` (boolean, optional)
   Skip modules present only as imports. Default true.

**Result**

``modules`` (array of objects)
   Each with ``name``, ``revision``, ``namespace``, ``prefix`` and
   ``implemented``. Each object also includes ``features``, an array of
   enabled YANG feature names (empty when none are enabled or the module is
   not implemented).

``count`` (integer)

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 52,
       "result": {
           "modules": [
               {
                   "name": "oven",
                   "revision": "2018-01-19",
                   "namespace": "urn:sysrepo:oven",
                   "prefix": "ov",
                   "implemented": true,
                   "features": []
               }
           ],
           "count": 1
       }
   }

System tools
------------

get_status
~~~~~~~~~~

*Status: implemented.* Server health and session counters.

**Arguments**

``verbose`` (boolean, optional)
   Include the list of live sessions. Default false.

**Result**

``version`` (string), ``uptime_seconds`` (integer), ``active_sessions``
(integer), ``max_sessions`` (integer), ``session_ttl_seconds`` (integer), and
``sessions`` (array, only when ``verbose`` is true).

Each entry of ``sessions`` carries ``session_id``, ``created``,
``idle_seconds``, ``subscriptions``, ``pending_notifications`` and
``current``, the last marking the session the request itself is using.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 60,
       "result": {
           "version": "0.1.0",
           "uptime_seconds": 3600,
           "active_sessions": 2,
           "max_sessions": 64,
           "session_ttl_seconds": 1800
       }
   }

.. note::

   ``get_status`` reports server health only. The same information, plus the
   session list, is available as YANG data under
   ``/sysrepo-mcp:server-state`` through ``sr_get_operational``; this tool
   exists so that a liveness probe does not need the datastore to be readable.

get_schema
~~~~~~~~~~

*Status: implemented.* Explore compiled YANG schemas. With no arguments, the
tool returns every implemented module. Pass ``xpath`` to narrow the response
to one node and its descendants. ``xpath: "/"`` has the same meaning as an
omitted path.

**Arguments**

``xpath`` (string, optional)
   Absolute schema XPath. Use the module **name** on its first segment, not
   the YANG ``prefix`` statement. For the example ``oven`` module,
   ``sr_list_modules`` reports name ``oven`` and YANG prefix ``ov``; the
   schema path is ``/oven:oven/temperature``. Omit ``xpath`` or pass ``/`` to
   inspect every implemented module.

``max_depth`` (integer, optional, default 0)
   Number of child levels to include below the selected node. Zero traverses
   to the configured hard schema-depth limit; positive values are capped by
   that same limit. For progressive exploration without a source YANG file,
   first call ``get_schema`` with ``max_depth: 1`` and no ``xpath``. Read a
   returned node's full ``xpath`` and pass it back to inspect that subtree at
   greater depth. This avoids the much larger unlimited response and avoids
   guessing a root path.

**Result**

``modules`` (object)
   Map keyed by module name. Each value has ``namespace``, ``prefix``,
   ``revision`` and ``nodes``. ``nodes`` maps root node names to recursive
   node objects. Each node includes its full ``xpath``, ``type``, ``config``,
   ``mandatory``, ``module``, ``namespace``, ``must`` and ``when`` fields.
   Nodes also include applicable compiled details: descriptions and
   references; leaf types, ranges, patterns, values and defaults; list keys
   and cardinality; presence containers; and choice/case and augment data.
   RPC and action ``input``/``output`` nodes are traversable, and the paths
   reported for their data leaves can be passed to the RPC/action tools.

``count`` (integer)
   Number of modules in the response (one for an XPath-specific request).

The hierarchy is the sole node representation; there is no duplicate flat
``nodes`` array. ``base_type`` reports the built-in type resolved by libyang,
not a typedef name.

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 70,
       "method": "tools/call",
       "params": {
           "name": "get_schema",
           "arguments": {"xpath": "/oven:oven", "max_depth": 2}
       }
   }

The result contains ``modules.oven.nodes.oven`` with a recursive ``children``
object. Every node carries its absolute XPath; for example the temperature
leaf is ``/oven:oven/temperature`` and includes ``base_type: "uint8"`` and
``range: ["0..250"]``.

Errors
------

A failure is reported as a JSON-RPC ``error`` object:

.. code-block:: json

   {
       "jsonrpc": "2.0",
       "id": 12,
       "error": {
           "code": -32602,
           "message": "Invalid params",
           "data": {
               "detail": "xpath is required",
               "tool": "sr_get_config"
           }
       }
   }

Codes
~~~~~

The standard JSON-RPC 2.0 codes:

.. list-table::
   :header-rows: 1
   :widths: 14 24 62

   * - Code
     - Message
     - Meaning
   * - ``-32700``
     - Parse error
     - The body is not valid JSON.
   * - ``-32600``
     - Invalid request
     - Valid JSON, but not a JSON-RPC 2.0 request.
   * - ``-32601``
     - Method not found
     - Unknown method, or unknown tool name in ``tools/call``.
   * - ``-32602``
     - Invalid params
     - Missing, ill-typed or mutually exclusive arguments.
   * - ``-32603``
     - Internal error
     - Unexpected server-side failure.

The implementation-defined range, ``-32000`` to ``-32099``, as defined in
``include/sysrepo/mcp/utilities.h`` (``MCP_ERR_*``):

.. list-table::
   :header-rows: 1
   :widths: 14 24 62

   * - Code
     - Message
     - Meaning
   * - ``-32000``
     - Server error
     - Generic failure with no more precise code (e.g. the maximum number of
       concurrent sessions is reached, or a sysrepo callback failed).
   * - ``-32001``
     - Not found
     - Unknown module, or an XPath that no schema node matches.
   * - ``-32002``
     - Validation failed
     - The edit was rejected by the YANG schema or by sysrepo validation.
   * - ``-32003``
     - Access denied
     - NACM refused the operation, or the tool is blocked outright (module
       install/uninstall, when authentication is on).
   * - ``-32004``
     - Datastore locked
     - Another sysrepo client holds a conflicting lock.
   * - ``-32005``
     - Timeout
     - An operational subscriber or an RPC handler did not answer in time.
   * - ``-32006``
     - Not supported
     - A valid request the server does not implement.
   * - ``-32007``
     - *(unused)*
     - Reserved; not currently assigned to any condition.
   * - ``-32008``
     - Session required
     - The tool keeps state between requests. Call ``initialize`` and send
       the ``Mcp-Session-Id`` header it returns.

.. warning::

   A missing or unknown API key is **not** reported with a code from this
   table: it currently reuses ``-32603`` (Internal error) with the message
   "Unauthorized" and HTTP 401 (``method_initialize()`` and ``serve()`` in
   ``transport.c``). This conflates a client-caused condition with the code
   meant for unexpected server-side failure; choosing and documenting a
   replacement is tracked as P1.2 in :doc:`todo`.

.. note::

   Codes ``-32000`` to ``-32006`` and ``-32008`` are specific to this server.
   Earlier drafts mapped every sysrepo failure to ``-32603``, which loses the
   distinction between "your request is wrong", worth retrying differently,
   and "the server is broken", not worth retrying at all. That distinction
   matters for an agent, which decides what to do next from the error alone.

sysrepo error mapping
~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 36 20 44

   * - sysrepo code
     - JSON-RPC code
     - HTTP status
   * - ``SR_ERR_OK``
     - none
     - 200
   * - ``SR_ERR_NOT_FOUND``
     - ``-32001``
     - 200
   * - ``SR_ERR_INVAL_ARG``
     - ``-32002``
     - 200
   * - ``SR_ERR_LY``
     - ``-32002``
     - 200
   * - ``SR_ERR_VALIDATION_FAILED``, ``SR_ERR_EXISTS``
     - ``-32002``
     - 200
   * - ``SR_ERR_UNAUTHORIZED``
     - ``-32003``
     - 200
   * - ``SR_ERR_LOCKED``
     - ``-32004``
     - 200
   * - ``SR_ERR_TIME_OUT``
     - ``-32005``
     - 200
   * - ``SR_ERR_UNSUPPORTED``
     - ``-32006``
     - 200
   * - ``SR_ERR_OPERATION_FAILED``, ``SR_ERR_CALLBACK_FAILED``
     - ``-32000``
     - 200
   * - ``SR_ERR_NO_MEMORY``, ``SR_ERR_INTERNAL``, ``SR_ERR_SYS``, and any
       other code with no entry above
     - ``-32603``
     - 200

.. note::

   ``SR_ERR_LY`` is mapped to a client error on purpose. It always comes from
   something the client supplied, an XPath libyang could not compile or data
   that does not match the schema, and reporting it as an internal failure
   would tell the agent to give up when it should fix its request.

   ``SR_ERR_INVAL_ARG`` and ``SR_ERR_LY`` are both mapped to ``-32002``
   (validation) by default in ``mcp_code_from_sr()``, but
   ``mcp_err_from_session()`` refines this from the sysrepo error message
   afterwards: a message containing "cannot resolve" becomes ``-32001`` (not
   found), and one containing "Expected" or "Unexpected" becomes ``-32602``
   (invalid params) rather than staying a data-validation error.

The ``data.sysrepo`` member of the error object carries the original code name
and the message returned by ``sr_session_get_error()``, so nothing upstream is
lost by the mapping.

HTTP status codes
~~~~~~~~~~~~~~~~~

A JSON-RPC error is still an HTTP 200: the request was delivered and answered.
Non-200 statuses are reserved for failures of the transport layer itself.

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Status
     - Condition
   * - ``200``
     - A JSON-RPC response, whether it holds ``result`` or ``error``.
   * - ``202``
     - A JSON-RPC notification was accepted. Empty body.
   * - ``204``
     - A session was terminated by ``DELETE``. Empty body.
   * - ``400``
     - Malformed HTTP request, or a missing or empty body.
   * - ``404``
     - Unknown or expired ``Mcp-Session-Id``, or ``DELETE`` without one. The
       client re-initializes.
   * - ``405``
     - A method other than POST or DELETE. Carries ``Allow: POST, DELETE``.
   * - ``413``
     - Body larger than the accepted maximum, currently 1 MiB.
   * - ``415``
     - ``Content-Type`` is not ``application/json``.
   * - ``503``
     - Maximum concurrent sessions reached, or sysrepo unreachable.
