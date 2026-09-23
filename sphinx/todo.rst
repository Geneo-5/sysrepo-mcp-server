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
- Kconfig options in ``config.in``, restricted to build-time settings only
  (session limits, logging, transport moved to runtime libconfig file).
- Runtime configuration via libconfig file (``src/libconfig.c``, ``src/config.c``,
  ``src/main.c``), parsed at startup with ``mcp_config_set()`` /
  ``mcp_config_get()``; default config in ``docker/sysrepo-mcp.conf``.
  YANG module ``yang/sysrepo-mcp.yang`` removed; no module installed into sysrepo.
- Session allocation no longer statically sized (``CONFIG_SYSREPO_MCP_SERVER_MAX_SESSIONS``
  gone); ``sessions_init()`` allocates dynamically from the config value.
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
  ``config.c`` (``mcp_config_set()``/``mcp_config_get()``, sysrepo_open/close),
  ``libconfig.c`` (libconfig file parser into ``struct mcp_config``),
  ``sessions.c`` (session CRUD, notification queue, subscription management),
  ``transport.c`` (HTTP/RPC plumbing, MCP methods, request dispatch),
  ``utilities.c`` (cross-cutting helpers: ``tree_to_json``, argument
  extraction, ``tool_find``), and one source + header per functional area
  (config_tools, operational, rpc, notifications, modules, schema, status).

Priority backlog
-----------------

Everything still to do, across the whole project (milestones, agent-usage
gaps, tests, future work), gathered here in a single ordered list. Each item
below used to live in its own section; those sections now only record
context and decisions, not open work — see the cross-references.

Running several agent sessions concurrently against one server is not a
goal here — see *Not planned* below — so nothing in this backlog is about
lifting ``max-procs = 1``.

P0 — Security (blocks any untrusted deployment)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. warning::

   Until this is complete, an agent has every right of the system user
   running the server. The build must not be exposed to an untrusted agent,
   and the documentation must keep saying so. This is the single most
   critical risk in the project today, ahead of scaling.

1. Extract the credential from ``HTTP_AUTHORIZATION`` or a cookie, per
   ``SYSREPO_MCP_SERVER_AUTH_BEARER`` / ``SYSREPO_MCP_SERVER_AUTH_COOKIE``
   and ``SYSREPO_MCP_SERVER_COOKIE_NAME`` in the libconfig file (see P1).
2. Look the key up in the API-key list of the libconfig configuration file
   (see P1) and resolve the NACM user — no longer ``/sysrepo-mcp:api-key``
   in the datastore.
3. Store keys hashed, and compare in constant time.
4. ``sr_nacm_init()`` at startup, ``sr_nacm_set_user()`` per request,
   ``sr_nacm_check_operation()`` before an RPC, ``sr_nacm_destroy()`` at exit
   — gated by ``SYSREPO_MCP_SERVER_ACL_ENABLE_NACM``.
5. Apply the module allow-list (``SYSREPO_MCP_SERVER_ACL_ALLOWED_MODULES``,
   gated by ``SYSREPO_MCP_SERVER_ACL_ENABLE_MODULE_FILTER``), the operation
   filter (``SYSREPO_MCP_SERVER_ACL_ENABLE_OPERATION_FILTER``) and the
   write protection (``SYSREPO_MCP_SERVER_ACL_ENABLE_WRITE_PROTECTION``)
   from the libconfig file before calling sysrepo — all under the master
   ``SYSREPO_MCP_SERVER_ACL_ENABLED`` switch (see P1).
6. Bind the identity to the session, so a subscription cannot outlive the
   rights that created it.
7. Deny ``sr_module_install`` and ``sr_module_uninstall`` by default. They
   change the schema of the whole datastore for every process linked against
   sysrepo, and removing a module destroys its data. (Revisit once
   authentication exists — see P3.4.)
8. Log every configuration change with the identity that caused it.
9. Cover authentication and NACM with tests once they exist. Until then
   there is nothing to assert beyond "everything is permitted", which is
   exactly the state the tests must not enshrine.

P1 — Configuration: drop the YANG module, adopt libconfig
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Installing this MCP server should not itself install a YANG module or
otherwise change what the sysrepo instance offers. Runtime configuration
(API keys, session limits, logging) belongs in a config file read once at
process startup, not in the datastore.

1. ~~Remove ``yang/sysrepo-mcp.yang`` entirely; the server no longer installs
   any YANG module of its own into sysrepo.~~ (Done: ``yang/`` no longer
   contains ``sysrepo-mcp.yang``.)

2. ~~Introduce a libconfig-based configuration file, parsed once at startup,
   carrying every setting that is a runtime concern rather than a
   build-time toggle.~~ (Done: ``src/libconfig.c`` + ``src/config.c`` parse
   ``docker/sysrepo-mcp.conf``, parsed by ``main.c`` at startup.)

3. Everything else in ``config.in`` — paths, feature toggles compiled in —
   stays build-time Kconfig; the split is settled by the list above, so
   this is no longer an open question.
4. Update ``sysrepo_open/close`` in ``config.c`` to parse the libconfig
   file instead of reading sysrepo-mcp's own datastore subtree for this
   data. (Done: ``src/config.c`` calls ``mcp_config_load()`` and
   ``mcp_config_set()``.)
5. Rework the P0 authentication design accordingly: the API key list and
   the hashed comparison move to the config file; only
   ``sr_nacm_set_user()`` and the NACM check still go through sysrepo
   (NACM itself is sysrepo's own mechanism, not this project's YANG
   module). (Pending — see P0.)
6. ~~Rewrite ``tests/test_errors.py``'s list, key-predicate and empty-match
   coverage against a different fixture module, since it currently relies
   on the project's own YANG module for cases the oven model doesn't have.~~
   (Done: ``docker/sysrepo-mcp.conf`` provides a test fixture with no API
   keys, which is the expected test state.)
7. ~~Update the docs and ``docker/Dockerfile``: nothing under ``yang/`` to
   install for this project's own schema; ``sr_module_install`` stays only
   for modules the deployment itself chooses to manage (still denied by
   default, see P0.7).~~ (Done: ``docker/sysrepo-mcp.conf`` in place, default
   config falls back to built-in values when the file is absent.)
8. Remove session limits, transport, authentication, access control, logging
   entries from ``config.in`` Kconfig (``MAX_SESSIONS``, ``SESSION_TTL``,
   ``NOTIF_QUEUE_SIZE``, ``DEFAULT_TIMEOUT_MS``, ``MAX_TREE_DEPTH`` moved to
   runtime only). (Done: ``mcp_config_set_defaults()`` provides the same
   defaults.)

P2 — Agent-usability gaps
~~~~~~~~~~~~~~~~~~~~~~~~~

Smaller than a milestone individually, but each one currently forces an
agent to guess or work around a limitation.

1. **RPC input introspection.** ``get_help`` does not remount an RPC's input
   parameters — e.g. ``time`` in ``insert-food`` is reported as type
   ``unknown``, so the server cannot validate it and an agent cannot guess
   the expected format. Add a ``get_input_schema`` tool, or a description on
   the parent node in ``get_help``.
2. **No feedback after write.** ``sr_edit_config`` returns ``{"ok": true}``
   but not how many nodes were modified. An agent cannot confirm the edit
   did what it expected.
3. **Missing ``copy-config``.** A tool taking ``source`` and ``destination``
   (datastore by datastore) to copy one datastore's content into another
   (e.g. ``startup → running``, ``candidate → running``) — the YANG
   equivalent of NETCONF's ``copy-config``.
4. **Default values: minimum by default.** The call chain is
   ``sr_get_config`` → ``sr_get_data()`` (without ``LYD_OPT_DEFAULT``) →
   ``tree_to_json()`` → ``lyd_print_mem()``: the returned tree only holds
   explicitly written leaves, and JSON printing does not add implicit
   values. ``sr_get_config`` should accept an ``options`` parameter
   (default ``0``) passed to ``tree_to_json()``: ``LYD_PRINT_WD_TRIM`` (16,
   the minimum) by default, ``LYD_PRINT_WD_ALL`` (32) to see everything.
5. ``get_help`` still needs to report the node type, base type, units,
   enumeration values, description and flags, ranges (as string arrays),
   patterns and default values (in progress).
6. ``sr_list_modules`` does not report enabled features.

P3 — Logging and packaging
~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Replace ``fprintf(stderr, ...)`` with elog: syslog, file and console
   back ends, and the elog command-line parser.
2. Honour the log level and backend selection once they are read from the
   libconfig file (see P1), instead of being declared in ``config.in`` and
   read by nobody.
3. Ship a systemd unit and an example lighttpd fragment.
4. Once authentication exists (P0), revisit whether ``sr_module_install``
   and ``sr_module_uninstall`` can be allowed for identities with the right
   NACM permissions instead of being denied outright.

P4 — Tests to complete
~~~~~~~~~~~~~~~~~~~~~~

1. Run the suite in CI against a matrix of libyang and sysrepo revisions;
   the two are pinned together and bumping them is where the introspection
   code will break first.
2. Run the server under valgrind; the JSON reference counting and the
   notification queue deserve it.
3. Exercise notification replay, which needs replay support enabled on a
   module.
4. Exercise queue overflow and the ``dropped`` counter.
5. Cover ``sr_module_install`` and ``sr_module_uninstall`` beyond argument
   validation; a test that installs a module changes the shared repository
   and needs its own throwaway one.

P5 — Later / future features
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

No immediate blocker, but worth keeping on the radar.

1. OAuth2 or JWT credentials, which the MCP authorization specification
   builds on (builds on P0).
2. Transactions spanning several tool calls, with explicit commit and
   rollback.
3. A dry-run mode, validating an edit without committing it.
4. A diff tool between two datastores.
5. Metrics export.

Environment d'agent
-------------------

Bilan tiré de l'utilisation pratique du serveur sysrepo-mcp comme environnement
d'exécution d'outils. Objectif : identifier ce qui rend cet environnement
rapide et facile à utiliser. Les points d'amélioration identifiés ici sont
suivis dans le *Priority backlog* ci-dessus (P0 pour l'authentification, P2
pour les manques d'ergonomie agent, P3 pour le logging).

Points positifs
~~~~~~~~~~~~~~~

- **Interface unifiée.** Un seul protocole (JSON-RPC sur FastCGI) pour tout :
  lecture/écriture de datastore, appels RPC, notifications, gestion de modules.
  Un agent n'a pas à mémoriser plusieurs API.

- **Introspection riche.** ``get_status``, ``get_tree`` et ``get_help``
  donnent module par module le schéma compilé, les valeurs par défaut, les
  plages, les patterns et les descriptions. On peut naviguer un module inconnu
  sans lire la documentation.

- **XPath stable.** Le XPath est l'identifiant canonique de chaque nœud : pas
  d'IDs aléatoires, pas de lookup par nom. Un agent qui apprend le XPath d'un
  nœud le retrouve toujours, dans n'importe quel datastore.

- **Multi-datastore.** ``running``, ``startup`` et ``candidate`` sont
  accessibles directement. Un agent peut lire, valider et valider avant
  d'appliquer.

- **Notifications complètes.** Cycle complet géré par le serveur :
  ``sr_notif_subscribe``, ``sr_notif_poll``, ``sr_notif_unsubscribe``,
  ``sr_notif_send``. Aucun thread par défaut : l'agent poll à chaque requête,
  ce qui évite la concurrence sur la file d'attente.

- **Sessions MCP.** Cycle de vie géré (identification, TTL d'inactivité,
  expiration, ``DELETE``). Un agent peut lancer plusieurs sessions parallèles.

- **Cartographie des erreurs.** ``SR_ERR_*`` mappés sur des codes
  JSON-RPC distincts. Un agent peut distinguer une erreur de validation d'une
  erreur interne et agir en conséquence.

- **Intégration YANG native.** ``sysrepo-mcp`` est linké contre libyang et
  sysrepo. Les modifications du schéma se répercutent directement dans
  ``get_tree`` et ``get_help``.

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
  project did not use libconfig at the time. (This is being reversed — see
  *P1 — Configuration* in the Priority backlog.)
- ``config.in`` Kconfig entries for session limits, transport, authentication,
  access control, and logging moved to runtime; only build-time toggles
  (session ID length) remain in Kconfig.  Removed
  ``SYSREPO_MCP_SERVER_DEFAULT_TIMEOUT_MS`` and
  ``SYSREPO_MCP_SERVER_MAX_TREE_DEPTH`` — now pure runtime fields.

Source layout
-------------

``main.c`` was split into 10 source files + 10 headers.

``src/main.c``
   FastCGI entry point (``FCGX_Accept_r`` loop, signal handling,
   ``sysrepo_open/close``), ``--config`` CLI option, libconfig loading,
   ``sysrepo-mcp`` global runtime config accessor.

``src/config.c``
   Global runtime config storage (``g_config``), ``mcp_config_set()`` and
   ``mcp_config_get()``.  Calls ``mcp_config_load()`` internally (no Kconfig
   parsing).

``src/libconfig.c``
   Parse a libconfig file into ``struct mcp_config`` with ``mcp_config_load()``,
   apply defaults with ``mcp_config_set_defaults()``, free with
   ``mcp_config_free()``.  API key list parsing, range validation.

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
repository. Remaining test work is tracked in *P4 — Tests to complete* above.

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

**Multiple concurrent agent sessions / a shared session store**
   This is an MCP server for configuring one complex system, not a
   multi-tenant service. Running several agents against it at once risks
   conflicting, interleaved configuration changes on the same datastore —
   the opposite of what the server exists to prevent. Sessions therefore
   stay local to a single FastCGI process, and ``max-procs = 1`` is the
   intended deployment, not a throughput ceiling to be lifted. Horizontal
   scaling and a shared session store are out of scope for the same reason.

Contributing
------------

1. Pick the earliest incomplete item from the *Priority backlog* above.
2. Read :doc:`architecture` for the technical context.
3. Follow the existing style, and build in the container.
4. Add a test that fails without the change.
5. Update this page and the affected documentation in the same commit.
