.. Copyright (C) 2026 Loic JOURDHEUIL SELLIN <46419549+Geneo-5@users.noreply.github.com>

Roadmap
=======

This appendix is the single source of truth for the state of the project. It
tracks what works, what does not, and in which order the remaining work should
be done.

.. note::

   Keep this page and the reality of the tree in sync. A roadmap that claims
   more than the code delivers is worse than no roadmap at all: it is what
   sends a reader looking for a feature that was never written.

Where the project stands
------------------------

Done
~~~~

- Project skeleton: ``Makefile``, ``ebuild.mk``, ``config.in``, layout.
- Docker build environment: every dependency compiled from ``extern/`` and
  installed under ``/usr/local``.
- ``docker/Makefile`` targets to download, build and run.
- ``scripts/build-docker.sh`` to drive the whole thing from the host.
- Kconfig options in ``config.in``, restricted to build-time settings.
- YANG module ``yang/sysrepo-mcp.yang``: API key list and server state.
- Sphinx documentation: installation, architecture, API reference, license.
- Test suite: 260-odd tests over HTTP through lighttpd, against the upstream
  oven plugin.
- FastCGI transport, JSON-RPC framing and the HTTP status contract.
- MCP lifecycle: ``initialize``, ``notifications/initialized``,
  ``tools/list``, ``ping``, and the ``content`` result envelope.
- Sessions: ``Mcp-Session-Id``, idle expiry, maximum count, ``DELETE``.
- Datastore tools: ``sr_get_config``, ``sr_edit_config``,
  ``sr_delete_config``, ``sr_get_operational``.
- Operation tools: ``sr_execute_rpc``, ``sr_action``.
- Notification tools: ``sr_notif_subscribe``, ``sr_notif_unsubscribe``,
  ``sr_notif_list_subscriptions``, ``sr_notif_poll``, ``sr_notif_send``.
- Module tools: ``sr_list_modules``, ``sr_module_install``,
  ``sr_module_uninstall``.
- Introspection: ``get_status``, ``get_tree``, ``get_help``.
- ``SR_ERR_*`` mapped onto distinct JSON-RPC codes.
- Source file split: ``main.c`` (FastCGI entry point, tool catalogue),
  ``config.c`` (Kconfig parsing, sysrepo_open/close), ``sessions.c`` (session
  CRUD, notification queue, subscription management), ``transport.c`` (HTTP/RPC
  plumbing, MCP methods, request dispatch), ``utilities.c`` (cross-cutting
  helpers: ``tree_to_json``, argument extraction, ``tool_find``), and one
  source + header per functional area (config_tools, operational, rpc,
  notifications, modules, schema, status).

In progress
~~~~~~~~~~~

- ``get_help`` reports the node type, base type, units, enumeration values,
  description and flags, ranges (as string arrays), patterns and default values.
- ``sr_list_modules`` does not report enabled features.
- ``sr_list_modules`` does not report enabled features.

Not started
~~~~~~~~~~~

- Authentication and NACM.
- elog integration.
- A shared session store, which is what ``max-procs = 1`` is waiting on.

Milestone 1: a shared session store
-----------------------------------

The largest open item, and the one that caps throughput.

Sessions, their notification subscriptions and their notification queues live
in the FastCGI process that created them. The deployment is therefore pinned
to ``max-procs = 1``: with more workers, consecutive requests from one agent
land in different processes and the session is not found.

- Move the session registry out of the process. The natural home is
  ``/sysrepo-mcp:server-state/session`` in the operational datastore, which
  gives sharing, expiry and introspection at once.
- Decide what happens to subscriptions. A sysrepo subscription belongs to the
  process that created it, so sharing sessions means either one worker
  receiving events for the others, or moving the queue into the datastore too
  and letting any worker drain it.
- Move ``MAX_SESSIONS``, ``SESSION_TTL`` and ``NOTIF_QUEUE_SIZE`` from
  ``config.in`` to the YANG module once the store is no longer a fixed array.
- Raise ``max-procs`` in ``docker/lighttpd.conf`` and in the documentation
  only after a concurrency test passes.

Milestone 2: access control
---------------------------

.. warning::

   Until this milestone is complete, an agent has every right of the system
   user running the server. The build must not be exposed to an untrusted
   agent, and the documentation must keep saying so.

- Extract the credential from ``HTTP_AUTHORIZATION``, or from the cookie.
- Look the key up in ``/sysrepo-mcp:api-key`` and resolve the NACM user.
- Store keys hashed, and compare in constant time.
- ``sr_nacm_init()`` at startup, ``sr_nacm_set_user()`` per request,
  ``sr_nacm_check_operation()`` before an RPC, ``sr_nacm_destroy()`` at exit.
- Apply the module allow-list and the write protection of ``config.in`` before
  calling sysrepo.
- Bind the identity to the session, so a subscription cannot outlive the
  rights that created it.
- Deny ``sr_module_install`` and ``sr_module_uninstall`` by default. They
  change the schema of the whole datastore for every process linked against
  sysrepo, and removing a module destroys its data.
- Log every configuration change with the identity that caused it.

Milestone 3: logging and packaging
----------------------------------

- Replace ``fprintf(stderr, ...)`` with elog: syslog, file and console back
  ends, and the elog command-line parser.
- Honour the ``SYSREPO_MCP_SERVER_LOG_*`` options, which are declared in
  ``config.in`` and read by nobody.
- Ship a systemd unit and an example lighttpd fragment.
- Ship the YANG module and a script to install it into sysrepo.

Smaller items
-------------

- ``initialize`` ignores the ``protocolVersion`` the client sends. An
  unsupported revision should be refused explicitly rather than answered as
  if it were understood.
- ``get_help`` reports the base type, not the typedef name: the latter is not
  recoverable from the compiled schema. Reaching it means walking the parsed
  schema instead.
- ``sr_list_modules`` does not report enabled features, for the same reason.
- ``get_tree`` always reports an empty ``imports`` array.
- ``sr_edit_config`` does not report how many nodes it changed.
- Run a real MCP client against the server, not only the test suite.

Build system
------------

All of the following were fixed; they are kept here as a record of what the
build used to get wrong.

- ``ebuild.mk``: ``libyang`` was missing from the ``pkg-config`` call while
  the code called ``lyd_*`` and ``lys_*``.
- ``ebuild.mk``: the ``pkg-config`` failure was silenced with ``2>/dev/null``,
  turning a missing ``.pc`` file into a confusing link error.
- ``docker/Dockerfile``: a mid-file
  ``ENV PKG_CONFIG_PATH=/usr/lib/pkgconfig`` hid
  ``/usr/local/lib/pkgconfig``, where libyang and sysrepo install their
  ``.pc`` files.
- ``docker/Makefile``: ``$(id -u)`` was expanded by make, not by the shell,
  and yielded an empty ``-u ":"``.
- ``.github/workflows/c-cpp.yml``: the script path was wrong, and the workflow
  had never run to completion.
- ``libconfig-dev`` and the stale ``docker/config.cfg`` were dropped: the
  project does not use libconfig.

Source layout
-------------

``main.c`` was split into 10 source files + 10 headers.

``src/main.c``
   FastCGI entry point (``FCGX_Accept_r`` loop, signal handling,
   ``sysrepo_open/close``), and the global ``tools[]`` catalogue.

``src/sessions.c``
   Session data structures, CRUD, notification queue, subscription management.

``src/transport.c``
   HTTP/RPC plumbing (``http_send``, ``rpc_send``), MCP methods
   (``initialize``, ``tools/list``, ``tools/call``), request dispatch,
   FastCGI ``serve()``.

``src/utilities.c``
   Cross-cutting helpers: ``tree_to_json``, ``nodetype_to_json``, argument
   extraction (``arg_string``, ``arg_object``, ``arg_int``, ``arg_bool``,
   ``arg_xpath``, ``arg_datastore``), ``xpath_wellformed``, ``tool_find``,
   ``tool_content``.

``src/config_tools.c``
   ``sr_get_config``, ``sr_edit_config``, ``sr_delete_config``.

``src/operational.c``
   ``sr_get_operational``.

``src/rpc.c``
   ``sr_execute_rpc``, ``sr_action``, ``rpc_common``.

``src/notifications.c``
   All notification tools: subscribe, unsubscribe, list_subscriptions,
   poll, send.

``src/modules.c``
   ``sr_list_modules``, ``sr_module_install``, ``sr_module_uninstall``.

``src/schema.c``
   ``get_tree``, ``get_help``, ``schema_node_to_json``, ``basetype_name``.

``src/status.c``
   ``get_status``.

Headers live under ``include/sysrepo/mcp/`` — one per source file, forward
declarations only. Every module includes ``utilities.h`` for shared helpers.

Testing
-------

The suite in ``tests/`` drives the server the way a client does: HTTP to
lighttpd on port 80, forwarded over FastCGI, against the upstream oven plugin
from ``extern/sysrepo/examples/plugin/oven.c``. It runs in a private sysrepo
repository and shared-memory namespace, so it never touches the system
repository.

Layout:

``tests/conftest.py``
   Fixtures: the private repository, module installation, lighttpd in front of
   the server, and the oven plugin. Each layer skips with an explicit reason,
   so a partial environment still runs the tests it can.

``tests/test_transport.py``
   HTTP and JSON-RPC framing: status codes, headers, malformed bodies,
   notifications, and the guarantee that every request gets an answer.

``tests/test_protocol.py``
   MCP lifecycle, the tool catalogue and its input schemas, the ``content``
   envelope, ``get_status``.

``tests/test_oven.py``
   The oven end to end: configuration reads and writes, refused writes,
   operational state from the plugin, the RPCs, and one full cooking session.

``tests/test_sessions.py``
   Sessions and notifications: the ``Mcp-Session-Id`` lifecycle, isolation
   between sessions, subscribing, polling, draining, and the full watch-a-
   device flow against ``oven-ready``.

``tests/test_schema.py``
   ``get_tree`` and ``get_help`` against every node of the oven module.

``tests/test_errors.py``
   Argument validation, the ``SR_ERR_*`` mapping, module listing, and the
   project's own YANG module, which supplies the list, key predicate and
   empty-match cases that the oven model has none of.

Remaining:

- Run the suite in CI against a matrix of libyang and sysrepo revisions; the
  two are pinned together and bumping them is where the introspection code
  will break first.
- Run the server under valgrind; the JSON reference counting and the
  notification queue deserve it.
- Add a concurrency test, which only becomes meaningful once ``max-procs``
  can be raised above 1.
- Exercise notification replay, which needs replay support enabled on a
  module.
- Exercise queue overflow and the ``dropped`` counter.
- Cover ``sr_module_install`` and ``sr_module_uninstall`` beyond argument
  validation; a test that installs a module changes the shared repository and
  needs its own throwaway one.
- Cover authentication and NACM once they exist. Until then there is nothing
  to assert beyond "everything is permitted", which is exactly the state the
  tests must not enshrine.

Not planned
-----------

Decisions already taken, recorded here so they are not re-litigated:

**SSE and server-initiated messages**
   The server answers POSTs with a single JSON object. This is permitted by
   the MCP Streamable HTTP binding, and it rules out MCP sampling and
   elicitation. Sysrepo notifications are not lost, but they are queued and
   polled rather than pushed.

**WebSocket transport**
   Not an MCP binding.

**A direct HTTP listener**
   HTTP, TLS and rate limiting belong to the reverse proxy.

**libconfig**
   Build-time configuration is Kconfig, runtime configuration is YANG. There
   is no third configuration file.

Later
-----

- OAuth2 or JWT credentials, which the MCP authorization specification builds
  on.
- Transactions spanning several tool calls, with explicit commit and rollback.
- A dry-run mode, validating an edit without committing it.
- A diff tool between two datastores.
- Horizontal scaling behind a load balancer, once sessions are shared.
- Metrics export.

Contributing
------------

1. Pick an item from the earliest incomplete milestone.
2. Read :doc:`architecture` for the technical context.
3. Follow the existing style, and build in the container.
4. Add a test that fails without the change.
5. Update this page and the affected documentation in the same commit.
